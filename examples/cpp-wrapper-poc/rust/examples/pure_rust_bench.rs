// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! Pure-Rust baseline for the verification run: the same google-cloud-storage
//! crate the C++ wrapper uses, but called directly from Rust (no C ABI, no C++).
//! The difference "rust_wrapped - rust_pure" is exactly the cost of the
//! wrapper. Mirrors storage-shared-benchmarking/w1r3/rust:
//!   * appendable upload in 2 MiB `append` chunks (SSB `upload_appendable_data`)
//!   * open      : `open_object().send()`
//!   * random8k  : two-step `send()` + `read_range(8 KiB)` (what C++ SSB and the
//!                 wrapper do) AND single-RPC `send_and_read(8 KiB)` (what SSB
//!                 Rust does), recorded as `random8k` and `random8k_send_and_read`
//!   * read50x2x4: `send()` + 50 concurrent `read_range(2 MiB)` via try_join_all
//! Output CSV has the same columns as the C++ harness (impl = rust_pure).
//!
//!   * w1r3 (JSON): upload SINGLE-SHOT or RESUMABLE (`write_object` with a
//!                  zero-copy `Bytes` slice), READ[0..2] = full `read_object`
//!                  (chunks discarded, as the C++ wrapper does), DELETE.
//!                  Heap bytes are counted by interposing `malloc` exactly like
//!                  cpp/bench/alloc_counter.cc, so numbers are comparable.
//!
//! Usage: pure_rust_bench --bucket=B --workload=open|random8k|read50x2x4|w1r3 \
//!          [--iterations=200] [--object-sizes=100000,2097152] [--out=file.csv]

use bytes::Bytes;
use futures::future::try_join_all;
use google_cloud_storage::client::{Storage, StorageControl};
use google_cloud_storage::model_ext::ReadRange;
use google_cloud_storage::read_object::ReadObjectResponse;
use std::error::Error;
use std::io::Write;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Instant;

type Res<T> = Result<T, Box<dyn Error + Send + Sync>>;

const KIB: u64 = 1024;
const MIB: u64 = 1024 * 1024;
const SIZES: [u64; 3] = [100_000, 2 * MIB, 100_000_000];

