# Test targets. One test executable keeps link times low on Windows while the
# source tree stays split by test layer (unit / integration / concurrency).
# Larger layers (failure injection, benchmarks) arrive in later phases as
# additional executables so slow suites never block fast ones.

find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(apex_tests
  unit/config_test.cpp
  unit/router_test.cpp
  unit/idempotency_contract_test.cpp
  unit/lease_contract_test.cpp
  common/test_helpers.cpp
  common/pg_fixture.cpp
  integration/http_test.cpp
  integration/dependency_check_test.cpp
  integration/db_repository_test.cpp
  integration/http_operations_test.cpp
  integration/lease_redis_test.cpp
  integration/fencing_test.cpp
  integration/cross_system_test.cpp
  concurrency/parallel_health_test.cpp
  concurrency/idempotency_race_test.cpp
  concurrency/lease_recovery_stress_test.cpp
)

target_include_directories(apex_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Absolute path to migrations/ so PostgreSQL-gated tests can apply V001
# without depending on the process working directory.
target_compile_definitions(apex_tests PRIVATE
  "APEX_MIGRATIONS_DIR=\"${PROJECT_SOURCE_DIR}/migrations\""
)

target_link_libraries(apex_tests PRIVATE
  apex_core
  GTest::gtest
  GTest::gtest_main
)

if(MSVC)
  target_compile_options(apex_tests PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(apex_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
  target_compile_definitions(apex_tests PRIVATE
    _WIN32_WINNT=0x0A00
    NOMINMAX
    WIN32_LEAN_AND_MEAN
  )
endif()

gtest_discover_tests(apex_tests DISCOVERY_TIMEOUT 60)
