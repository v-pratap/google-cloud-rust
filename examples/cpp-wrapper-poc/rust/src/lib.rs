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

//! C ABI over the Rust `google-cloud-storage` crate.
//!
//! This is the "thick core" in the GCS SDK consolidation POC. The C++ SDK
//! (and, in principle, any other language) links `libgcs_core.a` and calls the
//! plain-C functions below; `cbindgen` turns this file into `gcs_core.h`.
//!
//! # Conventions
//! * Every fallible function returns a gRPC status code (`0` == OK) and, if
//!   `status` is non-null, fills it in. Free the message with
//!   [`gcs_status_clear`].
//! * Handles (`GcsClient*`, `GcsObjectDescriptor*`, `GcsAppendableWriter*`)
//!   are opaque and owned by the caller; release them with the matching
//!   `*_free` function. Child handles keep the runtime alive, so they may
//!   outlive the `GcsClient` that created them.
//! * Bucket arguments accept either `my-bucket` or `projects/_/buckets/my-bucket`.
//! * Calls block the calling thread. They are safe to make concurrently from
//!   many threads; the async work runs on a Tokio runtime owned by the client.
//! * Callbacks run on the calling thread, must not unwind (throw) across the
//!   boundary, and may return `false` to stop early (yields `CANCELLED`).
//! * Panics never cross the boundary; they are reported as `INTERNAL`.

use std::ffi::{CStr, CString, c_char, c_void};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;
use std::sync::Arc;

use bytes::Bytes;
use google_cloud_gax::paginator::ItemPaginator as _;
use google_cloud_storage::appendable_object_writer::AppendableObjectWriter;
use google_cloud_storage::client::{Storage, StorageControl};
use google_cloud_storage::model::Object;
use google_cloud_storage::model_ext::ReadRange;
use google_cloud_storage::object_descriptor::ObjectDescriptor;
use tokio::runtime::Runtime;

// ---------------------------------------------------------------------------
// Status codes (canonical gRPC codes)
// ---------------------------------------------------------------------------

const OK: i32 = 0;
const CANCELLED: i32 = 1;
const UNKNOWN: i32 = 2;
const INVALID_ARGUMENT: i32 = 3;
const DEADLINE_EXCEEDED: i32 = 4;
const NOT_FOUND: i32 = 5;
const ALREADY_EXISTS: i32 = 6;
const PERMISSION_DENIED: i32 = 7;
const RESOURCE_EXHAUSTED: i32 = 8;
const FAILED_PRECONDITION: i32 = 9;
const ABORTED: i32 = 10;
const OUT_OF_RANGE: i32 = 11;
const UNIMPLEMENTED: i32 = 12;
const INTERNAL: i32 = 13;
const UNAVAILABLE: i32 = 14;
const DATA_LOSS: i32 = 15;
const UNAUTHENTICATED: i32 = 16;

/// Error details returned by every fallible function.
#[repr(C)]
pub struct GcsStatus {
    /// Canonical gRPC status code; `0` means OK.
    pub code: i32,
    /// NUL-terminated, heap-allocated message, or NULL on success. Release
    /// with `gcs_status_clear`.
    pub message: *mut c_char,
}

/// Object metadata. String fields are heap-allocated; release with
/// `gcs_object_metadata_clear` (except when passed to a list callback, where
/// the library owns it).
#[repr(C)]
pub struct GcsObjectMetadata {
    pub bucket: *mut c_char,
    pub name: *mut c_char,
    pub storage_class: *mut c_char,
    pub size: i64,
    pub generation: i64,
    pub metageneration: i64,
}

/// Options for `gcs_client_new`. Zero / NULL fields use library defaults.
#[repr(C)]
pub struct GcsClientOptions {
    /// Override the service endpoint (e.g. a testbench), or NULL.
    pub endpoint: *const c_char,
    /// Number of gRPC subchannels used for the data path, 0 = default.
    pub grpc_subchannel_count: u32,
    /// Tokio worker threads, 0 = number of CPUs.
    pub worker_threads: u32,
}

/// Streaming-read callback: receives a chunk that is only valid during the
/// call. Return `false` to stop reading.
pub type GcsDataCallback =
    Option<unsafe extern "C" fn(user_data: *mut c_void, data: *const u8, len: usize) -> bool>;

/// List callback: receives metadata that is only valid during the call.
/// Return `false` to stop listing.
pub type GcsObjectCallback =
    Option<unsafe extern "C" fn(user_data: *mut c_void, object: *const GcsObjectMetadata) -> bool>;

