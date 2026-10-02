# Apex — Distributed Transaction Mesh & Idempotency Engine
# Phase 1: durable PostgreSQL-backed idempotency state (no Redis coordination,
# no leases, no multiplexing — those are later phases).
#
# Configure (Visual Studio generator is the supported default on Windows):
#   cmake -S . -B build -G "Visual Studio 18 2026" -A x64 `
#     -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
# Build:
#   cmake --build build --config Debug
#   cmake --build build --config Release
# Test:
#   ctest --test-dir build -C Debug --output-on-failure
#
# Dependency management: vcpkg manifest mode (see vcpkg.json). The toolchain
# file above makes CMake install the pinned dependencies automatically at
# configure time. VCPKG_ROOT must point at a bootstrapped vcpkg checkout
# (see scripts/configure.ps1 and README.md).

cmake_minimum_required(VERSION 3.28)

project(Apex
  VERSION 0.2.0
  DESCRIPTION "Distributed Transaction Mesh & Idempotency Engine"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(APEX_BUILD_TESTS "Build the automated test suite" ON)
option(APEX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

find_package(boost_asio CONFIG REQUIRED)
find_package(boost_beast CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
# libpq (durable state) + OpenSSL (SHA-256 for request fingerprints).
# Provided by vcpkg: libpq via FindPostgreSQL wrapper, OpenSSL via its
# official config. See scripts/configure.ps1 for the installed-tree path fix.
find_package(PostgreSQL REQUIRED)
find_package(OpenSSL REQUIRED)

# ---------------------------------------------------------------------------
# Shared warning policy. MSVC /W4 is the baseline; warnings-as-errors is
# opt-in because Boost.Beast template instantiations have historically been
# noisy under /WX on some MSVC versions. The policy is revisited whenever
# the toolchain or Boost version changes.
# ---------------------------------------------------------------------------
if(MSVC)
  set(APEX_WARNING_FLAGS /W4 /permissive- /EHsc)
else()
  set(APEX_WARNING_FLAGS -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# apex_core: everything except main(). Tests link this library so they
# exercise the same code that ships in the server binary.
# ---------------------------------------------------------------------------
add_library(apex_core STATIC
  src/config/Config.cpp
  src/observability/Logger.cpp
  src/api/Router.cpp
  src/execution/DependencyChecker.cpp
  src/execution/Session.cpp
  src/execution/HttpServer.cpp
  src/idempotency/IdempotencyKey.cpp
  src/idempotency/Fingerprint.cpp
  src/idempotency/SimulatedOperation.cpp
  src/idempotency/IdempotencyService.cpp
  src/persistence/PgConnection.cpp
  src/persistence/ConnectionPool.cpp
  src/persistence/IdempotencyRepository.cpp
  src/persistence/Schema.cpp
)

target_include_directories(apex_core PUBLIC src)

target_link_libraries(apex_core PUBLIC
  Boost::asio
  Boost::beast
  nlohmann_json::nlohmann_json
  PostgreSQL::PostgreSQL
)

# OpenSSL::Crypto is implementation-only (Fingerprint.cpp); PRIVATE keeps it
# out of consumers' link interfaces while still propagating to the final link
# (required for static-library transitive linking).
target_link_libraries(apex_core PRIVATE OpenSSL::Crypto)

target_compile_options(apex_core PRIVATE ${APEX_WARNING_FLAGS})

if(APEX_WARNINGS_AS_ERRORS)
  if(MSVC)
    target_compile_options(apex_core PRIVATE /WX)
  else()
    target_compile_options(apex_core PRIVATE -Werror)
  endif()
endif()

if(WIN32)
  # Asio needs a modern Windows target; NOMINMAX/LEAN_AND_MEAN keep the
  # Windows headers from polluting the global namespace (min/max macros).
  target_compile_definitions(apex_core PUBLIC
    _WIN32_WINNT=0x0A00
    NOMINMAX
    WIN32_LEAN_AND_MEAN
  )
endif()

# ---------------------------------------------------------------------------
# apex: the server binary. Small by design — all logic lives in apex_core.
# ---------------------------------------------------------------------------
add_executable(apex src/main.cpp)

target_link_libraries(apex PRIVATE apex_core)
target_compile_options(apex PRIVATE ${APEX_WARNING_FLAGS})

if(APEX_WARNINGS_AS_ERRORS AND MSVC)
  target_compile_options(apex PRIVATE /WX)
endif()

target_compile_definitions(apex PRIVATE APEX_VERSION="${PROJECT_VERSION}-phase1")

# ---------------------------------------------------------------------------
# Tests (separate target tree; see tests/CMakeLists.txt).
# ---------------------------------------------------------------------------
if(APEX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
