# Test targets. One test executable keeps link times low on Windows while the
# source tree stays split by test layer (unit / integration / concurrency).
# Larger layers (failure injection, benchmarks) arrive in later phases as
# additional executables so slow suites never block fast ones.

find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(apex_tests
  unit/config_test.cpp
  unit/router_test.cpp
  common/test_helpers.cpp
  integration/http_test.cpp
  integration/dependency_check_test.cpp
  concurrency/parallel_health_test.cpp
)

target_include_directories(apex_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

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