// ---------------------------------------------------------------------------
// Opaque handles
// ---------------------------------------------------------------------------

/// A connection to Cloud Storage plus the runtime that drives it.
pub struct GcsClient {
    rt: Arc<Runtime>,
    storage: Storage,
    control: StorageControl,
}

/// An object opened for (concurrent) ranged reads over a BidiReadObject stream.
pub struct GcsObjectDescriptor {
    rt: Arc<Runtime>,
    inner: ObjectDescriptor,
}

/// An appendable object opened over a BidiWriteObject stream.
pub struct GcsAppendableWriter {
    rt: Arc<Runtime>,
    inner: Option<AppendableObjectWriter>,
}

// ---------------------------------------------------------------------------
// Error plumbing
// ---------------------------------------------------------------------------

struct FfiError {
    code: i32,
    message: String,
}

impl FfiError {
    fn new(code: i32, message: impl Into<String>) -> Self {
        Self { code, message: message.into() }
    }
}

fn code_from_name(name: &str) -> i32 {
    match name {
        "OK" => OK,
        "CANCELLED" => CANCELLED,
        "INVALID_ARGUMENT" => INVALID_ARGUMENT,
        "DEADLINE_EXCEEDED" => DEADLINE_EXCEEDED,
        "NOT_FOUND" => NOT_FOUND,
        "ALREADY_EXISTS" => ALREADY_EXISTS,
        "PERMISSION_DENIED" => PERMISSION_DENIED,
        "RESOURCE_EXHAUSTED" => RESOURCE_EXHAUSTED,
        "FAILED_PRECONDITION" => FAILED_PRECONDITION,
        "ABORTED" => ABORTED,
        "OUT_OF_RANGE" => OUT_OF_RANGE,
        "UNIMPLEMENTED" => UNIMPLEMENTED,
        "INTERNAL" => INTERNAL,
        "UNAVAILABLE" => UNAVAILABLE,
        "DATA_LOSS" => DATA_LOSS,
        "UNAUTHENTICATED" => UNAUTHENTICATED,
        _ => UNKNOWN,
    }
}

fn code_from_http(status: u16) -> i32 {
    match status {
        400 => INVALID_ARGUMENT,
        401 => UNAUTHENTICATED,
        403 => PERMISSION_DENIED,
        404 => NOT_FOUND,
        409 => ABORTED,
        412 => FAILED_PRECONDITION,
        416 => OUT_OF_RANGE,
        429 => RESOURCE_EXHAUSTED,
        499 => CANCELLED,
        500 => INTERNAL,
        501 => UNIMPLEMENTED,
        503 => UNAVAILABLE,
        504 => DEADLINE_EXCEEDED,
        _ => UNKNOWN,
    }
}

impl From<google_cloud_storage::Error> for FfiError {
    fn from(e: google_cloud_storage::Error) -> Self {
        let code = if let Some(s) = e.status() {
            code_from_name(s.code.name())
        } else if let Some(h) = e.http_status_code() {
            code_from_http(h)
        } else if e.is_timeout() {
            DEADLINE_EXCEEDED
        } else {
            UNKNOWN
        };
        // Include the full source chain; the top-level Display is often terse.
        let mut message = e.to_string();
        let mut source = std::error::Error::source(&e);
        while let Some(s) = source {
            message.push_str(": ");
            message.push_str(&s.to_string());
            source = s.source();
        }
        FfiError::new(code, message)
    }
}

fn builder_err(e: google_cloud_gax::client_builder::Error) -> FfiError {
    FfiError::new(INVALID_ARGUMENT, format!("cannot create client: {e}"))
}

fn to_cstring(s: &str) -> *mut c_char {
    // Interior NULs cannot be represented in a C string; replace them.
    CString::new(s.replace('\0', "\u{FFFD}"))
        .map(CString::into_raw)
        .unwrap_or(ptr::null_mut())
}

unsafe fn free_cstring(p: &mut *mut c_char) {
    if !p.is_null() {
        // SAFETY: every non-null string we hand out came from CString::into_raw.
        drop(unsafe { CString::from_raw(*p) });
        *p = ptr::null_mut();
    }
}

unsafe fn arg_str<'a>(p: *const c_char, what: &str) -> Result<&'a str, FfiError> {
    if p.is_null() {
        return Err(FfiError::new(INVALID_ARGUMENT, format!("`{what}` must not be NULL")));
    }
    // SAFETY: caller guarantees a valid NUL-terminated string.
    unsafe { CStr::from_ptr(p) }
        .to_str()
        .map_err(|_| FfiError::new(INVALID_ARGUMENT, format!("`{what}` is not valid UTF-8")))
}

