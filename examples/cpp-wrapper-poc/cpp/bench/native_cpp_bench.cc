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

// Benchmark baseline: native google-cloud-cpp (gRPC AsyncClient for BidiRead /
// BidiWrite, gRPC sync client for ListObjects). Same workload as the
// Rust-wrapped benchmark via bench_common.h.

#include "bench_common.h"
#include "google/cloud/storage/async/client.h"
#include "google/cloud/storage/grpc_plugin.h"

#include <optional>

namespace {

namespace gc = ::google::cloud;
namespace gcs = ::google::cloud::storage;

std::string Err(gc::Status const& s) {
  return s.ok() ? std::string() : gc::StatusCodeToString(s.code()) + ": " + s.message();
}

class Native : public bench::Target {
 public:
  explicit Native(std::string bucket)
      : async_(), sync_(gcs::MakeGrpcClient()), bucket_(std::move(bucket)) {}

  std::string Name() const override { return "native_cpp"; }

  std::string Write(std::string const& object, std::string const& data,
                    std::size_t chunk) override {
    auto start = async_.StartAppendableObjectUpload(gcs::BucketName(bucket_), object).get();
    if (!start) return Err(start.status());
    auto writer = std::move(start->first);
    auto token = std::move(start->second);
    for (std::size_t off = 0; off < data.size(); off += chunk) {
      auto n = std::min(chunk, data.size() - off);
      // Copy into the payload, same as the Rust path's Bytes::copy_from_slice.
      auto t = writer.Write(std::move(token), gcs::WritePayload(data.substr(off, n))).get();
      if (!t) return Err(t.status());
      token = std::move(*t);
    }
    auto m = writer.Finalize(std::move(token)).get();
    return m ? std::string() : Err(m.status());
  }

  std::string Open(std::string const& object) override {
    auto d = async_.Open(gcs::BucketName(bucket_), object).get();
    if (!d) return Err(d.status());
    desc_.emplace(std::move(*d));
    return {};
  }

  std::string ReadRange(std::uint64_t offset, std::uint64_t length,
                        std::uint64_t* bytes) override {
    auto [reader, token] = length == 0
                               ? desc_->ReadFromOffset(static_cast<std::int64_t>(offset))
                               : desc_->Read(static_cast<std::int64_t>(offset),
                                             static_cast<std::int64_t>(length));
    std::uint64_t got = 0;
    while (token.valid()) {
      auto r = reader.Read(std::move(token)).get();
      if (!r) return Err(r.status());
      got += r->first.size();
      token = std::move(r->second);
    }
    *bytes = got;
    return {};
  }

  std::string CloseDescriptor() override {
    desc_.reset();
    return {};
  }

  std::string List(std::string const& prefix, std::uint64_t* count) override {
    std::uint64_t n = 0;
    for (auto& o : sync_.ListObjects(bucket_, gcs::Prefix(prefix))) {
      if (!o) return Err(o.status());
      ++n;
    }
    *count = n;
    return {};
  }

  std::string Delete(std::string const& object) override {
    return Err(sync_.DeleteObject(bucket_, object));
  }

 private:
  gcs::AsyncClient async_;
  gcs::Client sync_;
  std::string bucket_;
  std::optional<gcs::ObjectDescriptor> desc_;
};

}  // namespace

int main(int argc, char* argv[]) {
  auto args = bench::ParseArgs(argc, argv);
  Native target(args.bucket);
  return bench::Run(target, args, argv[0]);
}
