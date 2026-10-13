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

// Re-implementation of the four storage-shared-benchmarking (SSB) RAPID
// workloads so the Rust-wrapped C++ client and native google-cloud-cpp run the
// exact same operations. Reference: storage-shared-benchmarking/w1r3/cpp/internal
//
//   w1r3       : upload(size) ; READ[0..2] = full read ; DELETE
//                SSB w1r3_worker.cc. On RAPID the reads use the bidi
//                ObjectDescriptor in both implementations.
//   open       : upload(size) ; DOWNLOAD = Open() only ; delete
//                SSB open_worker.cc
//   random8k   : upload(size) ; DOWNLOAD = Open() + one 8 KiB read at a
//                uniformly random offset ; delete           SSB random8k_worker.cc
//   read50x2x4 : upload(200 MiB) ; DOWNLOAD = Open() + 50 concurrent 2 MiB
//                reads at 4 MiB stride ; delete              SSB read50x2x4_worker.cc
//
// Uploads use appendable objects (SSB appendable_upload on RAPID buckets): one
// write of the full payload, then finalize. Object sizes default to SSB's
// {100 KB, 2 MiB, 100 MB}, picked per iteration from a fixed-seed PRNG so both
// implementations see the identical sequence.
//
// Per-operation results go to a CSV (one row per op per iteration) and a
// p50/p90/p99 summary is printed at the end.

#ifndef GCS_RUST_POC_SSB_WORKLOADS_H
#define GCS_RUST_POC_SSB_WORKLOADS_H

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <sys/resource.h>

#include "alloc_counter.h"

namespace ssb {

constexpr std::int64_t kKB = 1000;
constexpr std::int64_t kMB = kKB * kKB;
constexpr std::int64_t kKiB = 1024;
constexpr std::int64_t kMiB = kKiB * kKiB;

/// Interface each implementation provides. Errors are strings (empty == OK).
class Target {
 public:
  virtual ~Target() = default;
  virtual std::string Name() const = 0;
  /// Appendable upload of data[0, size): open, one write, finalize.
  virtual std::string Upload(std::string const& object, char const* data,
                             std::int64_t size) = 0;
  /// Opens a bidi read descriptor (stored as the "current" descriptor).
  virtual std::string Open(std::string const& object) = 0;
  /// Reads [offset, offset+length) from the current descriptor; length 0 = to
  /// end. Returns bytes read via *bytes.
  virtual std::string Read(std::int64_t offset, std::int64_t length,
                           std::int64_t* bytes) = 0;
  /// Issues all ranges concurrently on the current descriptor and waits.
  virtual std::string ReadConcurrent(
      std::vector<std::pair<std::int64_t, std::int64_t>> const& ranges,
      std::int64_t* bytes) = 0;
  virtual void CloseDescriptor() = 0;
  virtual std::string Delete(std::string const& object) = 0;

  /// Single round trip: open + read [offset, offset+length)
  /// (Rust send_and_read / C++ Open(..., InitialReadRanges)). Stores descriptor.
  virtual std::string OpenAndRead(std::string const& object, std::int64_t offset,
                                  std::int64_t length, std::int64_t* bytes) = 0;
  /// Concurrent ranges without one caller thread per range (Rust try_join_all
  /// inside the core; native C++ async chains, same as ReadConcurrent).
  virtual std::string ReadConcurrentInCore(
      std::vector<std::pair<std::int64_t, std::int64_t>> const& ranges,
      std::int64_t* bytes) = 0;

  // --- w1r3 (SSB sync client, JSON transport) ---------------------------------
  /// JSON upload to `bucket`: single-shot (InsertObject) or resumable (WriteObject).
  virtual std::string JsonUpload(std::string const& bucket, std::string const& object,
                                 char const* data, std::int64_t size, bool resumable) = 0;
  /// JSON full-object read (ReadObject), discarding data.
  virtual std::string JsonRead(std::string const& bucket, std::string const& object,
                               std::int64_t* bytes) = 0;
  /// Delete used by w1r3 (JSON for native; the Rust core only has a gRPC delete).
  virtual std::string JsonDelete(std::string const& bucket, std::string const& object) = 0;
};

struct Args {
  std::string bucket;       // RAPID bucket: open, random8k, read50x2x4
  std::string w1r3_bucket;  // STANDARD bucket: w1r3 (JSON); defaults to --bucket
  std::string workload = "all";
  int iterations = 100;
  std::vector<std::int64_t> object_sizes = {100 * kKB, 2 * kMiB, 100 * kMB};
  std::string out;  // CSV path; default: <impl>-<workload>.csv
  std::string prefix = "gcs-rust-poc/ssb/";
  std::string label;  // optional suffix for the impl name, e.g. "dp"
};

inline std::vector<std::int64_t> ParseSizes(std::string const& s) {
  std::vector<std::int64_t> r;
  std::stringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, ',')) r.push_back(std::stoll(tok));
  return r;
}