unsafe fn opt_str<'a>(p: *const c_char, what: &str) -> Result<Option<&'a str>, FfiError> {
    if p.is_null() { Ok(None) } else { unsafe { arg_str(p, what) }.map(Some) }
}

fn bucket_path(bucket: &str) -> String {
    if bucket.starts_with("projects/") {
        bucket.to_string()
    } else {
        format!("projects/_/buckets/{bucket}")
    }
}

fn short_bucket(bucket: &str) -> &str {
    bucket.strip_prefix("projects/_/buckets/").unwrap_or(bucket)
}

fn fill_metadata(out: &mut GcsObjectMetadata, o: &Object) {
    out.bucket = to_cstring(short_bucket(&o.bucket));
    out.name = to_cstring(&o.name);
    out.storage_class = to_cstring(&o.storage_class);
    out.size = o.size;
    out.generation = o.generation;
    out.metageneration = o.metageneration;
}

/// Runs `f`, converts errors / panics into a status code and fills `status`.
fn ffi_call(status: *mut GcsStatus, f: impl FnOnce() -> Result<(), FfiError>) -> i32 {
    let err = match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => None,
        Ok(Err(e)) => Some(e),
        Err(panic) => {
            let msg = panic
                .downcast_ref::<&str>()
                .map(|s| s.to_string())
                .or_else(|| panic.downcast_ref::<String>().cloned())
                .unwrap_or_else(|| "unknown panic".to_string());
            Some(FfiError::new(INTERNAL, format!("panic in gcs_core: {msg}")))
        }
    };
    let (code, message) = match err {
        None => (OK, ptr::null_mut()),
        Some(e) => (e.code, to_cstring(&e.message)),
    };
    if !status.is_null() {
        // SAFETY: caller passed a valid, writable GcsStatus.
        unsafe {
            free_cstring(&mut (*status).message);
            (*status).code = code;
            (*status).message = message;
        }
    } else if !message.is_null() {
        // SAFETY: allocated just above by to_cstring.
        drop(unsafe { CString::from_raw(message) });
    }
    code
}

macro_rules! out_ref {
    ($p:expr, $what:literal) => {{
        if $p.is_null() {
            return Err(FfiError::new(INVALID_ARGUMENT, concat!("`", $what, "` must not be NULL")));
        }
        // SAFETY: checked non-null; caller guarantees it is valid and writable.
        unsafe { &mut *$p }
    }};
}

// ---------------------------------------------------------------------------
// Library / status / metadata helpers
// ---------------------------------------------------------------------------

/// Returns a static version string for the core library.
#[unsafe(no_mangle)]
pub extern "C" fn gcs_core_version() -> *const c_char {
    concat!("gcs_core ", env!("CARGO_PKG_VERSION"), " (google-cloud-storage rust)\0").as_ptr()
        as *const c_char
}

/// Releases the message held by `status` and resets it to OK.
///
/// # Safety
/// `status` must be NULL or point to a `GcsStatus` filled by this library.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_status_clear(status: *mut GcsStatus) {
    if let Some(s) = unsafe { status.as_mut() } {
        unsafe { free_cstring(&mut s.message) };
        s.code = OK;
    }
}

/// Releases the strings held by `metadata` and zeroes it.
///
/// # Safety
/// `metadata` must be NULL or point to metadata filled by this library.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_object_metadata_clear(metadata: *mut GcsObjectMetadata) {
    if let Some(m) = unsafe { metadata.as_mut() } {
        unsafe {
            free_cstring(&mut m.bucket);
            free_cstring(&mut m.name);
            free_cstring(&mut m.storage_class);
        }
        m.size = 0;
        m.generation = 0;
        m.metageneration = 0;
    }
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

