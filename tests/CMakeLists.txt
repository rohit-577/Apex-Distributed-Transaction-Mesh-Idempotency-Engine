# Test targets. apex_tests is the ctest suite (discovered per-test);
# apex_bench is the Phase 6 benchmark runner (NOT part of ctest — it prints
# measured numbers, never asserts performance). Both share apex_test_support
# (fixtures, HTTP client, executors) so benchmarks exercise the same paths
# as tests without duplicating harness code.

find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_library(apex_test_support STATIC
  common/test_helpers.cpp
  common/pg_fixture.cpp
)

target_include_directories(apex_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(apex_test_support PUBLIC
  apex_core
  GTest::gtest
)

if(MSVC)
  target_compile_options(apex_test_support PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(apex_test_support PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
  target_compile_definitions(apex_test_support PUBLIC
    _WIN32_WINNT=0x0A00
    NOMINMAX
    WIN32_LEAN_AND_MEAN
  )
endif()

# Absolute paths so gated tests/benchmarks work regardless of the process
# working directory.
target_compile_definitions(apex_test_support PUBLIC
  "APEX_MIGRATIONS_DIR=\"${PROJECT_SOURCE_DIR}/migrations\""
  "APEX_COMPOSE_FILE=\"${PROJECT_SOURCE_DIR}/docker-compose.yml\""
)

add_executable(apex_tests
  unit/config_test.cpp
  unit/router_test.cpp
  unit/idempotency_contract_test.cpp
  unit/lease_contract_test.cpp
  unit/waiter_registry_test.cpp
  integration/http_test.cpp
  integration/dependency_check_test.cpp
  integration/db_repository_test.cpp
  integration/http_operations_test.cpp
  integration/lease_redis_test.cpp
  integration/fencing_test.cpp
  integration/cross_system_test.cpp
  integration/waiter_lifecycle_test.cpp
  integration/subscriber_spike_test.cpp
  integration/reaper_test.cpp
  integration/resilience_docker_test.cpp
  integration/metrics_test.cpp
  integration/migration_test.cpp
  integration/storm_isolation_test.cpp
  concurrency/parallel_health_test.cpp
  concurrency/idempotency_race_test.cpp
  concurrency/lease_recovery_stress_test.cpp
  concurrency/multiplexing_test.cpp
  concurrency/cross_node_test.cpp
)

target_link_libraries(apex_tests PRIVATE
  apex_test_support
  GTest::gtest_main
)

if(MSVC)
  target_compile_options(apex_tests PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(apex_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

gtest_discover_tests(apex_tests DISCOVERY_TIMEOUT 60)

add_executable(apex_bench
  benchmark/bench.cpp
)

target_link_libraries(apex_bench PRIVATE
  apex_test_support
)

if(MSVC)
  target_compile_options(apex_bench PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(apex_bench PRIVATE -Wall -Wextra -Wpedantic)
endif()
