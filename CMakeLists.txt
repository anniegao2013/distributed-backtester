cmake_minimum_required(VERSION 3.16)

project(distributed_backtester LANGUAGES CXX)

# -------------------------------------------------------------------
# Project settings
# -------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# -------------------------------------------------------------------
# Compiler warnings
# -------------------------------------------------------------------

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# -------------------------------------------------------------------
# Core library
# -------------------------------------------------------------------

add_library(backtester_core
    src/market_data.cpp
    src/moving_average_crossover.cpp
    src/rsi_mean_reversion.cpp
    src/donchian_channel_breakout.cpp
    src/backtest_engine.cpp
    src/execution.cpp
    src/portfolio.cpp
    src/parameter_grid.cpp
    src/performance_stats.cpp
)

target_include_directories(backtester_core PUBLIC include)

# -------------------------------------------------------------------
# Main executable
# -------------------------------------------------------------------

add_executable(backtester
    src/main.cpp
)

target_link_libraries(backtester PRIVATE backtester_core)

# -------------------------------------------------------------------
# Tests
# -------------------------------------------------------------------

# Market data unit tests
add_executable(market_data_tests
    tests/market_data_test.cpp
)

target_link_libraries(market_data_tests PRIVATE backtester_core)

add_test(
    NAME market_data_tests
    COMMAND market_data_tests
)

# Market data integration tests
add_executable(market_data_integration_test
    tests/market_data_integration_test.cpp
)

target_link_libraries(
    market_data_integration_test
    PRIVATE backtester_core
)

add_test(
    NAME market_data_integration_test
    COMMAND market_data_integration_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# Moving-average crossover tests
add_executable(moving_average_crossover_tests
    tests/moving_average_crossover_test.cpp
)

target_link_libraries(
    moving_average_crossover_tests
    PRIVATE backtester_core
)

add_test(
    NAME moving_average_crossover_tests
    COMMAND moving_average_crossover_tests
)

# RSI mean reversion tests
add_executable(rsi_mean_reversion_tests
    tests/rsi_mean_reversion_test.cpp
)

target_link_libraries(
    rsi_mean_reversion_tests
    PRIVATE backtester_core
)

add_test(
    NAME rsi_mean_reversion_tests
    COMMAND rsi_mean_reversion_tests
)

# Donchian channel breakout tests
add_executable(donchian_channel_breakout_tests
    tests/donchian_channel_breakout_test.cpp
)

target_link_libraries(
    donchian_channel_breakout_tests
    PRIVATE backtester_core
)

add_test(
    NAME donchian_channel_breakout_tests
    COMMAND donchian_channel_breakout_tests
)

# Backtesting engine tests
add_executable(backtest_engine_tests
    tests/backtest_engine_test.cpp
)

target_link_libraries(
    backtest_engine_tests
    PRIVATE backtester_core
)

add_test(
    NAME backtest_engine_tests
    COMMAND backtest_engine_tests
)

# Portfolio tests
add_executable(portfolio_tests
    tests/portfolio_test.cpp
)

target_link_libraries(
    portfolio_tests
    PRIVATE backtester_core
)

add_test(
    NAME portfolio_tests
    COMMAND portfolio_tests
)

# Order execution tests
add_executable(execution_tests
    tests/execution_test.cpp
)

target_link_libraries(
    execution_tests
    PRIVATE backtester_core
)

add_test(
    NAME execution_tests
    COMMAND execution_tests
)

# Accounting invariant tests
add_executable(accounting_invariants_tests
    tests/accounting_invariants_test.cpp
)

target_link_libraries(
    accounting_invariants_tests
    PRIVATE backtester_core
)

add_test(
    NAME accounting_invariants_tests
    COMMAND accounting_invariants_tests
)

# Parameter grid tests
add_executable(parameter_grid_tests
    tests/parameter_grid_test.cpp
)

target_link_libraries(
    parameter_grid_tests
    PRIVATE backtester_core
)

add_test(
    NAME parameter_grid_tests
    COMMAND parameter_grid_tests
)

# Parameter grid to backtest integration tests
add_executable(parameter_grid_backtest_integration_test
    tests/parameter_grid_backtest_integration_test.cpp
)

target_link_libraries(
    parameter_grid_backtest_integration_test
    PRIVATE backtester_core
)

add_test(
    NAME parameter_grid_backtest_integration_test
    COMMAND parameter_grid_backtest_integration_test
)

# Performance stats unit tests
add_executable(performance_stats_tests
    tests/performance_stats_test.cpp
)

target_link_libraries(
    performance_stats_tests
    PRIVATE backtester_core
)

add_test(
    NAME performance_stats_tests
    COMMAND performance_stats_tests
)

# Parallel execution tests
add_executable(parallel_execution_tests
    tests/parallel_execution_test.cpp
)

target_link_libraries(
    parallel_execution_tests
    PRIVATE backtester_core
)

add_test(
    NAME parallel_execution_tests
    COMMAND parallel_execution_tests
)

# End-to-end test: runs the backtester executable against real market data.
add_executable(backtester_end_to_end_test
    tests/backtester_end_to_end_test.cpp
)

target_compile_definitions(
    backtester_end_to_end_test
    PRIVATE "EXECUTABLE_PATH=\"$<TARGET_FILE:backtester>\""
)

add_test(
    NAME backtester_end_to_end_test
    COMMAND backtester_end_to_end_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
