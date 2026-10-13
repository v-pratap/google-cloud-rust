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

// Thin, idiomatic C++ veneer over the Rust core (`gcs_core.h`).
//
// This is the only layer a C++ user sees. It owns no GCS logic: retries,
// streaming, checksums, auth and gRPC all live in the Rust core. Its jobs are:
//   * RAII ownership of the opaque C handles,
//   * converting C status codes into a C++ Status / StatusOr,
//   * turning C callbacks into std::function and catching C++ exceptions so
//     they never unwind into Rust.

#ifndef GCS_RUST_CLIENT_H
#define GCS_RUST_CLIENT_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Forward declarations of the opaque Rust handles (defined in gcs_core.h).
extern "C" {
typedef struct GcsClient GcsClient;
typedef struct GcsObjectDescriptor GcsObjectDescriptor;
typedef struct GcsAppendableWriter GcsAppendableWriter;
}

namespace gcs_rust {

/// Canonical gRPC status codes, same numbering as google::cloud::StatusCode.
enum class StatusCode : int {
  kOk = 0,
  kCancelled = 1,
  kUnknown = 2,
  kInvalidArgument = 3,
  kDeadlineExceeded = 4,
  kNotFound = 5,
  kAlreadyExists = 6,
  kPermissionDenied = 7,
  kResourceExhausted = 8,
  kFailedPrecondition = 9,
  kAborted = 10,
  kOutOfRange = 11,
  kUnimplemented = 12,
  kInternal = 13,
  kUnavailable = 14,
  kDataLoss = 15,
  kUnauthenticated = 16,
};

std::string StatusCodeToString(StatusCode code);

class Status {
 public:
  Status() = default;
  Status(StatusCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  bool ok() const { return code_ == StatusCode::kOk; }
  StatusCode code() const { return code_; }
  std::string const& message() const { return message_; }
  std::string ToString() const;

 private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

/// Minimal StatusOr<T>, enough for the POC.
template <typename T>
class StatusOr {
 public:
  StatusOr(Status s) : status_(std::move(s)) {}  // NOLINT(implicit)
  StatusOr(T v) : value_(std::move(v)) {}        // NOLINT(implicit)

  bool ok() const { return status_.ok(); }
  explicit operator bool() const { return ok(); }
  Status const& status() const { return status_; }
  T& value() & { return *value_; }
  T const& value() const& { return *value_; }
  T&& value() && { return *std::move(value_); }
  T& operator*() & { return *value_; }
  T const& operator*() const& { return *value_; }
  T* operator->() { return &*value_; }
  T const* operator->() const { return &*value_; }

 private:
  Status status_;
  std::optional<T> value_;
};

struct ObjectMetadata {
  std::string bucket;
  std::string name;
  std::string storage_class;
  std::int64_t size = 0;
  std::int64_t generation = 0;
  std::int64_t metageneration = 0;
};

namespace internal {
struct ClientDeleter {
  void operator()(GcsClient* p) const;
};
struct DescriptorDeleter {
  void operator()(GcsObjectDescriptor* p) const;
};
struct WriterDeleter {
  void operator()(GcsAppendableWriter* p) const;
};
}  // namespace internal

/// An object opened for reading over a single BidiReadObject stream.
///
/// Thread-safe: many threads may call `Read*()` concurrently; the ranges are
/// multiplexed over the same stream by the Rust core.
class ObjectDescriptor {
 public:
  /// Receives chunks as they arrive. The pointer is only valid during the
  /// call. Return false to stop (the read then returns kCancelled).
  using ChunkHandler = std::function<bool(std::uint8_t const*, std::size_t)>;

  StatusOr<ObjectMetadata> metadata() const;

  /// Streams [offset, offset+length) to `handler` with zero copies.
  /// `length == 0` means "until the end of the object".
  StatusOr<std::uint64_t> Read(std::uint64_t offset, std::uint64_t length,
                               ChunkHandler const& handler) const;

  /// Convenience: reads a range into a std::string.
  StatusOr<std::string> ReadToString(std::uint64_t offset,
                                     std::uint64_t length) const;

  /// Reads many ranges concurrently *inside the Rust core* (no C++ threads).
  /// `handler(index, data, size)` is called on the calling thread; `index` is
  /// the position of the range in `ranges`. Returns total bytes read.
  using RangeChunkHandler =
      std::function<bool(std::size_t, std::uint8_t const*, std::size_t)>;
  StatusOr<std::uint64_t> ReadRanges(
      std::vector<std::pair<std::uint64_t, std::uint64_t>> const& ranges,
      RangeChunkHandler const& handler) const;

 private:
  friend class Client;
  explicit ObjectDescriptor(GcsObjectDescriptor* h) : handle_(h) {}
  std::unique_ptr<GcsObjectDescriptor, internal::DescriptorDeleter> handle_;
};

/// An appendable object open over a single BidiWriteObject stream.
/// Not thread-safe: use from one thread at a time.
class AppendableWriter {
 public:
  Status Append(void const* data, std::size_t size);
  Status Append(std::string_view data) {
    return Append(data.data(), data.size());
  }
  /// Waits until everything appended so far is durable; returns persisted size.
  StatusOr<std::int64_t> Flush();
  /// Flushes and finalizes; the object becomes immutable.
  StatusOr<ObjectMetadata> Finalize();
  /// Flushes and closes without finalizing; returns persisted size.
  StatusOr<std::int64_t> Close();
  std::int64_t generation() const;

 private:
  friend class Client;
  explicit AppendableWriter(GcsAppendableWriter* h) : handle_(h) {}
  std::unique_ptr<GcsAppendableWriter, internal::WriterDeleter> handle_;
};

class Client {
 public:
  struct Options {
    std::string endpoint;                    // empty = default
    std::uint32_t grpc_subchannel_count = 0;  // 0 = default
    std::uint32_t worker_threads = 0;         // 0 = #CPUs
  };

  /// Creates a client with Application Default Credentials.
  static StatusOr<Client> Create(Options const& options);
  static StatusOr<Client> Create() { return Create(Options{}); }

  /// BidiRead: open once, then read many (possibly concurrent) ranges.
  StatusOr<ObjectDescriptor> OpenObject(std::string const& bucket,
                                        std::string const& object) const;

  /// BidiRead: open AND read [offset, offset+length) in one round trip
  /// (Rust `send_and_read`). Returns the descriptor; `*bytes` = bytes read.
  StatusOr<ObjectDescriptor> OpenAndRead(std::string const& bucket,
                                         std::string const& object,
                                         std::uint64_t offset, std::uint64_t length,
                                         ObjectDescriptor::ChunkHandler const& handler,
                                         std::uint64_t* bytes) const;

  /// BidiWrite: create a new appendable object.
  StatusOr<AppendableWriter> OpenAppendableObject(
      std::string const& bucket, std::string const& object) const;

  /// ListObjects (streaming). Return false from `handler` to stop early.
  Status ListObjects(std::string const& bucket, std::string const& prefix,
                     std::int32_t page_size,
                     std::function<bool(ObjectMetadata const&)> const& handler,
                     std::uint64_t* count = nullptr) const;

  /// ListObjects (collect everything).
  StatusOr<std::vector<ObjectMetadata>> ListObjects(
      std::string const& bucket, std::string const& prefix = {},
      std::int32_t page_size = 0) const;

  /// JSON API upload. `resumable=false` => single-shot, `true` => resumable.
  StatusOr<ObjectMetadata> WriteObject(std::string const& bucket,
                                       std::string const& object,
                                       void const* data, std::size_t size,
                                       bool resumable) const;

  /// JSON API full-object read, streamed to `handler` (zero-copy).
  StatusOr<std::uint64_t> ReadObject(std::string const& bucket,
                                     std::string const& object,
                                     ObjectDescriptor::ChunkHandler const& handler) const;

  /// Deletes an object (cleanup helper for tests/benchmarks).
  Status DeleteObject(std::string const& bucket,
                      std::string const& object) const;

  /// Version string of the Rust core.
  static std::string CoreVersion();

 private:
  explicit Client(GcsClient* h) : handle_(h, internal::ClientDeleter{}) {}
  std::shared_ptr<GcsClient> handle_;
};

}  // namespace gcs_rust

#endif  // GCS_RUST_CLIENT_H
