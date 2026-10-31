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
    # Foundations: data, JSON, parameters, statistics
    src/core/date.cpp
    src/core/identifier.cpp
    src/core/json_value.cpp
    src/core/market_data.cpp
    src/core/parameter_grid.cpp
    src/core/performance_stats.cpp

    # Trading model: portfolio, execution, engine
    src/trading/backtest_engine.cpp
    src/trading/execution.cpp
    src/trading/portfolio.cpp

    # Built-in strategies
    src/strategies/donchian_channel_breakout.cpp
    src/strategies/moving_average_crossover.cpp
    src/strategies/rsi_mean_reversion.cpp

    # Experiment schema, validation, and execution
    src/experiment/experiment.cpp
    src/experiment/experiment_parse.cpp
    src/experiment/experiment_result_json.cpp
    src/experiment/experiment_runner.cpp
    src/experiment/experiment_validate.cpp
    src/experiment/strategy_registry.cpp

    # Coordinator <-> worker wire protocol
    src/protocol/protocol_envelope.cpp
    src/protocol/protocol_job_parse.cpp
    src/protocol/protocol_json.cpp
    src/protocol/protocol_result_parse.cpp
    src/protocol/protocol_validate.cpp
)

target_include_directories(backtester_core PUBLIC include/backtester)

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

# Creates a test executable from a path relative to tests/, links it
# against the core library, and registers it with CTest. An optional
# WORKING_DIRECTORY is forwarded to add_test for tests that read
# repository-relative data.
function(add_backtester_test target source)
    cmake_parse_arguments(
        TEST "" "WORKING_DIRECTORY" "" ${ARGN}
    )

    add_executable(${target} tests/${source})
    target_link_libraries(${target} PRIVATE backtester_core)

    if(TEST_WORKING_DIRECTORY)
        add_test(
            NAME ${target}
            COMMAND ${target}
            WORKING_DIRECTORY ${TEST_WORKING_DIRECTORY}
        )
    else()
        add_test(
            NAME ${target}
            COMMAND ${target}
        )
    endif()
endfunction()

# Foundations
add_backtester_test(json_value_tests core/json_value_test.cpp)
add_backtester_test(market_data_tests core/market_data_test.cpp)
add_backtester_test(parameter_grid_tests core/parameter_grid_test.cpp)
add_backtester_test(performance_stats_tests core/performance_stats_test.cpp)

# Trading model
add_backtester_test(
    backtest_engine_tests trading/backtest_engine_test.cpp
)
add_backtester_test(execution_tests trading/execution_test.cpp)
add_backtester_test(portfolio_tests trading/portfolio_test.cpp)
add_backtester_test(
    accounting_invariants_tests
    trading/accounting_invariants_test.cpp
)

# Strategies
add_backtester_test(
    donchian_channel_breakout_tests
    strategies/donchian_channel_breakout_test.cpp
)
add_backtester_test(
    moving_average_crossover_tests
    strategies/moving_average_crossover_test.cpp
)
add_backtester_test(
    rsi_mean_reversion_tests
    strategies/rsi_mean_reversion_test.cpp
)

# Experiment schema, service entry point, and execution
add_backtester_test(experiment_tests experiment/experiment_test.cpp)
add_backtester_test(
    parallel_execution_tests experiment/parallel_execution_test.cpp
)

# Coordinator <-> worker wire protocol
add_backtester_test(protocol_tests protocol/protocol_test.cpp)

# Integration tests
add_backtester_test(
    market_data_integration_test
    integration/market_data_integration_test.cpp
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
add_backtester_test(
    parameter_grid_backtest_integration_test
    integration/parameter_grid_backtest_integration_test.cpp
)
add_backtester_test(
    protocol_examples_test
    integration/protocol_examples_test.cpp
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# End-to-end test: runs the backtester executable against real market data.
add_backtester_test(
    backtester_end_to_end_test
    integration/backtester_end_to_end_test.cpp
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
target_compile_definitions(
    backtester_end_to_end_test
    PRIVATE "EXECUTABLE_PATH=\"$<TARGET_FILE:backtester>\""
)
