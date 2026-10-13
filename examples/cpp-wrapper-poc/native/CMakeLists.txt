cmake_minimum_required(VERSION 3.16)

# Native google-cloud-cpp baseline for the POC benchmark.
#
# google-cloud-cpp is pulled in with add_subdirectory(... EXCLUDE_FROM_ALL), so
# only what native_cpp_bench links against (storage_grpc and its deps) is built.
# This avoids the opentelemetry exporter library, which does not compile against
# the opentelemetry-cpp version currently in vcpkg.
#
# Configure:
#   cmake -S native -B native/build -GNinja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake

set(GCS_POC_GOOGLE_CLOUD_CPP_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../../google-cloud-cpp"
    CACHE PATH "google-cloud-cpp checkout")
# Use google-cloud-cpp's vcpkg.json and reuse the already-installed packages.
set(VCPKG_MANIFEST_DIR "${GCS_POC_GOOGLE_CLOUD_CPP_DIR}" CACHE PATH "")
set(VCPKG_INSTALLED_DIR "${CMAKE_CURRENT_LIST_DIR}/../native-build/vcpkg_installed"
    CACHE PATH "")

project(gcs_rust_poc_native LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GOOGLE_CLOUD_CPP_ENABLE "storage;storage_grpc" CACHE STRING "")
set(BUILD_TESTING OFF CACHE BOOL "")
set(GOOGLE_CLOUD_CPP_WITH_MOCKS OFF CACHE BOOL "")
set(GOOGLE_CLOUD_CPP_ENABLE_EXAMPLES OFF CACHE BOOL "")
set(GOOGLE_CLOUD_CPP_ENABLE_WERROR OFF CACHE BOOL "")
add_subdirectory(${GCS_POC_GOOGLE_CLOUD_CPP_DIR} google-cloud-cpp EXCLUDE_FROM_ALL)

add_executable(native_cpp_bench ../cpp/bench/native_cpp_bench.cc)
target_link_libraries(native_cpp_bench PRIVATE google-cloud-cpp::storage_grpc)

# storage-shared-benchmarking workloads (w1r3, open, random8k, read50x2x4).
add_executable(ssb_native_cpp ../cpp/bench/ssb_native_cpp.cc ../cpp/bench/alloc_counter.cc)
target_link_libraries(ssb_native_cpp PRIVATE google-cloud-cpp::storage_grpc)
