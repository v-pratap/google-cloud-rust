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

// SSB workloads on native google-cloud-cpp, using the same calls and clients as
// storage-shared-benchmarking/w1r3/cpp (release channel configuration):
//   open / random8k / read50x2x4 : AsyncClient, GRPC+DP (google-c2p:///)
//     upload     : StartAppendableObjectUpload + one Write + Finalize
//     download   : Open + ObjectDescriptor::Read (50 async chains for read50x2x4)
//     delete     : AsyncClient::DeleteObject
//   w1r3 : sync storage::Client, JSON transport (sync_w1r3_worker.cc)
//     upload     : InsertObject (SINGLE-SHOT) or WriteObject (RESUMABLE)
//     READ[0..2] : ReadObject, read into a 2 MiB discard buffer
//     delete     : DeleteObject

#include "google/cloud/storage/async/client.h"
#include "google/cloud/storage/client.h"
#include "google/cloud/storage/options.h"
#include "google/cloud/common_options.h"
#include "ssb_workloads.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>

namespace {

namespace gc = ::google::cloud;
namespace gcs = ::google::cloud::storage;

std::string Err(gc::Status const& s) {
  return s.ok() ? std::string() : gc::StatusCodeToString(s.code()) + ": " + s.message();
}

// SSB read50x2x4_worker.cc ReadUntilDone, plus a byte counter.
void ReadUntilDone(gcs::AsyncReader reader, gcs::AsyncToken token,
                   std::shared_ptr<std::atomic<std::int64_t>> bytes,
                   std::function<void(gc::Status)> on_done) {
  reader.Read(std::move(token))
      .then([reader = std::move(reader), bytes, on_done = std::move(on_done)](auto f) mutable {
        auto result = f.get();
        if (!result.ok()) return on_done(result.status());
        auto pair = *std::move(result);
        *bytes += static_cast<std::int64_t>(pair.first.size());
        if (pair.second.valid()) {
          ReadUntilDone(std::move(reader), std::move(pair.second), bytes, std::move(on_done));
        } else {
          on_done(gc::Status());
        }
      });
}

class Native : public ssb::Target {
 public:
  explicit Native(std::string bucket)
      : client_(AsyncOptions()), json_(JsonOptions()), bucket_(std::move(bucket)) {}

  std::string Name() const override { return "native_cpp"; }

  // SSB make_async_clients(): GRPC+DP = AsyncClient on google-c2p:///.
  static gc::Options AsyncOptions() {
    return gc::Options{}
        .set<gcs::UploadBufferSizeOption>(256 * 1024)
        .set<gc::EndpointOption>("google-c2p:///storage.googleapis.com");
  }
  // SSB make_sync_clients(): JSON = storage::Client(options).
  static gc::Options JsonOptions() {
    return gc::Options{}.set<gcs::UploadBufferSizeOption>(256 * 1024);
  }

  // --- w1r3: sync client, JSON (SSB sync_w1r3_worker.cc) --------------------
  std::string JsonUpload(std::string const& bucket, std::string const& object, char const* data,
                         std::int64_t size, bool resumable) override {
    if (!resumable) {  // insert_object()
      auto r = json_.InsertObject(bucket, object,
                                  std::string_view{data, static_cast<std::size_t>(size)});
      return Err(r.status());
    }
    // write_object(): buffer.size() >= object size, so a single write() call.
    auto os = json_.WriteObject(bucket, object);
    os.write(data, size);
    os.Close();
    return Err(os.metadata().status());
  }

  std::string JsonRead(std::string const& bucket, std::string const& object,
                       std::int64_t* bytes) override {
    auto is = json_.ReadObject(bucket, object);
    std::vector<char> discard(2 * 1024 * 1024);  // as SSB read_object()
    std::int64_t got = 0;
    while (true) {
      is.read(discard.data(), static_cast<std::streamsize>(discard.size()));
      got += is.gcount();
      if (!is.good()) break;
    }
    *bytes = got;
    return Err(is.status());
  }