/// Creates a client using Application Default Credentials.
///
/// # Safety
/// `options` may be NULL. `out` must be a valid pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_client_new(
    options: *const GcsClientOptions,
    out: *mut *mut GcsClient,
    status: *mut GcsStatus,
) -> i32 {
    ffi_call(status, || {
        let out = out_ref!(out, "out");
        *out = ptr::null_mut();
        let opts = unsafe { options.as_ref() };
        let endpoint = match opts {
            Some(o) => unsafe { opt_str(o.endpoint, "options.endpoint") }?.map(str::to_string),
            None => None,
        };
        let subchannels = opts.map(|o| o.grpc_subchannel_count).unwrap_or(0);
        let workers = opts.map(|o| o.worker_threads).unwrap_or(0);

        let mut rt = tokio::runtime::Builder::new_multi_thread();
        rt.enable_all().thread_name("gcs-core");
        if workers > 0 {
            rt.worker_threads(workers as usize);
        }
        let rt = rt
            .build()
            .map_err(|e| FfiError::new(INTERNAL, format!("cannot start runtime: {e}")))?;

        let (storage, control) = rt.block_on(async {
            let mut sb = Storage::builder();
            let mut cb = StorageControl::builder();
            if let Some(ep) = &endpoint {
                sb = sb.with_endpoint(ep.clone());
                cb = cb.with_endpoint(ep.clone());
            }
            if subchannels > 0 {
                sb = sb.with_grpc_subchannel_count(subchannels as usize);
            }
            let storage = sb.build().await.map_err(builder_err)?;
            let control = cb.build().await.map_err(builder_err)?;
            Ok::<_, FfiError>((storage, control))
        })?;

        *out = Box::into_raw(Box::new(GcsClient { rt: Arc::new(rt), storage, control }));
        Ok(())
    })
}

/// Destroys a client. Outstanding descriptors / writers remain usable.
///
/// # Safety
/// `client` must be NULL or a handle returned by `gcs_client_new`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_client_free(client: *mut GcsClient) {
    if !client.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(client) })));
    }
}

// ---------------------------------------------------------------------------
// BidiRead: open_object + read_range
// ---------------------------------------------------------------------------

/// Opens `bucket/object` for ranged reads (one BidiReadObject stream).
///
/// # Safety
/// `client`, `bucket`, `object` and `out` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_object_open(
    client: *const GcsClient,
    bucket: *const c_char,
    object: *const c_char,
    out: *mut *mut GcsObjectDescriptor,
    status: *mut GcsStatus,
) -> i32 {
    ffi_call(status, || {
        let out = out_ref!(out, "out");
        *out = ptr::null_mut();
        let client = unsafe { client.as_ref() }
            .ok_or_else(|| FfiError::new(INVALID_ARGUMENT, "`client` must not be NULL"))?;
        let bucket = bucket_path(unsafe { arg_str(bucket, "bucket") }?);
        let object = unsafe { arg_str(object, "object") }?.to_string();
        let inner = client
            .rt
            .block_on(client.storage.open_object(bucket, object).send())?;
        *out = Box::into_raw(Box::new(GcsObjectDescriptor { rt: client.rt.clone(), inner }));
        Ok(())
    })
}

/// Copies the metadata of an open object into `out`.
///
/// # Safety
/// `descriptor` and `out` must be valid. Release `out` with
/// `gcs_object_metadata_clear`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_object_metadata(
    descriptor: *const GcsObjectDescriptor,
    out: *mut GcsObjectMetadata,
    status: *mut GcsStatus,
) -> i32 {
    ffi_call(status, || {
        let out = out_ref!(out, "out");
        let d = unsafe { descriptor.as_ref() }
            .ok_or_else(|| FfiError::new(INVALID_ARGUMENT, "`descriptor` must not be NULL"))?;
        fill_metadata(out, &d.inner.object());
        Ok(())
    })
}

/// Reads `length` bytes starting at `offset` (`length == 0` reads to the end),
/// delivering data through `callback` without extra copies.
///
/// Many threads may call this concurrently on the same descriptor; the reads
/// are multiplexed over the same BidiReadObject stream.
///
/// # Safety
/// `descriptor` must be valid; `callback` must be non-NULL and must not unwind.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_object_read_range(
    descriptor: *const GcsObjectDescriptor,
    offset: u64,
    length: u64,
    callback: GcsDataCallback,
    user_data: *mut c_void,
    bytes_read: *mut u64,
    status: *mut GcsStatus,
) -> i32 {
    // Raw pointers are not Send; carry the address across the closure as usize.
    let user = user_data as usize;
    ffi_call(status, || {
        let d = unsafe { descriptor.as_ref() }
            .ok_or_else(|| FfiError::new(INVALID_ARGUMENT, "`descriptor` must not be NULL"))?;
        let cb = callback
            .ok_or_else(|| FfiError::new(INVALID_ARGUMENT, "`callback` must not be NULL"))?;
        let range = if length == 0 { ReadRange::offset(offset) } else { ReadRange::segment(offset, length) };
        // `block_on` polls this future on the calling thread, so the callback
        // runs on the caller's thread.
        let total = d.rt.block_on(async {
            let mut reader = d.inner.read_range(range).await;
            let mut total = 0u64;
            while let Some(chunk) = reader.next().await {
                let chunk = chunk?;
                total += chunk.len() as u64;
                // SAFETY: caller contract: valid fn pointer that does not unwind.
                if !unsafe { cb(user as *mut c_void, chunk.as_ptr(), chunk.len()) } {
                    return Err(FfiError::new(CANCELLED, "read cancelled by callback"));
                }
            }
            Ok::<u64, FfiError>(total)
        })?;
        if let Some(b) = unsafe { bytes_read.as_mut() } {
            *b = total;
        }
        Ok(())
    })
}

