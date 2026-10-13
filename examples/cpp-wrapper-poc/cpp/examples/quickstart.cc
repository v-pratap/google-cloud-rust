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

// End-to-end functional check of the three POC APIs through the C++ wrapper:
//   1. BidiWrite  : create an appendable object, append, flush, finalize
//   2. BidiRead   : open it, read it all + a few ranges (concurrently), verify
//   3. ListObjects: list by prefix and find the object
//
// Usage: quickstart <bucket> [object-size-mib=8]

#include "gcs_rust/client.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

[[noreturn]] void Die(std::string const& what, gcs_rust::Status const& s) {
  std::cerr << "FAILED: " << what << ": " << s.ToString() << "\n";
  std::exit(1);
}

std::string MakePayload(std::size_t size) {
  std::string data(size, '\0');
  std::mt19937_64 gen(42);
  for (auto& c : data) c = static_cast<char>('a' + gen() % 26);
  return data;
}

double Secs(std::chrono::steady_clock::duration d) {
  return std::chrono::duration<double>(d).count();
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <bucket> [object-size-mib=8]\n";
    return 1;
  }
  std::string const bucket = argv[1];
  std::size_t const size_mib = argc > 2 ? std::stoul(argv[2]) : 8;
  std::size_t const size = size_mib * 1024 * 1024;
  std::string const prefix = "gcs-rust-poc/quickstart/";
  std::string const object =
      prefix + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

  std::cout << "core: " << gcs_rust::Client::CoreVersion() << "\n";
  auto client = gcs_rust::Client::Create();
  if (!client) Die("Client::Create", client.status());

  auto const payload = MakePayload(size);

  // 1. BidiWrite --------------------------------------------------------------
  auto t0 = std::chrono::steady_clock::now();
  auto writer = client->OpenAppendableObject(bucket, object);
  if (!writer) Die("OpenAppendableObject", writer.status());
  std::size_t const chunk = 2 * 1024 * 1024;
  for (std::size_t off = 0; off < payload.size(); off += chunk) {
    auto n = std::min(chunk, payload.size() - off);
    if (auto s = writer->Append(payload.data() + off, n); !s.ok()) Die("Append", s);
  }
  auto persisted = writer->Flush();
  if (!persisted) Die("Flush", persisted.status());
  auto meta = writer->Finalize();
  if (!meta) Die("Finalize", meta.status());
  auto t1 = std::chrono::steady_clock::now();
  std::cout << "[BidiWrite] wrote gs://" << meta->bucket << "/" << meta->name
            << " size=" << meta->size << " gen=" << meta->generation
            << " class=" << meta->storage_class << " in " << Secs(t1 - t0) << "s ("
            << size_mib / Secs(t1 - t0) << " MiB/s)\n";
  if (meta->size != static_cast<std::int64_t>(size)) {
    std::cerr << "FAILED: size mismatch\n";
    return 1;
  }

  // 2. BidiRead ---------------------------------------------------------------
  t0 = std::chrono::steady_clock::now();
  auto desc = client->OpenObject(bucket, object);
  if (!desc) Die("OpenObject", desc.status());
  auto full = desc->ReadToString(0, 0);
  if (!full) Die("Read full", full.status());
  t1 = std::chrono::steady_clock::now();
  std::cout << "[BidiRead]  full read " << full->size() << " bytes in " << Secs(t1 - t0)
            << "s (" << size_mib / Secs(t1 - t0) << " MiB/s), match="
            << (*full == payload ? "yes" : "NO") << "\n";
  if (*full != payload) return 1;

  // Concurrent ranged reads on the *same* descriptor (one bidi stream).
  struct Range { std::uint64_t off, len; };
  std::vector<Range> ranges = {{0, 1024}, {size / 2, 64 * 1024}, {size - 4096, 4096},
                               {12345, 1 << 20}};
  std::vector<std::thread> threads;
  std::vector<int> ok(ranges.size(), 0);
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    threads.emplace_back([&, i] {
      auto r = desc->ReadToString(ranges[i].off, ranges[i].len);
      ok[i] = r && *r == payload.substr(ranges[i].off, ranges[i].len);
      if (!r) std::cerr << "range " << i << ": " << r.status().ToString() << "\n";
    });
  }
  for (auto& t : threads) t.join();
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    std::cout << "[BidiRead]  range off=" << ranges[i].off << " len=" << ranges[i].len
              << " match=" << (ok[i] ? "yes" : "NO") << "\n";
    if (!ok[i]) return 1;
  }

  // 3. ListObjects ------------------------------------------------------------
  t0 = std::chrono::steady_clock::now();
  auto objects = client->ListObjects(bucket, prefix);
  if (!objects) Die("ListObjects", objects.status());
  t1 = std::chrono::steady_clock::now();
  bool found = false;
  for (auto const& o : *objects) found |= (o.name == object);
  std::cout << "[List]      " << objects->size() << " objects under " << prefix << " in "
            << Secs(t1 - t0) * 1000 << "ms, found ours=" << (found ? "yes" : "NO") << "\n";
  if (!found) return 1;

  // Error path: NOT_FOUND must surface as a C++ Status, not a crash.
  auto missing = client->OpenObject(bucket, prefix + "does-not-exist");
  std::cout << "[Errors]    open missing object -> " << missing.status().ToString().substr(0, 80)
            << "\n";

  if (auto s = client->DeleteObject(bucket, object); !s.ok()) Die("DeleteObject", s);
  std::cout << "PASS\n";
  return 0;
}