// ---- malloc interposition (same method as cpp/bench/alloc_counter.cc) -------
// Counts every heap allocation in the process: Rust (System allocator ->
// malloc), aws-lc (C), everything.
static MALLOC_BYTES: AtomicU64 = AtomicU64::new(0);
unsafe extern "C" {
    fn __libc_malloc(n: usize) -> *mut libc::c_void;
    fn __libc_calloc(n: usize, m: usize) -> *mut libc::c_void;
    fn __libc_realloc(p: *mut libc::c_void, n: usize) -> *mut libc::c_void;
    fn __libc_memalign(a: usize, n: usize) -> *mut libc::c_void;
    fn __libc_free(p: *mut libc::c_void);
}
#[inline]
fn count(n: usize) {
    MALLOC_BYTES.fetch_add(n as u64, Ordering::Relaxed);
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn malloc(n: usize) -> *mut libc::c_void {
    count(n);
    unsafe { __libc_malloc(n) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn calloc(n: usize, m: usize) -> *mut libc::c_void {
    count(n.saturating_mul(m));
    unsafe { __libc_calloc(n, m) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn realloc(p: *mut libc::c_void, n: usize) -> *mut libc::c_void {
    count(n);
    unsafe { __libc_realloc(p, n) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn free(p: *mut libc::c_void) {
    unsafe { __libc_free(p) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn memalign(a: usize, n: usize) -> *mut libc::c_void {
    count(n);
    unsafe { __libc_memalign(a, n) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aligned_alloc(a: usize, n: usize) -> *mut libc::c_void {
    count(n);
    unsafe { __libc_memalign(a, n) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn posix_memalign(out: *mut *mut libc::c_void, a: usize, n: usize) -> libc::c_int {
    count(n);
    let p = unsafe { __libc_memalign(a, n) };
    if p.is_null() {
        return libc::ENOMEM;
    }
    unsafe { *out = p };
    0
}
fn alloc_bytes() -> u64 {
    MALLOC_BYTES.load(Ordering::Relaxed)
}
fn status_kib(key: &str) -> i64 {
    std::fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|s| {
            s.lines()
                .find(|l| l.starts_with(key))
                .and_then(|l| l[key.len()..].trim().trim_end_matches("kB").trim().parse().ok())
        })
        .unwrap_or(-1)
}
fn rss_kib() -> i64 {
    status_kib("VmRSS:")
}
fn peak_rss_kib() -> i64 {
    status_kib("VmHWM:")
}

fn cpu_ms() -> f64 {
    // SAFETY: getrusage writes into a zeroed, correctly sized struct.
    unsafe {
        let mut ru: libc::rusage = std::mem::zeroed();
        libc::getrusage(libc::RUSAGE_SELF, &mut ru);
        let t = |tv: libc::timeval| tv.tv_sec as f64 * 1e3 + tv.tv_usec as f64 / 1e3;
        t(ru.ru_utime) + t(ru.ru_stime)
    }
}

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        // xorshift64*
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545F4914F6CDD1D)
    }
}

async fn drain(mut r: ReadObjectResponse) -> Res<u64> {
    let mut n = 0u64;
    while let Some(chunk) = r.next().await {
        n += chunk?.len() as u64;
    }
    Ok(n)
}

struct Csv(std::fs::File, u64);
impl Csv {
    #[allow(clippy::too_many_arguments)]
    fn row(&mut self, w: &str, i: i64, size: u64, op: &str, ms: f64, cpu: f64, bytes: u64, err: Option<String>) {
        let (ok, e) = match err {
            None => (1, String::new()),
            Some(e) => (0, e.replace([',', '\n'], ";").chars().take(120).collect()),
        };
        // self.1 = heap bytes allocated during the op (set by measure!).
        let (alloc, rss) = (self.1, rss_kib());
        let _ = writeln!(self.0, "rust_pure,{w},{i},{size},{op},{ms:.3},{cpu:.3},{bytes},{ok},{e},{alloc},0,{rss}");
        if ok == 0 {
            eprintln!("  ERROR {w} #{i} {op}: {e}");
        }
    }
    fn proc_row(&mut self, w: &str, op: &str, ms: f64, cpu: f64, alloc: u64, rss: i64) {
        let _ = writeln!(self.0, "rust_pure,{w},-1,0,{op},{ms:.3},{cpu:.3},0,1,,{alloc},0,{rss}");
    }
}

macro_rules! measure {
    ($csv:expr, $w:expr, $i:expr, $size:expr, $op:expr, $body:expr) => {{
        let a0 = alloc_bytes();
        let c0 = cpu_ms();
        let t0 = Instant::now();
        let r: Res<u64> = $body.await;
        let ms = t0.elapsed().as_secs_f64() * 1e3;
        let cpu = cpu_ms() - c0;
        $csv.1 = alloc_bytes() - a0;
        match r {
            Ok(b) => { $csv.row($w, $i as i64, $size, $op, ms, cpu, b, None); true }
            Err(e) => { $csv.row($w, $i as i64, $size, $op, ms, cpu, 0, Some(e.to_string())); false }
        }
    }};
}

#[tokio::main(flavor = "multi_thread")]
async fn main() -> Res<()> {
    let mut bucket = String::new();
    let mut workload = String::from("open");
    let mut iterations = 200usize;
    let mut out = String::new();
    let mut sizes: Vec<u64> = SIZES.to_vec();
    let mut w1r3_bucket = String::new();
    for a in std::env::args().skip(1) {
        let (k, v) = a.split_once('=').unwrap_or((a.as_str(), ""));
        match k {
            "--bucket" => bucket = v.to_string(),
            "--w1r3-bucket" => w1r3_bucket = v.to_string(),
            "--workload" => workload = v.to_string(),
            "--iterations" => iterations = v.parse()?,
            "--object-sizes" => sizes = v.split(',').map(|s| s.parse()).collect::<Result<_, _>>()?,
            "--out" => out = v.to_string(),
            _ => return Err(format!("unknown flag {a}").into()),
        }
    }
    if out.is_empty() {
        out = format!("rust_pure-{workload}.csv");
    }
    if w1r3_bucket.is_empty() {
        w1r3_bucket = bucket.clone();
    }
    let bucket_path = format!("projects/_/buckets/{bucket}");
    let storage = Storage::builder().build().await?;
    let control = StorageControl::builder().build().await?;

    let mut buf = vec![0u8; (200 * MIB).max(*sizes.iter().max().unwrap()) as usize];
    let mut g = Rng(7);
    for c in buf.chunks_mut(8) {
        c.copy_from_slice(&g.next().to_le_bytes()[..c.len()]);
    }
    let data = Bytes::from(buf);

    let mut csv = Csv(std::fs::File::create(&out)?, 0);
    writeln!(csv.0, "impl,workload,iteration,object_size,op,latency_ms,cpu_ms,bytes,ok,error,alloc_bytes,new_bytes,rss_kib")?;
    let run_id = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)?.as_nanos();
    let mut rng = Rng(42);
    let t_all = Instant::now();
    println!("impl=rust_pure bucket={bucket} workload={workload} iterations={iterations}");

    if workload == "w1r3" {
        // SSB sync w1r3 worker over JSON, same steps as the C++ harness.
        let wb = format!("projects/_/buckets/{w1r3_bucket}");
        let (rss_start, cpu0, alloc0) = (rss_kib(), cpu_ms(), alloc_bytes());
        for i in 0..iterations {
            let size = sizes[(rng.next() % sizes.len() as u64) as usize];
            let resumable = rng.next() % 2 == 1;
            let up = if resumable { "RESUMABLE" } else { "SINGLE-SHOT" };
            let name = format!("gcs-rust-poc/ssb/rust_pure/{run_id}/w1r3-{i}");
            let (t0, c0, a0) = (Instant::now(), cpu_ms(), alloc_bytes());
            let ok = measure!(csv, "w1r3", i, size, up, async {
                let threshold = if resumable { 0usize } else { usize::MAX };
                storage
                    .write_object(&wb, &name, data.slice(..size as usize))
                    .with_resumable_upload_threshold(threshold)
                    .send_unbuffered()
                    .await?;
                Res::Ok(size)
            });
            if !ok {
                continue;
            }
            for op in ["READ[0]", "READ[1]", "READ[2]"] {
                measure!(csv, "w1r3", i, size, op, async {
                    let r = storage.read_object(&wb, &name).send().await?;
                    let n = drain(r).await?;
                    if n != size {
                        return Err(format!("short read {n}").into());
                    }
                    Res::Ok(n)
                });
            }
            measure!(csv, "w1r3", i, size, "DELETE", async {
                control.delete_object().set_bucket(&wb).set_object(&name).send().await?;
                Res::Ok(0)
            });
            csv.1 = alloc_bytes() - a0;
            let (ms, cpu) = (t0.elapsed().as_secs_f64() * 1e3, cpu_ms() - c0);
            csv.row("w1r3", i as i64, size, &format!("W1R3D1-{up}"), ms, cpu, 4 * size, None);
        }
        let (rss_end, peak) = (rss_kib(), peak_rss_kib());
        csv.proc_row("w1r3", "RSS_START", 0.0, 0.0, 0, rss_start);
        csv.proc_row("w1r3", "RSS_END", 0.0, 0.0, 0, rss_end);
        csv.proc_row("w1r3", "RSS_PEAK", 0.0, 0.0, 0, peak);
        let total_ms = t_all.elapsed().as_secs_f64() * 1e3;
        csv.proc_row("w1r3", "WORKLOAD_TOTAL", total_ms, cpu_ms() - cpu0, alloc_bytes() - alloc0, rss_end);
        println!(
            "  w1r3: {iterations} iterations in {:.1} s  cpu={:.1} s  alloc={} MiB  rss start/end/peak={}/{}/{} MiB",
            total_ms / 1e3,
            (cpu_ms() - cpu0) / 1e3,
            (alloc_bytes() - alloc0) / MIB,
            rss_start / 1024,
            rss_end / 1024,
            peak / 1024
        );
        return Ok(());
    }

    for i in 0..iterations {
        let size = if workload == "read50x2x4" { 200 * MIB } else { SIZES[(rng.next() % 3) as usize] };
        let offset = rng.next() % (size - 8 * KIB + 1);
        let name = format!("gcs-rust-poc/ssb/rust_pure/{run_id}/{workload}-{i}");

        // APPENDABLE upload, 2 MiB chunks (as SSB Rust).
        let ok = measure!(csv, &workload, i, size, "APPENDABLE", async {
            let mut w = storage.open_appendable_object(&bucket_path, &name).send().await?;
            let slice = data.slice(..size as usize);
            let mut s = 0usize;
            while s < slice.len() {
                let e = (s + (2 * MIB) as usize).min(slice.len());
                w.append(slice.slice(s..e)).await?;
                s = e;
            }
            w.finalize().await?;
            Res::Ok(size)
        });
        if !ok {
            continue;
        }

        match workload.as_str() {
            "open" => {
                measure!(csv, &workload, i, size, "open", async {
                    let d = storage.open_object(&bucket_path, &name).send().await?;
                    drop(d);
                    Res::Ok(0)
                });
            }
            "random8k" => {
                // Alternate the order of the two variants to avoid bias.
                let two_step = || async {
                    let d = storage.open_object(&bucket_path, &name).send().await?;
                    let n = drain(d.read_range(ReadRange::segment(offset, 8 * KIB)).await).await?;
                    Res::Ok(n)
                };
                let single = || async {
                    let (_d, r) = storage
                        .open_object(&bucket_path, &name)
                        .send_and_read(ReadRange::segment(offset, 8 * KIB))
                        .await?;
                    Res::Ok(drain(r).await?)
                };
                if i % 2 == 0 {
                    measure!(csv, &workload, i, size, "random8k", two_step());
                    measure!(csv, &workload, i, size, "random8k_initial_range", single());
                } else {
                    measure!(csv, &workload, i, size, "random8k_initial_range", single());
                    measure!(csv, &workload, i, size, "random8k", two_step());
                }
            }
            "read50x2x4" => {
                measure!(csv, &workload, i, size, "read50x2x4_in_core", async {
                    let d = storage.open_object(&bucket_path, &name).send().await?;
                    let d = &d;
                    let tasks = (0..50u64).map(|j| async move {
                        drain(d.read_range(ReadRange::segment(j * 4 * MIB, 2 * MIB)).await).await
                    });
                    let n: u64 = try_join_all(tasks).await?.into_iter().sum();
                    Res::Ok(n)
                });
            }
            other => return Err(format!("unknown workload {other}").into()),
        }

        measure!(csv, &workload, i, size, "DELETE", async {
            control.delete_object().set_bucket(&bucket_path).set_object(&name).send().await?;
            Res::Ok(0)
        });
    }
    println!("  {workload}: {iterations} iterations in {:.1} s", t_all.elapsed().as_secs_f64());
    Ok(())
}