/// Closes the descriptor (ends the BidiReadObject stream).
///
/// # Safety
/// `descriptor` must be NULL or a handle returned by `gcs_object_open`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_object_descriptor_free(descriptor: *mut GcsObjectDescriptor) {
    if !descriptor.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            let d = unsafe { Box::from_raw(descriptor) };
            // Drop inside the runtime so background stream tasks shut down cleanly.
            let rt = d.rt.clone();
            let _guard = rt.enter();
            drop(d);
        }));
    }
}

// ---------------------------------------------------------------------------
// BidiWrite: appendable objects
// ---------------------------------------------------------------------------

/// Creates a new appendable object (one BidiWriteObject stream).
///
/// # Safety
/// `client`, `bucket`, `object` and `out` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_appendable_open(
    client: *const GcsClient,
    bucket: *const c_char,
    object: *const c_char,
    out: *mut *mut GcsAppendableWriter,
    status: *mut GcsStatus,
) -> i32 {
    ffi_call(status, || {
        let out = out_ref!(out, "out");
        *out = ptr::null_mut();
        let client = unsafe { client.as_ref() }
            .ok_or_else(|| FfiError::new(INVALID_ARGUMENT, "`client` must not be NULL"))?;
        let bucket = bucket_path(unsafe { arg_str(bucket, "bucket") }?);
        let object = unsafe { arg_str(object, "object") }?.to_string();
        let writer = client
            .rt
            .block_on(client.storage.open_appendable_object(bucket, object).send())?;
        *out = Box::into_raw(Box::new(GcsAppendableWriter {
            rt: client.rt.clone(),
            inner: Some(writer),
        }));
        Ok(())
    })
}

fn live_writer<'a>(w: *mut GcsAppendableWriter) -> Result<&'a mut GcsAppendableWriter, FfiError> {
    let w = unsafe { w.as_mut() }
        .ok_or_else(|| FfiError::new(INVALID_ARGUMENT, "`writer` must not be NULL"))?;
    if w.inner.is_none() {
        return Err(FfiError::new(FAILED_PRECONDITION, "writer already finalized or closed"));
    }
    Ok(w)
}

/// Appends `len` bytes (copied) to the writer's buffer.
///
/// # Safety
/// `writer` must be valid and not used concurrently; `data` must point to
/// `len` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_appendable_append(
    writer: *mut GcsAppendableWriter,
    data: *const u8,
    len: usize,
    status: *mut GcsStatus,
) -> i32 {
    ffi_call(status, || {
        let w = live_writer(writer)?;
        if len == 0 {
            return Ok(());
        }
        if data.is_null() {
            return Err(FfiError::new(INVALID_ARGUMENT, "`data` must not be NULL"));
        }
        // SAFETY: caller guarantees `len` readable bytes.
        let chunk = Bytes::copy_from_slice(unsafe { std::slice::from_raw_parts(data, len) });
        let inner = w.inner.as_mut().unwrap();
        w.rt.block_on(inner.append(chunk))?;
        Ok(())
    })
}

/// Waits until all appended bytes are persisted; returns the persisted size.
///
/// # Safety
/// `writer` must be valid and not used concurrently.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_appendable_flush(
    writer: *mut GcsAppendableWriter,
    persisted_size: *mut i64,
    status: *mut GcsStatus,
) -> i32 {
    ffi_call(status, || {
        let w = live_writer(writer)?;
        let inner = w.inner.as_mut().unwrap();
        let size = w.rt.block_on(inner.flush())?;
        if let Some(p) = unsafe { persisted_size.as_mut() } {
            *p = size;
        }
        Ok(())
    })
}

/// Flushes and finalizes the object (no more appends). Fills `out` (may be NULL).
///
/// # Safety
/// `writer` must be valid. Release `out` with `gcs_object_metadata_clear`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_appendable_finalize(
    writer: *mut GcsAppendableWriter,
    out: *mut GcsObjectMetadata,
    status: *mut GcsStatus,
) -> i32 {
    ffi_call(status, || {
        let w = live_writer(writer)?;
        let inner = w.inner.take().unwrap();
        let object = w.rt.block_on(inner.finalize())?;
        if let Some(m) = unsafe { out.as_mut() } {
            fill_metadata(m, &object);
        }
        Ok(())
    })
}

