cmake_minimum_required(VERSION 3.16)
project(gcs_rust_poc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif ()

# ---------------------------------------------------------------------------
# 1. Build the Rust core with cargo and generate its C header with cbindgen.
#    This is the "CI/CD -> cbindgen & cargo build" box from the design doc.
# ---------------------------------------------------------------------------
find_program(CARGO cargo REQUIRED)
find_program(CBINDGEN cbindgen REQUIRED)

set(RUST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rust)
set(RUST_TARGET_DIR ${CMAKE_BINARY_DIR}/cargo)
set(RUST_STATICLIB ${RUST_TARGET_DIR}/release/libgcs_core.a)
set(GEN_INCLUDE_DIR ${CMAKE_BINARY_DIR}/generated/include)
set(RUST_HEADER ${GEN_INCLUDE_DIR}/gcs_core.h)
file(MAKE_DIRECTORY ${GEN_INCLUDE_DIR})
file(GLOB_RECURSE RUST_SOURCES CONFIGURE_DEPENDS ${RUST_DIR}/src/*.rs)

add_custom_command(
  OUTPUT ${RUST_STATICLIB}
  # Run from RUST_DIR so rust/.cargo/config.toml (unstable bidi cfg) applies.
  COMMAND ${CARGO} build --release --target-dir ${RUST_TARGET_DIR}
  WORKING_DIRECTORY ${RUST_DIR}
  DEPENDS ${RUST_SOURCES} ${RUST_DIR}/Cargo.toml
  COMMENT "cargo build (Rust core: google-cloud-storage + C ABI)"
  USES_TERMINAL)

add_custom_command(
  OUTPUT ${RUST_HEADER}
  COMMAND ${CBINDGEN} --config ${RUST_DIR}/cbindgen.toml --crate gcs_core_ffi
          --output ${RUST_HEADER} ${RUST_DIR}
  WORKING_DIRECTORY ${RUST_DIR}
  DEPENDS ${RUST_SOURCES} ${RUST_DIR}/cbindgen.toml ${RUST_STATICLIB}
  COMMENT "cbindgen -> gcs_core.h")

add_custom_target(gcs_core_build DEPENDS ${RUST_STATICLIB} ${RUST_HEADER})

find_package(Threads REQUIRED)
add_library(gcs_core STATIC IMPORTED GLOBAL)
set_target_properties(gcs_core PROPERTIES
  IMPORTED_LOCATION ${RUST_STATICLIB}
  INTERFACE_INCLUDE_DIRECTORIES ${GEN_INCLUDE_DIR})
# System libs required by a Rust staticlib on Linux.
target_link_libraries(gcs_core INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)
add_dependencies(gcs_core gcs_core_build)

# ---------------------------------------------------------------------------
# 2. The thin C++ SDK veneer.
# ---------------------------------------------------------------------------
add_library(gcs_rust cpp/src/client.cc)
target_include_directories(gcs_rust PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cpp/include)
target_link_libraries(gcs_rust PUBLIC gcs_core)
add_dependencies(gcs_rust gcs_core_build)

# ---------------------------------------------------------------------------
# 3. Examples / benchmarks.
# ---------------------------------------------------------------------------
add_executable(quickstart cpp/examples/quickstart.cc)
target_link_libraries(quickstart PRIVATE gcs_rust)

# Minimal JSON app (upload + read + delete), for binary-size comparison only.
add_executable(json_min_wrapped cpp/examples/json_min_wrapped.cc)
target_link_libraries(json_min_wrapped PRIVATE gcs_rust)

add_executable(rust_wrapped_bench cpp/bench/rust_wrapped_bench.cc)
target_link_libraries(rust_wrapped_bench PRIVATE gcs_rust)

# storage-shared-benchmarking workloads (w1r3, open, random8k, read50x2x4).
add_executable(ssb_rust_wrapped cpp/bench/ssb_rust_wrapped.cc cpp/bench/alloc_counter.cc)
target_link_libraries(ssb_rust_wrapped PRIVATE gcs_rust)

# The native google-cloud-cpp baseline (native_cpp_bench) is built by the
# separate project in native/ - see README.md.
