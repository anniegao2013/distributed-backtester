cmake_minimum_required(VERSION 3.16)

project(distributed_backtester LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(backtester
    src/main.cpp
    src/market_data.cpp
    src/moving_average_crossover.cpp
    src/backtest_engine.cpp
)

target_include_directories(backtester PRIVATE include)

enable_testing()

add_executable(market_data_tests
    tests/market_data_test.cpp
    src/market_data.cpp
)

target_include_directories(market_data_tests PRIVATE include)

add_test(NAME market_data_tests COMMAND market_data_tests)

add_executable(market_data_integration_test
tests/market_data_integration_test.cpp
src/market_data.cpp
)

target_include_directories(market_data_integration_test PRIVATE include)

add_test(
NAME market_data_integration_test
COMMAND market_data_integration_test
WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_executable(moving_average_crossover_tests
    tests/moving_average_crossover_test.cpp
    src/moving_average_crossover.cpp
)

target_include_directories(moving_average_crossover_tests PRIVATE include)

add_test(
    NAME moving_average_crossover_tests
    COMMAND moving_average_crossover_tests
)

add_executable(backtest_engine_tests
    tests/backtest_engine_test.cpp
    src/backtest_engine.cpp
)

target_include_directories(backtest_engine_tests PRIVATE include)

add_test(
    NAME backtest_engine_tests
    COMMAND backtest_engine_tests
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(backtester PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(market_data_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(market_data_integration_test PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(moving_average_crossover_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(backtest_engine_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()