/// Flushes and closes the stream without finalizing (object stays appendable).
///
/// # Safety
/// `writer` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_appendable_close(
    writer: *mut GcsAppendableWriter,
    persisted_size: *mut i64,
    status: *mut GcsStatus,
) -> i32 {
    ffi_call(status, || {
        let w = live_writer(writer)?;
        let inner = w.inner.take().unwrap();
        let size = w.rt.block_on(inner.close())?;
        if let Some(p) = unsafe { persisted_size.as_mut() } {
            *p = size;
        }
        Ok(())
    })
}

/// Returns the generation of the appendable object, or -1 if unavailable.
///
/// # Safety
/// `writer` must be NULL or valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_appendable_generation(writer: *const GcsAppendableWriter) -> i64 {
    unsafe { writer.as_ref() }
        .and_then(|w| w.inner.as_ref())
        .map(|i| i.generation())
        .unwrap_or(-1)
}

/// Destroys the writer. Un-flushed data is discarded.
///
/// # Safety
/// `writer` must be NULL or a handle returned by `gcs_appendable_open`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_appendable_free(writer: *mut GcsAppendableWriter) {
    if !writer.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            let w = unsafe { Box::from_raw(writer) };
            let rt = w.rt.clone();
            let _guard = rt.enter();
            drop(w);
        }));
    }
}

// ---------------------------------------------------------------------------
// ListObjects
// ---------------------------------------------------------------------------

/// Lists objects in `bucket` whose names start with `prefix` (NULL = all),
/// invoking `callback` once per object. Pagination is handled internally.
///
/// # Safety
/// `client`, `bucket` must be valid; `callback` must be non-NULL and must not
/// unwind.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_list_objects(
    client: *const GcsClient,
    bucket: *const c_char,
    prefix: *const c_char,
    page_size: i32,
    callback: GcsObjectCallback,
    user_data: *mut c_void,
    count: *mut u64,
    status: *mut GcsStatus,
) -> i32 {
    let user = user_data as usize;
    ffi_call(status, || {
        let client = unsafe { client.as_ref() }
            .ok_or_else(|| FfiError::new(INVALID_ARGUMENT, "`client` must not be NULL"))?;
        let cb = callback
            .ok_or_else(|| FfiError::new(INVALID_ARGUMENT, "`callback` must not be NULL"))?;
        let bucket = bucket_path(unsafe { arg_str(bucket, "bucket") }?);
        let prefix = unsafe { opt_str(prefix, "prefix") }?.map(str::to_string);

        let n = client.rt.block_on(async {
            let mut req = client.control.list_objects().set_parent(bucket);
            if let Some(p) = prefix {
                req = req.set_prefix(p);
            }
            if page_size > 0 {
                req = req.set_page_size(page_size);
            }
            let mut items = req.by_item();
            let mut n = 0u64;
            while let Some(item) = items.next().await {
                let object = item?;
                n += 1;
                let mut m = GcsObjectMetadata {
                    bucket: ptr::null_mut(),
                    name: ptr::null_mut(),
                    storage_class: ptr::null_mut(),
                    size: 0,
                    generation: 0,
                    metageneration: 0,
                };
                fill_metadata(&mut m, &object);
                // SAFETY: caller contract: valid fn pointer that does not unwind.
                let keep_going = unsafe { cb(user as *mut c_void, &m) };
                unsafe { gcs_object_metadata_clear(&mut m) };
                if !keep_going {
                    return Err(FfiError::new(CANCELLED, "list cancelled by callback"));
                }
            }
            Ok::<u64, FfiError>(n)
        })?;
        if let Some(c) = unsafe { count.as_mut() } {
            *c = n;
        }
        Ok(())
    })
}

/// Deletes `bucket/object` (latest generation). Used by tests / benchmarks to
/// clean up; not part of the three POC data-path APIs.
///
/// # Safety
/// `client`, `bucket`, `object` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_delete_object(
    client: *const GcsClient,
    bucket: *const c_char,
    object: *const c_char,
    status: *mut GcsStatus,
) -> i32 {
    ffi_call(status, || {
        let client = unsafe { client.as_ref() }
            .ok_or_else(|| FfiError::new(INVALID_ARGUMENT, "`client` must not be NULL"))?;
        let bucket = bucket_path(unsafe { arg_str(bucket, "bucket") }?);
        let object = unsafe { arg_str(object, "object") }?.to_string();
        client.rt.block_on(
            client.control.delete_object().set_bucket(bucket).set_object(object).send(),
        )?;
        Ok(())
    })
}

