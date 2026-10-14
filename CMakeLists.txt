cmake_minimum_required(VERSION 3.16)

project(distributed_backtester LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(backtester
    src/main.cpp
    src/market_data.cpp
)

target_include_directories(backtester PRIVATE include)

enable_testing()

add_executable(market_data_tests
    tests/market_data_test.cpp
    src/market_data.cpp
)

target_include_directories(market_data_tests PRIVATE include)

add_test(NAME market_data_tests COMMAND market_data_tests)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(backtester PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(market_data_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()