  std::string JsonDelete(std::string const& bucket, std::string const& object) override {
    return Err(json_.DeleteObject(bucket, object));
  }

  std::string Upload(std::string const& object, char const* data,
                     std::int64_t size) override {
    auto start = client_.StartAppendableObjectUpload(gcs::BucketName(bucket_), object).get();
    if (!start) return Err(start.status());
    auto [writer, token] = *std::move(start);
    // SSB copies the vector into the payload too: WritePayload(*data).
    auto w = writer.Write(std::move(token),
                          gcs::WritePayload(std::string(data, static_cast<std::size_t>(size))))
                 .get();
    if (!w) return Err(w.status());
    auto m = writer.Finalize(*std::move(w)).get();
    return Err(m.status());
  }

  std::string Open(std::string const& object) override {
    auto d = client_.Open(gcs::BucketName(bucket_), object).get();
    if (!d) return Err(d.status());
    desc_.emplace(*std::move(d));
    return {};
  }

  std::string Read(std::int64_t offset, std::int64_t length, std::int64_t* bytes) override {
    auto [reader, token] = length == 0 ? desc_->ReadFromOffset(offset)
                                       : desc_->Read(offset, length);
    std::int64_t got = 0;
    while (token.valid()) {
      auto r = reader.Read(std::move(token)).get();
      if (!r) return Err(r.status());
      got += static_cast<std::int64_t>(r->first.size());
      token = std::move(r->second);
    }
    *bytes = got;
    return {};
  }

  std::string ReadConcurrent(std::vector<std::pair<std::int64_t, std::int64_t>> const& ranges,
                             std::int64_t* bytes) override {
    struct State {
      std::mutex mu;
      std::condition_variable cv;
      int done = 0;
      std::string first_error;
    };
    auto state = std::make_shared<State>();
    auto counter = std::make_shared<std::atomic<std::int64_t>>(0);
    int const n = static_cast<int>(ranges.size());
    for (auto const& [off, len] : ranges) {
      auto [reader, token] = desc_->Read(off, len);
      ReadUntilDone(std::move(reader), std::move(token), counter, [state](gc::Status s) {
        std::lock_guard<std::mutex> lk(state->mu);
        if (!s.ok() && state->first_error.empty()) state->first_error = Err(s);
        ++state->done;
        state->cv.notify_one();
      });
    }
    std::unique_lock<std::mutex> lk(state->mu);
    if (!state->cv.wait_for(lk, std::chrono::seconds(120), [&] { return state->done == n; })) {
      return "Download timeout";
    }
    *bytes = counter->load();
    return state->first_error;
  }

  // As SSB open_with_initial_range_worker.cc: the range is requested in the
  // same BidiReadObject call that opens the object, then drained via Read().
  std::string OpenAndRead(std::string const& object, std::int64_t offset, std::int64_t length,
                          std::int64_t* bytes) override {
    gcs::AsyncClient::InitialReadRanges cfg;
    cfg.initial_ranges = {{offset, length}};
    auto d = client_.Open(gcs::BucketName(bucket_), object, std::move(cfg)).get();
    if (!d) return Err(d.status());
    desc_.emplace(*std::move(d));
    return Read(offset, length, bytes);
  }

  std::string ReadConcurrentInCore(std::vector<std::pair<std::int64_t, std::int64_t>> const& ranges,
                                   std::int64_t* bytes) override {
    return ReadConcurrent(ranges, bytes);  // native is already async, no extra threads
  }

  void CloseDescriptor() override { desc_.reset(); }

  std::string Delete(std::string const& object) override {
    return Err(client_.DeleteObject(gcs::BucketName(bucket_), object).get());
  }

 private:
  gcs::AsyncClient client_;  // GRPC+DP: open, random8k, read50x2x4
  gcs::Client json_;         // JSON:    w1r3
  std::string bucket_;
  std::optional<gcs::ObjectDescriptor> desc_;
};

}  // namespace

int main(int argc, char* argv[]) {
  auto args = ssb::ParseArgs(argc, argv);
  Native target(args.bucket);
  return ssb::Runner(target, args).Run();
}