// ---------------------------------------------------------------------------
// JSON (HTTP) data path: used for the SSB w1r3 workload (sync, JSON).
// google-cloud-storage's `write_object` / `read_object` use the JSON API.
// ---------------------------------------------------------------------------

/// Uploads `len` bytes to `bucket/object` over the JSON API.
/// `resumable == false` forces a single-shot upload (one request);
/// `resumable == true` forces a resumable upload session.
///
/// # Safety
/// `client`, `bucket`, `object` must be valid; `data` must point to `len`
/// readable bytes. `out` may be NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_write_object(
    client: *const GcsClient,
    bucket: *const c_char,
    object: *const c_char,
    data: *const u8,
    len: usize,
    resumable: bool,
    out: *mut GcsObjectMetadata,
    status: *mut GcsStatus,
) -> i32 {
    ffi_call(status, || {
        let client = unsafe { client.as_ref() }
            .ok_or_else(|| FfiError::new(INVALID_ARGUMENT, "`client` must not be NULL"))?;
        let bucket = bucket_path(unsafe { arg_str(bucket, "bucket") }?);
        let object = unsafe { arg_str(object, "object") }?.to_string();
        if data.is_null() && len > 0 {
            return Err(FfiError::new(INVALID_ARGUMENT, "`data` must not be NULL"));
        }
        let payload = if len == 0 {
            Bytes::new()
        } else {
            // SAFETY: caller guarantees `len` readable bytes.
            Bytes::copy_from_slice(unsafe { std::slice::from_raw_parts(data, len) })
        };
        // Threshold 0 => always resumable; usize::MAX => always single-shot.
        let threshold = if resumable { 0usize } else { usize::MAX };
        let object = client.rt.block_on(
            client
                .storage
                .write_object(bucket, object, payload)
                .with_resumable_upload_threshold(threshold)
                .send_unbuffered(),
        )?;
        if let Some(m) = unsafe { out.as_mut() } {
            fill_metadata(m, &object);
        }
        Ok(())
    })
}

/// Reads the whole object over the JSON API, streaming chunks to `callback`.
///
/// # Safety
/// `client`, `bucket`, `object` must be valid; `callback` must be non-NULL and
/// must not unwind.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_read_object(
    client: *const GcsClient,
    bucket: *const c_char,
    object: *const c_char,
    callback: GcsDataCallback,
    user_data: *mut c_void,
    bytes_read: *mut u64,
    status: *mut GcsStatus,
) -> i32 {
    let user = user_data as usize;
    ffi_call(status, || {
        let client = unsafe { client.as_ref() }
            .ok_or_else(|| FfiError::new(INVALID_ARGUMENT, "`client` must not be NULL"))?;
        let cb = callback
            .ok_or_else(|| FfiError::new(INVALID_ARGUMENT, "`callback` must not be NULL"))?;
        let bucket = bucket_path(unsafe { arg_str(bucket, "bucket") }?);
        let object = unsafe { arg_str(object, "object") }?.to_string();
        let total = client.rt.block_on(async {
            let mut reader = client.storage.read_object(bucket, object).send().await?;
            let mut total = 0u64;
            while let Some(chunk) = reader.next().await {
                let chunk = chunk?;
                total += chunk.len() as u64;
                // SAFETY: caller contract: valid fn pointer that does not unwind.
                if !unsafe { cb(user as *mut c_void, chunk.as_ptr(), chunk.len()) } {
                    return Err(FfiError::new(CANCELLED, "read cancelled by callback"));
                }
            }
            Ok::<u64, FfiError>(total)
        })?;
        if let Some(b) = unsafe { bytes_read.as_mut() } {
            *b = total;
        }
        Ok(())
    })
}

// ---------------------------------------------------------------------------
// Optimised read paths (verification run)
// ---------------------------------------------------------------------------