inline Args ParseArgs(int argc, char* argv[]) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto eq = arg.find('=');
    auto key = arg.substr(0, eq);
    auto val = eq == std::string::npos ? std::string() : arg.substr(eq + 1);
    if (key == "--bucket") a.bucket = val;
    else if (key == "--w1r3-bucket") a.w1r3_bucket = val;
    else if (key == "--workload") a.workload = val;
    else if (key == "--iterations") a.iterations = std::stoi(val);
    else if (key == "--object-sizes") a.object_sizes = ParseSizes(val);
    else if (key == "--out") a.out = val;
    else if (key == "--prefix") a.prefix = val;
    else if (key == "--label") a.label = val;
    else if (key == "--directpath") { /* handled by the native main */ }
    else {
      std::cerr << "unknown flag " << arg << "\nflags: --bucket=B "
                   "[--workload=w1r3|open|random8k|read50x2x4|all] "
                   "[--iterations=100] [--object-sizes=100000,2097152,100000000] "
                   "[--out=file.csv] [--prefix=P] [--label=L]\n";
      std::exit(2);
    }
  }
  if (a.bucket.empty()) {
    std::cerr << "--bucket is required\n";
    std::exit(2);
  }
  if (a.w1r3_bucket.empty()) a.w1r3_bucket = a.bucket;
  return a;
}

using Clock = std::chrono::steady_clock;

/// Measures wall time, process CPU time (user+sys, all threads) and heap
/// allocation volume of an op, similar to SSB's `usage()` tracker.
class Usage {
 public:
  Usage()
      : start_(Clock::now()), cpu_start_(CpuUs()),
        malloc_start_(alloc_counter::malloc_bytes()), new_start_(alloc_counter::new_bytes()) {}
  double WallMs() const {
    return std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
  }
  double CpuMs() const { return (CpuUs() - cpu_start_) / 1000.0; }
  /// All heap bytes allocated during the op (Rust + C++ + gRPC core).
  std::uint64_t AllocBytes() const { return alloc_counter::malloc_bytes() - malloc_start_; }
  /// Only C++ operator new bytes (what SSB's `memory` metric counts).
  std::uint64_t NewBytes() const { return alloc_counter::new_bytes() - new_start_; }

 private:
  static double CpuUs() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec * 1e6 + ru.ru_utime.tv_usec + ru.ru_stime.tv_sec * 1e6 +
           ru.ru_stime.tv_usec;
  }
  Clock::time_point start_;
  double cpu_start_;
  std::uint64_t malloc_start_;
  std::uint64_t new_start_;
};

struct Sample {
  std::string workload, op;
  int iteration;
  std::int64_t object_size, bytes;
  double wall_ms, cpu_ms;
  bool ok;
  std::string error;
  std::uint64_t alloc_bytes = 0;  // all heap allocations during the op
  std::uint64_t new_bytes = 0;    // C++ operator new only (SSB metric)
  long rss_kib = 0;               // process RSS when the op finished
};

