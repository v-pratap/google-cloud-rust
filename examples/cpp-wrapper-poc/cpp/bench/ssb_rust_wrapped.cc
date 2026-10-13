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

// SSB workloads through the C++ wrapper -> C ABI -> Rust core.

#include "gcs_rust/client.h"
#include "ssb_workloads.h"

#include <future>
#include <optional>

namespace {

class RustWrapped : public ssb::Target {
 public:
  RustWrapped(gcs_rust::Client c, std::string bucket)
      : client_(std::move(c)), bucket_(std::move(bucket)) {}

  std::string Name() const override { return "rust_wrapped"; }

  std::string Upload(std::string const& object, char const* data,
                     std::int64_t size) override {
    auto w = client_.OpenAppendableObject(bucket_, object);
    if (!w) return w.status().ToString();
    if (auto s = w->Append(data, static_cast<std::size_t>(size)); !s.ok()) return s.ToString();
    auto m = w->Finalize();
    return m ? std::string() : m.status().ToString();
  }

  std::string Open(std::string const& object) override {
    auto d = client_.OpenObject(bucket_, object);
    if (!d) return d.status().ToString();
    desc_.emplace(std::move(*d));
    return {};
  }

  std::string Read(std::int64_t offset, std::int64_t length, std::int64_t* bytes) override {
    // Discard bytes (like SSB), zero-copy callback.
    auto r = desc_->Read(offset, length, [](std::uint8_t const*, std::size_t) { return true; });
    if (!r) return r.status().ToString();
    *bytes = static_cast<std::int64_t>(*r);
    return {};
  }

  std::string ReadConcurrent(std::vector<std::pair<std::int64_t, std::int64_t>> const& ranges,
                             std::int64_t* bytes) override {
    // The C ABI is blocking, so concurrency = one C++ thread per range. All
    // ranges are multiplexed by the Rust core over the same BidiReadObject stream.
    std::vector<std::future<std::pair<std::string, std::int64_t>>> futures;
    futures.reserve(ranges.size());
    for (auto const& [off, len] : ranges) {
      futures.push_back(std::async(std::launch::async, [this, off = off, len = len] {
        std::int64_t b = 0;
        auto e = Read(off, len, &b);
        return std::make_pair(e, b);
      }));
    }
    std::string first_error;
    std::int64_t total = 0;
    for (auto& f : futures) {
      auto [e, b] = f.get();
      total += b;
      if (!e.empty() && first_error.empty()) first_error = e;
    }
    *bytes = total;
    return first_error;
  }

  void CloseDescriptor() override { desc_.reset(); }

  std::string OpenAndRead(std::string const& object, std::int64_t offset, std::int64_t length,
                          std::int64_t* bytes) override {
    std::uint64_t got = 0;
    auto d = client_.OpenAndRead(bucket_, object, offset, length,
                                 [](std::uint8_t const*, std::size_t) { return true; }, &got);
    if (!d) return d.status().ToString();
    desc_.emplace(std::move(*d));
    *bytes = static_cast<std::int64_t>(got);
    return {};
  }

  std::string ReadConcurrentInCore(std::vector<std::pair<std::int64_t, std::int64_t>> const& ranges,
                                   std::int64_t* bytes) override {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> r;
    r.reserve(ranges.size());
    for (auto const& [o, l] : ranges) r.emplace_back(o, l);
    auto n = desc_->ReadRanges(r, [](std::size_t, std::uint8_t const*, std::size_t) { return true; });
    if (!n) return n.status().ToString();
    *bytes = static_cast<std::int64_t>(*n);
    return {};
  }

  std::string Delete(std::string const& object) override {
    auto s = client_.DeleteObject(bucket_, object);
    return s.ok() ? std::string() : s.ToString();
  }

  // --- w1r3: Rust write_object / read_object (JSON API) ----------------------
  std::string JsonUpload(std::string const& bucket, std::string const& object, char const* data,
                         std::int64_t size, bool resumable) override {
    auto m = client_.WriteObject(bucket, object, data, static_cast<std::size_t>(size), resumable);
    return m ? std::string() : m.status().ToString();
  }

  std::string JsonRead(std::string const& bucket, std::string const& object,
                       std::int64_t* bytes) override {
    auto r = client_.ReadObject(bucket, object,
                                [](std::uint8_t const*, std::size_t) { return true; });
    if (!r) return r.status().ToString();
    *bytes = static_cast<std::int64_t>(*r);
    return {};
  }

  // The Rust crate only exposes delete via StorageControl (gRPC).
  std::string JsonDelete(std::string const& bucket, std::string const& object) override {
    auto s = client_.DeleteObject(bucket, object);
    return s.ok() ? std::string() : s.ToString();
  }

 private:
  gcs_rust::Client client_;
  std::string bucket_;
  std::optional<gcs_rust::ObjectDescriptor> desc_;
};

}  // namespace

int main(int argc, char* argv[]) {
  auto args = ssb::ParseArgs(argc, argv);
  std::cout << "core: " << gcs_rust::Client::CoreVersion() << "\n";
  auto client = gcs_rust::Client::Create();
  if (!client) {
    std::cerr << "Client::Create: " << client.status().ToString() << "\n";
    return 1;
  }
  RustWrapped target(std::move(*client), args.bucket);
  return ssb::Runner(target, args).Run();
}
