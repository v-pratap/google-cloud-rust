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

// Shared benchmark harness so that the Rust-wrapped client and the native
// google-cloud-cpp client run the *identical* Rapid-bucket workload:
//
//   write  : N appendable objects of S MiB, appended in C MiB chunks (BidiWrite)
//   read   : full sequential read of each object (BidiRead, one range)
//   random : T threads issuing R random ranged reads of K KiB on ONE shared
//            descriptor (BidiRead multiplexing) -> latency p50/p90/p99
//   list   : L ListObjects calls over the benchmark prefix
//
// Output: a human-readable table plus `CSV,...` lines for easy comparison.

#ifndef GCS_RUST_POC_BENCH_COMMON_H
#define GCS_RUST_POC_BENCH_COMMON_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>

namespace bench {

struct Args {
  std::string bucket;
  std::string prefix = "gcs-rust-poc/bench/";
  std::size_t object_size_mib = 256;
  std::size_t chunk_mib = 2;
  int objects = 3;
  std::size_t range_kib = 1024;
  int random_reads = 512;
  int threads = 16;
  int list_iterations = 5;
  bool keep = false;
};

inline Args ParseArgs(int argc, char* argv[]) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto eq = arg.find('=');
    auto key = arg.substr(0, eq);
    auto val = eq == std::string::npos ? std::string() : arg.substr(eq + 1);
    if (key == "--bucket") a.bucket = val;
    else if (key == "--prefix") a.prefix = val;
    else if (key == "--object-size-mib") a.object_size_mib = std::stoul(val);
    else if (key == "--chunk-mib") a.chunk_mib = std::stoul(val);
    else if (key == "--objects") a.objects = std::stoi(val);
    else if (key == "--range-kib") a.range_kib = std::stoul(val);
    else if (key == "--random-reads") a.random_reads = std::stoi(val);
    else if (key == "--threads") a.threads = std::stoi(val);
    else if (key == "--list-iterations") a.list_iterations = std::stoi(val);
    else if (key == "--keep") a.keep = true;
    else {
      std::cerr << "unknown flag: " << arg << "\n"
                << "flags: --bucket=B [--prefix=P] [--object-size-mib=256] "
                   "[--chunk-mib=2] [--objects=3] [--range-kib=1024] "
                   "[--random-reads=512] [--threads=16] [--list-iterations=5] "
                   "[--keep]\n";
      std::exit(2);
    }
  }
  if (a.bucket.empty()) {
    std::cerr << "--bucket is required\n";
    std::exit(2);
  }
  return a;
}

/// What each implementation must provide. Errors are returned as strings
/// (empty == OK) so the harness is independent of either Status type.
class Target {
 public:
  virtual ~Target() = default;
  virtual std::string Name() const = 0;
  virtual std::string Write(std::string const& object, std::string const& data,
                            std::size_t chunk) = 0;
  /// Opens a descriptor that later ReadRange calls (possibly concurrent) use.
  virtual std::string Open(std::string const& object) = 0;
  virtual std::string ReadRange(std::uint64_t offset, std::uint64_t length,
                                std::uint64_t* bytes) = 0;
  virtual std::string CloseDescriptor() = 0;
  virtual std::string List(std::string const& prefix, std::uint64_t* count) = 0;
  virtual std::string Delete(std::string const& object) = 0;
};

using Clock = std::chrono::steady_clock;
inline double Ms(Clock::duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}

inline double Pct(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  auto idx = static_cast<std::size_t>(p / 100.0 * (v.size() - 1) + 0.5);
  return v[std::min(idx, v.size() - 1)];
}

inline long PeakRssKiB() {
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("VmHWM:", 0) == 0) return std::stol(line.substr(6));
  }
  return -1;
}

inline long FileSizeBytes(char const* path) {
  struct stat st{};
  return ::stat(path, &st) == 0 ? static_cast<long>(st.st_size) : -1;
}

[[noreturn]] inline void Die(std::string const& what, std::string const& err) {
  std::cerr << "FAILED " << what << ": " << err << "\n";
  std::exit(1);
}