class Recorder {
 public:
  Recorder(std::string impl, std::string const& path) : impl_(std::move(impl)), csv_(path) {
    csv_ << "impl,workload,iteration,object_size,op,latency_ms,cpu_ms,bytes,ok,error,"
            "alloc_bytes,new_bytes,rss_kib\n";
  }
  void Add(Sample s) {
    auto err = s.error.substr(0, 120);
    std::replace(err.begin(), err.end(), ',', ';');
    std::replace(err.begin(), err.end(), '\n', ' ');
    csv_ << impl_ << ',' << s.workload << ',' << s.iteration << ',' << s.object_size << ','
         << s.op << ',' << std::fixed << std::setprecision(3) << s.wall_ms << ',' << s.cpu_ms
         << ',' << s.bytes << ',' << (s.ok ? 1 : 0) << ',' << err << ',' << s.alloc_bytes << ','
         << s.new_bytes << ',' << s.rss_kib << '\n';
    csv_.flush();
    if (!s.ok) std::cerr << "  ERROR " << s.workload << " #" << s.iteration << " " << s.op
                         << ": " << s.error << "\n";
    samples_.push_back(std::move(s));
  }

  void PrintSummary() const {
    using Key = std::tuple<std::string, std::string, std::int64_t>;
    std::map<Key, std::vector<Sample const*>> groups;
    for (auto const& s : samples_) groups[{s.workload, s.op, s.object_size}].push_back(&s);
    std::cout << "\nSUMMARY impl=" << impl_ << "\n"
              << std::left << std::setw(11) << "workload" << std::setw(11) << "op"
              << std::right << std::setw(11) << "size" << std::setw(5) << "n" << std::setw(5)
              << "err" << std::setw(10) << "p50ms" << std::setw(10) << "p90ms"
              << std::setw(10) << "p99ms" << std::setw(10) << "mean" << std::setw(10)
              << "MiB/s50" << std::setw(10) << "cpu50ms" << "\n";
    for (auto const& [k, v] : groups) {
      std::vector<double> lat, cpu, tput;
      int errors = 0;
      for (auto const* s : v) {
        if (!s->ok) { ++errors; continue; }
        lat.push_back(s->wall_ms);
        cpu.push_back(s->cpu_ms);
        if (s->bytes > 0) tput.push_back((s->bytes / 1048576.0) / (s->wall_ms / 1000.0));
      }
      double mean = 0;
      for (auto x : lat) mean += x;
      if (!lat.empty()) mean /= lat.size();
      std::cout << std::left << std::setw(11) << std::get<0>(k) << std::setw(11)
                << std::get<1>(k) << std::right << std::setw(11) << std::get<2>(k)
                << std::setw(5) << v.size() << std::setw(5) << errors << std::fixed
                << std::setprecision(2) << std::setw(10) << Pct(lat, 50) << std::setw(10)
                << Pct(lat, 90) << std::setw(10) << Pct(lat, 99) << std::setw(10) << mean
                << std::setw(10) << Pct(tput, 50) << std::setw(10) << Pct(cpu, 50) << "\n";
    }
  }

  static double Pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    auto idx = static_cast<std::size_t>(p / 100.0 * (v.size() - 1) + 0.5);
    return v[std::min(idx, v.size() - 1)];
  }

 private:
  std::string impl_;
  std::ofstream csv_;
  std::vector<Sample> samples_;
};

class Runner {
 public:
  Runner(Target& t, Args a)
      : t_(t), a_(std::move(a)), gen_(42),
        impl_(t.Name() + (a_.label.empty() ? "" : "-" + a_.label)),
        run_id_(std::to_string(std::chrono::system_clock::now().time_since_epoch().count())) {
    // One random payload, large enough for every workload (read50x2x4 = 200 MiB).
    auto max_size = std::max<std::int64_t>(
        200 * kMiB, *std::max_element(a_.object_sizes.begin(), a_.object_sizes.end()));
    data_.resize(max_size);
    std::mt19937_64 g(7);
    for (std::size_t i = 0; i + 8 <= data_.size(); i += 8) {
      auto v = g();
      std::memcpy(&data_[i], &v, 8);
    }
  }

