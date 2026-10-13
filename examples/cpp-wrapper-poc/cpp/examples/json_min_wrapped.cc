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

// Smallest useful JSON app on the Rust-wrapped SDK: upload, read, delete.
// Used only to compare binary size with json_min_native.cc.
#include "gcs_rust/client.h"

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <bucket>\n";
    return 1;
  }
  std::string const bucket = argv[1];
  std::string const object = "gcs-rust-poc/json-min/wrapped";
  auto client = gcs_rust::Client::Create();
  if (!client) return std::cerr << client.status().message() << "\n", 1;
  std::string const data(1024 * 1024, 'x');
  auto m = client->WriteObject(bucket, object, data.data(), data.size(), false);
  if (!m) return std::cerr << m.status().message() << "\n", 1;
  auto n = client->ReadObject(bucket, object, [](std::uint8_t const*, std::size_t) { return true; });
  if (!n) return std::cerr << n.status().message() << "\n", 1;
  auto s = client->DeleteObject(bucket, object);
  if (!s.ok()) return std::cerr << s.message() << "\n", 1;
  std::cout << "OK read " << *n << " bytes\n";
  return 0;
}
