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

// Benchmark: C++ wrapper -> C ABI -> Rust core (google-cloud-storage crate).

#include "bench_common.h"
#include "gcs_rust/client.h"

#include <cstring>
#include <optional>

namespace {

class RustWrapped : public bench::Target {
 public:
  RustWrapped(gcs_rust::Client c, std::string bucket)
      : client_(std::move(c)), bucket_(std::move(bucket)) {}

  std::string Name() const override { return "rust_wrapped"; }

  std::string Write(std::string const& object, std::string const& data,
                    std::size_t chunk) override {
    auto w = client_.OpenAppendableObject(bucket_, object);
    if (!w) return w.status().ToString();
    for (std::size_t off = 0; off < data.size(); off += chunk) {
      auto s = w->Append(data.data() + off, std::min(chunk, data.size() - off));
      if (!s.ok()) return s.ToString();
    }
    auto m = w->Finalize();
    return m ? std::string() : m.status().ToString();
  }

  std::string Open(std::string const& object) override {
    auto d = client_.OpenObject(bucket_, object);
    if (!d) return d.status().ToString();
    desc_.emplace(std::move(*d));
    return {};
  }

  std::string ReadRange(std::uint64_t offset, std::uint64_t length,
                        std::uint64_t* bytes) override {
    // Zero-copy: just count bytes, like an app that streams to a consumer.
    auto r = desc_->Read(offset, length, [](std::uint8_t const*, std::size_t) { return true; });
    if (!r) return r.status().ToString();
    *bytes = *r;
    return {};
  }

  std::string CloseDescriptor() override {
    desc_.reset();
    return {};
  }

  std::string List(std::string const& prefix, std::uint64_t* count) override {
    auto s = client_.ListObjects(bucket_, prefix, 1000,
                                 [](gcs_rust::ObjectMetadata const&) { return true; }, count);
    return s.ok() ? std::string() : s.ToString();
  }

  std::string Delete(std::string const& object) override {
    auto s = client_.DeleteObject(bucket_, object);
    return s.ok() ? std::string() : s.ToString();
  }

 private:
  gcs_rust::Client client_;
  std::string bucket_;
  std::optional<gcs_rust::ObjectDescriptor> desc_;
};

}  // namespace

int main(int argc, char* argv[]) {
  auto args = bench::ParseArgs(argc, argv);
  std::cout << "core: " << gcs_rust::Client::CoreVersion() << "\n";
  auto client = gcs_rust::Client::Create();
  if (!client) bench::Die("Client::Create", client.status().ToString());
  RustWrapped target(std::move(*client), args.bucket);
  return bench::Run(target, args, argv[0]);
}
