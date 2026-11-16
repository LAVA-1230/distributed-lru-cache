cmake_minimum_required(VERSION 3.16)
project(distributed_lru_cache CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to Release if no build type is given (Release adds -O2 below)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

# Optional ThreadSanitizer build for finding data races
option(USE_TSAN "Build with ThreadSanitizer" OFF)
if(USE_TSAN)
  add_compile_options(-fsanitize=thread -g)
  add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

# ---- Library: every src/*.cpp except the *_main.cpp files ----
file(GLOB LIB_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(FILTER LIB_SOURCES EXCLUDE REGEX ".*_main\\.cpp$")

add_library(dcache_lib STATIC ${LIB_SOURCES})
target_include_directories(dcache_lib PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(dcache_lib PUBLIC Threads::Threads)

# ---- Executables (only added once their main file exists in a later phase) ----
if(EXISTS ${CMAKE_SOURCE_DIR}/src/node_main.cpp)
  add_executable(dcache_node src/node_main.cpp)
  target_link_libraries(dcache_node dcache_lib Threads::Threads)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/cli_main.cpp)
  add_executable(dcache_cli src/cli_main.cpp)
  target_link_libraries(dcache_cli dcache_lib Threads::Threads)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/bench_main.cpp)
  add_executable(dcache_bench src/bench_main.cpp)
  target_link_libraries(dcache_bench dcache_lib Threads::Threads)
endif()

# ---- Tests (GoogleTest) ----
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
)
FetchContent_MakeAvailable(googletest)

enable_testing()

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.cpp)
add_executable(dcache_tests ${TEST_SOURCES})
target_link_libraries(dcache_tests gtest_main dcache_lib Threads::Threads)

include(GoogleTest)
gtest_discover_tests(dcache_tests DISCOVERY_TIMEOUT 30)