  int Run() {
    auto csv = a_.out.empty() ? impl_ + "-" + a_.workload + ".csv" : a_.out;
    Recorder rec(impl_, csv);
    std::vector<std::string> workloads;
    if (a_.workload == "all") workloads = {"w1r3", "open", "random8k", "read50x2x4"};
    else workloads = {a_.workload};
    std::cout << "impl=" << impl_ << " bucket=" << a_.bucket << " iterations="
              << a_.iterations << " csv=" << csv << "\n";
    for (auto const& w : workloads) {
      gen_.seed(42);  // identical size/offset sequence per workload, per impl
      auto t0 = Clock::now();
      // RSS_START includes the shared payload buffer (same size for both impls).
      long const rss_start = alloc_counter::rss_kib();
      Usage whole;
      for (int i = 0; i < a_.iterations; ++i) Iteration(w, i, rec);
      long const rss_end = alloc_counter::rss_kib();
      long const rss_peak = alloc_counter::peak_rss_kib();
      // Process-level rows (iteration = -1).
      auto proc = [&](std::string op, long rss) {
        Sample s{w, std::move(op), -1, 0, 0, 0, 0, true, ""};
        s.rss_kib = rss;
        rec.Add(std::move(s));
      };
      proc("RSS_START", rss_start);
      proc("RSS_END", rss_end);
      proc("RSS_PEAK", rss_peak);
      Sample total{w, "WORKLOAD_TOTAL", -1, 0, 0, whole.WallMs(), whole.CpuMs(), true, ""};
      total.alloc_bytes = whole.AllocBytes();
      total.new_bytes = whole.NewBytes();
      total.rss_kib = rss_end;
      rec.Add(std::move(total));
      std::cout << "  " << w << ": " << a_.iterations << " iterations in "
                << std::chrono::duration<double>(Clock::now() - t0).count() << " s"
                << "  cpu=" << whole.CpuMs() / 1000 << " s  alloc=" << whole.AllocBytes() / 1048576
                << " MiB  rss start/end/peak=" << rss_start / 1024 << "/" << rss_end / 1024 << "/"
                << rss_peak / 1024 << " MiB\n";
    }
    rec.PrintSummary();
    return 0;
  }

 private:
  std::string ObjectName(std::string const& w, int i) const {
    return a_.prefix + impl_ + "/" + run_id_ + "/" + w + "-" + std::to_string(i);
  }

  std::int64_t PickSize() {
    std::uniform_int_distribution<std::size_t> d(0, a_.object_sizes.size() - 1);
    return a_.object_sizes[d(gen_)];
  }

  // Returns false (and records the failure) if the op failed.
  template <typename F>
  bool Measure(Recorder& rec, std::string const& w, int i, std::int64_t size,
               std::string const& op, F&& f) {
    Usage u;
    std::int64_t bytes = 0;
    auto err = f(&bytes);
    Sample s{w, op, i, size, bytes, u.WallMs(), u.CpuMs(), err.empty(), err};
    s.alloc_bytes = u.AllocBytes();
    s.new_bytes = u.NewBytes();
    s.rss_kib = alloc_counter::rss_kib();
    rec.Add(std::move(s));
    return err.empty();
  }

