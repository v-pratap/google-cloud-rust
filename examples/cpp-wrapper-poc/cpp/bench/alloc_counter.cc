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

#include "alloc_counter.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
std::atomic<std::uint64_t> g_malloc_bytes{0};
std::atomic<std::uint64_t> g_malloc_calls{0};
std::atomic<std::uint64_t> g_new_bytes{0};

inline void count(std::size_t n) {
  g_malloc_bytes.fetch_add(n, std::memory_order_relaxed);
  g_malloc_calls.fetch_add(1, std::memory_order_relaxed);
}

long status_kib(char const* key) {
  FILE* f = std::fopen("/proc/self/status", "r");
  if (!f) return -1;
  char line[256];
  long v = -1;
  auto const klen = std::strlen(key);
  while (std::fgets(line, sizeof(line), f)) {
    if (std::strncmp(line, key, klen) == 0) {
      v = std::strtol(line + klen, nullptr, 10);
      break;
    }
  }
  std::fclose(f);
  return v;
}
}  // namespace

// glibc's real implementations (exported, stable ABI since GLIBC_2.2.5).
extern "C" {
void* __libc_malloc(std::size_t);
void* __libc_calloc(std::size_t, std::size_t);
void* __libc_realloc(void*, std::size_t);
void* __libc_memalign(std::size_t, std::size_t);
void __libc_free(void*);

// Interposed allocation functions: definitions in the executable take
// precedence over libc.so for every caller in the process.
void* malloc(std::size_t n) {
  count(n);
  return __libc_malloc(n);
}
void* calloc(std::size_t n, std::size_t m) {
  count(n * m);
  return __libc_calloc(n, m);
}
void* realloc(void* p, std::size_t n) {
  count(n);
  return __libc_realloc(p, n);
}
void free(void* p) { __libc_free(p); }
void* memalign(std::size_t a, std::size_t n) {
  count(n);
  return __libc_memalign(a, n);
}
void* aligned_alloc(std::size_t a, std::size_t n) {
  count(n);
  return __libc_memalign(a, n);
}
int posix_memalign(void** out, std::size_t a, std::size_t n) {
  count(n);
  void* p = __libc_memalign(a, n);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}
}  // extern "C"

// SSB-equivalent counter: C++ operator new (forwards to malloc, so the bytes
// are also included in malloc_bytes()).
void* operator new(std::size_t n) {
  g_new_bytes.fetch_add(n, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  g_new_bytes.fetch_add(n, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace alloc_counter {
std::uint64_t malloc_bytes() { return g_malloc_bytes.load(std::memory_order_relaxed); }
std::uint64_t malloc_calls() { return g_malloc_calls.load(std::memory_order_relaxed); }
std::uint64_t new_bytes() { return g_new_bytes.load(std::memory_order_relaxed); }
long rss_kib() { return status_kib("VmRSS:"); }
long peak_rss_kib() { return status_kib("VmHWM:"); }
}  // namespace alloc_counter
