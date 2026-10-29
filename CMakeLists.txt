cmake_minimum_required(VERSION 3.16)
project(brotli_bench LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMMON_OPT_FLAGS -O3 -march=native -g)
set(BROTLI_C_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/brotli-c/c/include")

# Rust release-profile knobs (defaults live in `rust/Cargo.toml`).
set(BROTLI_BENCH_RUST_LTO "fat" CACHE STRING
  "Cargo `profile.release.lto` for the Rust FFI (fat, thin, off)")
set(BROTLI_BENCH_RUST_PANIC "abort" CACHE STRING
  "Cargo `profile.release.panic` for the Rust FFI (abort, unwind)")
set(BROTLI_BENCH_RUSTFLAGS "-C target-cpu=native" CACHE STRING
  "RUSTFLAGS used when building the Rust FFI")

# 1. Google C Reference Implementation (`brotli_c`)
file(GLOB BROTLI_C_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/brotli-c/c/common/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/brotli-c/c/dec/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/brotli-c/c/enc/*.c"
)

add_library(brotli_c STATIC ${BROTLI_C_SOURCES})
target_include_directories(brotli_c PUBLIC "${BROTLI_C_INCLUDE_DIR}")
target_compile_options(brotli_c PRIVATE ${COMMON_OPT_FLAGS})
target_link_libraries(brotli_c PUBLIC m)

# 2. Rust C-FFI static library (`libbrotli_bench_ffi.a`), built from the
# wrapper crate in `rust/`, which patches `brotli` / `brotli-decompressor` to
# the `vendor/` submodules. Override the toolchain with
# `-DCARGO_EXECUTABLE=/path/to/cargo`.
find_program(CARGO_EXECUTABLE cargo
  HINTS
    "$ENV{CARGO_HOME}/bin"
    "$ENV{HOME}/.cargo/bin"
    "$ENV{HOME}/.local/bin"
  REQUIRED
)
get_filename_component(CARGO_BIN_DIR "${CARGO_EXECUTABLE}" DIRECTORY)

set(RUST_FFI_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml")
set(RUST_FFI_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo-target")
set(RUST_FFI_LIB "${RUST_FFI_TARGET_DIR}/release/libbrotli_bench_ffi.a")

add_custom_target(cargo_brotli_ffi ALL
  COMMAND "${CMAKE_COMMAND}" -E env
    "PATH=${CARGO_BIN_DIR}:$ENV{PATH}"
    "RUSTFLAGS=${BROTLI_BENCH_RUSTFLAGS}"
    "CARGO_PROFILE_RELEASE_LTO=${BROTLI_BENCH_RUST_LTO}"
    "CARGO_PROFILE_RELEASE_PANIC=${BROTLI_BENCH_RUST_PANIC}"
    "${CARGO_EXECUTABLE}" build --release
      --manifest-path "${RUST_FFI_MANIFEST}"
      --target-dir "${RUST_FFI_TARGET_DIR}"
  BYPRODUCTS "${RUST_FFI_LIB}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/rust"
  COMMENT "Building Rust FFI staticlib (libbrotli_bench_ffi.a) via cargo"
  USES_TERMINAL
  VERBATIM
)

find_package(Threads REQUIRED)

add_library(brotli_rust_ffi STATIC IMPORTED GLOBAL)
set_target_properties(brotli_rust_ffi PROPERTIES
  IMPORTED_LOCATION "${RUST_FFI_LIB}"
  INTERFACE_INCLUDE_DIRECTORIES "${BROTLI_C_INCLUDE_DIR}"
  INTERFACE_LINK_LIBRARIES "Threads::Threads;dl;m"
)
add_dependencies(brotli_rust_ffi cargo_brotli_ffi)

# 3. Symbol-prefixed C reference library (`libbrotli_c_prefixed.a`): every
# global symbol of `brotli_c` renamed `C_*` so it can be linked next to the
# Rust FFI (which owns the unprefixed `Brotli*` names).
find_program(NM_EXECUTABLE nm REQUIRED)
find_program(OBJCOPY_EXECUTABLE objcopy REQUIRED)

set(C_SYMS_MAP "${CMAKE_CURRENT_BINARY_DIR}/c_syms.map")
set(BROTLI_C_PREFIXED_LIB "${CMAKE_CURRENT_BINARY_DIR}/libbrotli_c_prefixed.a")

add_custom_command(
  OUTPUT "${BROTLI_C_PREFIXED_LIB}" "${C_SYMS_MAP}"
  COMMAND sh -c "\"${NM_EXECUTABLE}\" --defined-only -g \"$<TARGET_FILE:brotli_c>\" | awk '$3 != \"\" {print $3, \"C_\"$3}' > \"${C_SYMS_MAP}\" && \"${OBJCOPY_EXECUTABLE}\" --redefine-syms=\"${C_SYMS_MAP}\" \"$<TARGET_FILE:brotli_c>\" \"${BROTLI_C_PREFIXED_LIB}\""
  DEPENDS brotli_c
  COMMENT "Generating symbol-prefixed C reference library libbrotli_c_prefixed.a"
  VERBATIM
)

add_custom_target(brotli_c_prefixed_target DEPENDS "${BROTLI_C_PREFIXED_LIB}")

add_library(brotli_c_prefixed STATIC IMPORTED GLOBAL)
set_target_properties(brotli_c_prefixed PROPERTIES
  IMPORTED_LOCATION "${BROTLI_C_PREFIXED_LIB}"
  INTERFACE_INCLUDE_DIRECTORIES "${BROTLI_C_INCLUDE_DIR}"
  INTERFACE_LINK_LIBRARIES "m"
)
add_dependencies(brotli_c_prefixed brotli_c_prefixed_target)

# 4. Executables. All of them also link `brotli_c_prefixed`; `bench_c` /
# `bench_rust` use it only to pre-encode decode workloads so both decode
# byte-identical C-encoded streams.
function(brotli_bench_executable name source)
  add_executable(${name} ${source})
  target_compile_options(${name} PRIVATE ${COMMON_OPT_FLAGS})
  target_compile_definitions(${name} PRIVATE
    BROTLI_BENCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  )
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endfunction()

# 4a. `bench_c`: `StandardBrotliApi` -> C reference.
brotli_bench_executable(bench_c src/bench_main.cc)
target_link_libraries(bench_c PRIVATE brotli_c brotli_c_prefixed)

# 4b. `bench_rust`: same source, `StandardBrotliApi` -> Rust FFI.
brotli_bench_executable(bench_rust src/bench_main.cc)
target_compile_definitions(bench_rust PRIVATE BROTLI_BENCH_STANDARD_API_IS_RUST)
target_link_libraries(bench_rust PRIVATE brotli_rust_ffi brotli_c_prefixed)

# 4c. `brotli_compare`: side-by-side verification & benchmark (Rust FFI +
# prefixed C reference).
brotli_bench_executable(brotli_compare src/compare_main.cc)
target_compile_definitions(brotli_compare PRIVATE
  BROTLI_BENCH_STANDARD_API_IS_RUST)
target_link_libraries(brotli_compare PRIVATE brotli_rust_ffi brotli_c_prefixed)