  void Iteration(std::string const& w, int i, Recorder& rec) {
    if (w == "w1r3") return W1r3Iteration(i, rec);

    // ---- open / random8k / read50x2x4: SSB async workers on a RAPID bucket ----
    auto const size = (w == "read50x2x4") ? 200 * kMiB : PickSize();
    auto const name = ObjectName(w, i);
    // Draw the random8k offset now so the PRNG sequence is independent of results.
    std::uniform_int_distribution<std::int64_t> off_dist(0, std::max<std::int64_t>(0, size - 8 * kKiB));
    auto const random_offset = off_dist(gen_);

    // SSB op label for appendable uploads on RAPID buckets.
    if (!Measure(rec, w, i, size, "APPENDABLE", [&](std::int64_t* b) {
          *b = size;
          return t_.Upload(name, data_.data(), size);
        })) {
      return;
    }

    // SSB labels the download op with the workload name.
    if (w == "open") {
      Measure(rec, w, i, size, w, [&](std::int64_t*) { return t_.Open(name); });
      t_.CloseDescriptor();
    } else if (w == "random8k") {
      auto const len = std::min<std::int64_t>(8 * kKiB, size);
      // (1) two-step: Open() then Read()  - what C++ SSB random8k does.
      auto two_step = [&] {
        Measure(rec, w, i, size, "random8k", [&](std::int64_t* b) {
          if (auto e = t_.Open(name); !e.empty()) return e;
          auto e = t_.Read(random_offset, len, b);
          if (e.empty() && *b != len) e = "short read";
          return e;
        });
        t_.CloseDescriptor();
      };
      // (2) single round trip: open + read together - what Rust SSB random8k
      //     does (send_and_read); C++ equivalent is Open(..., InitialReadRanges).
      auto single = [&] {
        Measure(rec, w, i, size, "random8k_initial_range", [&](std::int64_t* b) {
          auto e = t_.OpenAndRead(name, random_offset, len, b);
          if (e.empty() && *b != len) e = "short read";
          return e;
        });
        t_.CloseDescriptor();
      };
      // Alternate the order so neither variant always gets the "warm" read.
      if (i % 2 == 0) { two_step(); single(); } else { single(); two_step(); }
    } else if (w == "read50x2x4") {
      std::vector<std::pair<std::int64_t, std::int64_t>> ranges;
      for (int j = 0; j < 50; ++j) ranges.emplace_back(j * 4 * kMiB, 2 * kMiB);
      // (1) original: wrapped = 50 C++ threads; native = 50 async chains.
      auto threads = [&] {
        Measure(rec, w, i, size, "read50x2x4", [&](std::int64_t* b) {
          if (auto e = t_.Open(name); !e.empty()) return e;
          auto e = t_.ReadConcurrent(ranges, b);
          if (e.empty() && *b != 50 * 2 * kMiB) e = "short read " + std::to_string(*b);
          return e;
        });
        t_.CloseDescriptor();
      };
      // (2) in-core: wrapped = 50 futures joined inside Rust (one C++ thread);
      //     native = same async chains as (1).
      auto in_core = [&] {
        Measure(rec, w, i, size, "read50x2x4_in_core", [&](std::int64_t* b) {
          if (auto e = t_.Open(name); !e.empty()) return e;
          auto e = t_.ReadConcurrentInCore(ranges, b);
          if (e.empty() && *b != 50 * 2 * kMiB) e = "short read " + std::to_string(*b);
          return e;
        });
        t_.CloseDescriptor();
      };
      if (i % 2 == 0) { threads(); in_core(); } else { in_core(); threads(); }
    }

    // SSB deletes but does not record a metric for these workloads; we record it anyway.
    Measure(rec, w, i, size, "DELETE", [&](std::int64_t*) { return t_.Delete(name); });
  }

  // ---- w1r3: SSB sync worker (w1r3_worker.cc + sync_w1r3_worker.cc), JSON ----
  //   upload = SINGLE-SHOT (InsertObject) or RESUMABLE (WriteObject), picked
  //   per iteration; READ[0..2] = full ReadObject; DELETE; plus the whole
  //   iteration as W1R3D1-SINGLE-SHOT / W1R3D1-RESUMABLE.
  void W1r3Iteration(int i, Recorder& rec) {
    std::string const w = "w1r3";
    auto const size = PickSize();
    std::uniform_int_distribution<int> up(0, 1);
    bool const resumable = up(gen_) == 1;
    std::string const upload_op = resumable ? "RESUMABLE" : "SINGLE-SHOT";
    auto const name = ObjectName(w, i);
    auto const& bucket = a_.w1r3_bucket;

    Usage total;
    if (!Measure(rec, w, i, size, upload_op, [&](std::int64_t* b) {
          *b = size;
          return t_.JsonUpload(bucket, name, data_.data(), size, resumable);
        })) {
      return;
    }
    for (auto const* op : {"READ[0]", "READ[1]", "READ[2]"}) {
      Measure(rec, w, i, size, op, [&](std::int64_t* b) {
        auto e = t_.JsonRead(bucket, name, b);
        if (e.empty() && *b != size) e = "short read " + std::to_string(*b);
        return e;
      });
    }
    Measure(rec, w, i, size, "DELETE", [&](std::int64_t*) { return t_.JsonDelete(bucket, name); });
    Sample t{w, "W1R3D1-" + upload_op, i, size, 4 * size, total.WallMs(), total.CpuMs(), true, ""};
    t.alloc_bytes = total.AllocBytes();
    t.new_bytes = total.NewBytes();
    t.rss_kib = alloc_counter::rss_kib();
    rec.Add(std::move(t));
  }

  Target& t_;
  Args a_;
  std::mt19937_64 gen_;
  std::string impl_;
  std::string run_id_;
  std::vector<char> data_;
};

}  // namespace ssb

#endif  // GCS_RUST_POC_SSB_WORKLOADS_H