inline int Run(Target& t, Args const& a, char const* argv0) {
  auto const size = a.object_size_mib * 1024 * 1024;
  auto const chunk = a.chunk_mib * 1024 * 1024;
  auto const range = a.range_kib * 1024;
  auto const run_id = std::to_string(
      std::chrono::system_clock::now().time_since_epoch().count());
  auto const prefix = a.prefix + t.Name() + "/" + run_id + "/";
  std::vector<std::string> names;
  for (int i = 0; i < a.objects; ++i) names.push_back(prefix + "obj-" + std::to_string(i));

  std::cout << "impl=" << t.Name() << " bucket=" << a.bucket << " objects=" << a.objects
            << " size=" << a.object_size_mib << "MiB chunk=" << a.chunk_mib
            << "MiB range=" << a.range_kib << "KiB threads=" << a.threads
            << " random_reads=" << a.random_reads << "\n";

  std::string data(size, '\0');
  std::mt19937_64 gen(1234);
  for (std::size_t i = 0; i + 8 <= size; i += 8) {
    auto v = gen();
    std::memcpy(&data[i], &v, 8);
  }

  std::ostringstream csv;
  auto emit = [&](std::string const& phase, std::string const& metric, double v) {
    csv << "CSV," << t.Name() << "," << phase << "," << metric << "," << std::fixed
        << std::setprecision(3) << v << "\n";
  };

  // --- write ------------------------------------------------------------------
  std::vector<double> write_mibps, write_ms;
  for (auto const& n : names) {
    auto t0 = Clock::now();
    if (auto e = t.Write(n, data, chunk); !e.empty()) Die("write " + n, e);
    auto ms = Ms(Clock::now() - t0);
    write_ms.push_back(ms);
    write_mibps.push_back(a.object_size_mib / (ms / 1000.0));
  }

  // --- full read --------------------------------------------------------------
  std::vector<double> read_mibps, open_ms;
  for (auto const& n : names) {
    auto t0 = Clock::now();
    if (auto e = t.Open(n); !e.empty()) Die("open " + n, e);
    open_ms.push_back(Ms(Clock::now() - t0));
    std::uint64_t got = 0;
    if (auto e = t.ReadRange(0, 0, &got); !e.empty()) Die("read " + n, e);
    auto ms = Ms(Clock::now() - t0);
    if (got != size) Die("read " + n, "short read: " + std::to_string(got));
    read_mibps.push_back(a.object_size_mib / (ms / 1000.0));
    t.CloseDescriptor();
  }

  // --- random ranged reads, concurrent on one descriptor ----------------------
  if (auto e = t.Open(names.front()); !e.empty()) Die("open (random)", e);
  std::vector<double> lat;
  std::mutex mu;
  std::atomic<int> next{0};
  std::atomic<std::uint64_t> total{0};
  auto t0 = Clock::now();
  std::vector<std::thread> pool;
  for (int w = 0; w < a.threads; ++w) {
    pool.emplace_back([&, w] {
      std::mt19937_64 g(w + 1);
      std::uniform_int_distribution<std::uint64_t> pick(0, size - range);
      std::vector<double> local;
      while (next.fetch_add(1) < a.random_reads) {
        auto off = pick(g);
        std::uint64_t got = 0;
        auto s = Clock::now();
        if (auto e = t.ReadRange(off, range, &got); !e.empty()) Die("random read", e);
        local.push_back(Ms(Clock::now() - s));
        total += got;
      }
      std::lock_guard<std::mutex> lk(mu);
      lat.insert(lat.end(), local.begin(), local.end());
    });
  }
  for (auto& th : pool) th.join();
  auto random_wall = Ms(Clock::now() - t0);
  t.CloseDescriptor();
  auto random_mibps = (total.load() / 1048576.0) / (random_wall / 1000.0);

  // --- list -------------------------------------------------------------------
  std::vector<double> list_ms;
  std::uint64_t listed = 0;
  for (int i = 0; i < a.list_iterations; ++i) {
    auto s = Clock::now();
    if (auto e = t.List(prefix, &listed); !e.empty()) Die("list", e);
    list_ms.push_back(Ms(Clock::now() - s));
  }
  if (listed != names.size()) Die("list", "expected " + std::to_string(names.size()) +
                                              " objects, got " + std::to_string(listed));

  if (!a.keep) {
    for (auto const& n : names) t.Delete(n);
  }

  auto avg = [](std::vector<double> const& v) {
    double s = 0;
    for (auto x : v) s += x;
    return v.empty() ? 0 : s / v.size();
  };
  std::cout << std::fixed << std::setprecision(1)
            << "  write  (BidiWrite) : avg " << avg(write_mibps) << " MiB/s, avg "
            << avg(write_ms) << " ms/object\n"
            << "  read   (BidiRead)  : avg " << avg(read_mibps) << " MiB/s, open p50 "
            << Pct(open_ms, 50) << " ms\n"
            << "  random (BidiRead)  : p50 " << Pct(lat, 50) << " ms, p90 " << Pct(lat, 90)
            << " ms, p99 " << Pct(lat, 99) << " ms, " << random_mibps << " MiB/s aggregate\n"
            << "  list   (ListObj.)  : p50 " << Pct(list_ms, 50) << " ms, p99 "
            << Pct(list_ms, 99) << " ms\n"
            << "  binary size        : " << FileSizeBytes(argv0) / 1048576.0 << " MiB\n"
            << "  peak RSS           : " << PeakRssKiB() / 1024.0 << " MiB\n";

  emit("write", "avg_mibps", avg(write_mibps));
  emit("read", "avg_mibps", avg(read_mibps));
  emit("read", "open_p50_ms", Pct(open_ms, 50));
  emit("random", "p50_ms", Pct(lat, 50));
  emit("random", "p90_ms", Pct(lat, 90));
  emit("random", "p99_ms", Pct(lat, 99));
  emit("random", "agg_mibps", random_mibps);
  emit("list", "p50_ms", Pct(list_ms, 50));
  emit("list", "p99_ms", Pct(list_ms, 99));
  emit("process", "binary_mib", FileSizeBytes(argv0) / 1048576.0);
  emit("process", "peak_rss_mib", PeakRssKiB() / 1024.0);
  std::cout << csv.str();
  return 0;
}

}  // namespace bench

#endif  // GCS_RUST_POC_BENCH_COMMON_H
