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

// Smallest useful JSON app on native google-cloud-cpp (storage, REST only):
// upload, read, delete. Used only to compare binary size with
// json_min_wrapped.cc.
#include "google/cloud/storage/client.h"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <bucket>\n";
    return 1;
  }
  namespace gcs = ::google::cloud::storage;
  std::string const bucket = argv[1];
  std::string const object = "gcs-rust-poc/json-min/native";
  auto client = gcs::Client();
  std::string const data(1024 * 1024, 'x');
  auto m = client.InsertObject(bucket, object, data);
  if (!m) return std::cerr << m.status() << "\n", 1;
  auto is = client.ReadObject(bucket, object);
  std::vector<char> buf(2 * 1024 * 1024);
  std::int64_t n = 0;
  while (is.read(buf.data(), static_cast<std::streamsize>(buf.size())), is.gcount() > 0) n += is.gcount();
  if (!is.status().ok()) return std::cerr << is.status() << "\n", 1;
  auto s = client.DeleteObject(bucket, object);
  if (!s.ok()) return std::cerr << s << "\n", 1;
  std::cout << "OK read " << n << " bytes\n";
  return 0;
}