/// Opens `bucket/object` AND reads [offset, offset+length) in the same
/// BidiReadObject round trip (`open_object().send_and_read()`), streaming the
/// data to `callback`. Returns the descriptor for further reads.
///
/// # Safety
/// `client`, `bucket`, `object`, `out` must be valid; `callback` non-NULL and
/// must not unwind.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_object_open_and_read(
    client: *const GcsClient,
    bucket: *const c_char,
    object: *const c_char,
    offset: u64,
    length: u64,
    callback: GcsDataCallback,
    user_data: *mut c_void,
    out: *mut *mut GcsObjectDescriptor,
    bytes_read: *mut u64,
    status: *mut GcsStatus,
) -> i32 {
    let user = user_data as usize;
    ffi_call(status, || {
        let out = out_ref!(out, "out");
        *out = ptr::null_mut();
        let client = unsafe { client.as_ref() }
            .ok_or_else(|| FfiError::new(INVALID_ARGUMENT, "`client` must not be NULL"))?;
        let cb = callback
            .ok_or_else(|| FfiError::new(INVALID_ARGUMENT, "`callback` must not be NULL"))?;
        let bucket = bucket_path(unsafe { arg_str(bucket, "bucket") }?);
        let object = unsafe { arg_str(object, "object") }?.to_string();
        let range = if length == 0 { ReadRange::offset(offset) } else { ReadRange::segment(offset, length) };
        let (descriptor, total) = client.rt.block_on(async {
            let (descriptor, mut reader) =
                client.storage.open_object(bucket, object).send_and_read(range).await?;
            let mut total = 0u64;
            while let Some(chunk) = reader.next().await {
                let chunk = chunk?;
                total += chunk.len() as u64;
                // SAFETY: caller contract: valid fn pointer that does not unwind.
                if !unsafe { cb(user as *mut c_void, chunk.as_ptr(), chunk.len()) } {
                    return Err(FfiError::new(CANCELLED, "read cancelled by callback"));
                }
            }
            Ok::<_, FfiError>((descriptor, total))
        })?;
        if let Some(b) = unsafe { bytes_read.as_mut() } {
            *b = total;
        }
        *out = Box::into_raw(Box::new(GcsObjectDescriptor { rt: client.rt.clone(), inner: descriptor }));
        Ok(())
    })
}

/// Range callback: `index` identifies which requested range the chunk belongs to.
pub type GcsRangeCallback = Option<
    unsafe extern "C" fn(user_data: *mut c_void, index: usize, data: *const u8, len: usize) -> bool,
>;

/// Reads `count` ranges concurrently on one descriptor *inside Rust*
/// (futures::try_join_all over read_range), i.e. without one C++ thread per
/// range. Chunks are delivered to `callback` on the calling thread.
///
/// # Safety
/// `descriptor` valid; `offsets`/`lengths` point to `count` elements;
/// `callback` non-NULL and must not unwind.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gcs_object_read_ranges(
    descriptor: *const GcsObjectDescriptor,
    offsets: *const u64,
    lengths: *const u64,
    count: usize,
    callback: GcsRangeCallback,
    user_data: *mut c_void,
    bytes_read: *mut u64,
    status: *mut GcsStatus,
) -> i32 {
    let user = user_data as usize;
    ffi_call(status, || {
        let d = unsafe { descriptor.as_ref() }
            .ok_or_else(|| FfiError::new(INVALID_ARGUMENT, "`descriptor` must not be NULL"))?;
        let cb = callback
            .ok_or_else(|| FfiError::new(INVALID_ARGUMENT, "`callback` must not be NULL"))?;
        if count > 0 && (offsets.is_null() || lengths.is_null()) {
            return Err(FfiError::new(INVALID_ARGUMENT, "`offsets`/`lengths` must not be NULL"));
        }
        // SAFETY: caller guarantees `count` elements.
        let offs = unsafe { std::slice::from_raw_parts(offsets, count) };
        let lens = unsafe { std::slice::from_raw_parts(lengths, count) };
        let total = d.rt.block_on(async {
            let tasks = offs.iter().zip(lens.iter()).enumerate().map(|(idx, (&off, &len))| {
                let inner = &d.inner;
                async move {
                    let range = if len == 0 { ReadRange::offset(off) } else { ReadRange::segment(off, len) };
                    let mut reader = inner.read_range(range).await;
                    let mut n = 0u64;
                    while let Some(chunk) = reader.next().await {
                        let chunk = chunk?;
                        n += chunk.len() as u64;
                        // SAFETY: caller contract; all futures are polled by
                        // this one block_on task, so calls are never concurrent.
                        if !unsafe { cb(user as *mut c_void, idx, chunk.as_ptr(), chunk.len()) } {
                            return Err(FfiError::new(CANCELLED, "read cancelled by callback"));
                        }
                    }
                    Ok::<u64, FfiError>(n)
                }
            });
            let parts = futures::future::try_join_all(tasks).await?;
            Ok::<u64, FfiError>(parts.into_iter().sum())
        })?;
        if let Some(b) = unsafe { bytes_read.as_mut() } {
            *b = total;
        }
        Ok(())
    })
}
