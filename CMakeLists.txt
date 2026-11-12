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
    src/experiment/experiment_aggregate.cpp
    src/experiment/experiment_parse.cpp
    src/experiment/experiment_result_json.cpp
    src/experiment/experiment_runner.cpp
    src/experiment/experiment_validate.cpp
    src/experiment/result_collector.cpp
    src/experiment/strategy_registry.cpp

    # Coordinator <-> worker wire protocol
    src/protocol/protocol_envelope.cpp
    src/protocol/protocol_job_parse.cpp
    src/protocol/protocol_json.cpp
    src/protocol/protocol_result_parse.cpp
    src/protocol/protocol_shutdown_parse.cpp
    src/protocol/protocol_validate.cpp
    src/protocol/protocol_worker_parse.cpp

    # TCP framing and socket helpers
    src/net/tcp.cpp

    # Communication layer: one JSON document per channel operation
    src/comm/tcp_channel.cpp

    # Worker loop and its transports
    src/worker/dataset_cache.cpp
    src/worker/stdio_transport.cpp
    src/worker/worker.cpp
    src/worker/worker_job.cpp

    # Coordinator scheduling and aggregation
    src/coordinator/coordinator.cpp
    src/coordinator/job_queue.cpp
    src/coordinator/job_state.cpp
    src/coordinator/persistent_state.cpp
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
# Worker executable
# -------------------------------------------------------------------

add_executable(backtester_worker
    src/worker_main.cpp
)

target_link_libraries(backtester_worker PRIVATE backtester_core)

# -------------------------------------------------------------------
# Coordinator executable
# -------------------------------------------------------------------

add_executable(backtester_coordinator
    src/coordinator_main.cpp
)

target_link_libraries(backtester_coordinator PRIVATE backtester_core)

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

# TCP helper
add_backtester_test(tcp_tests net/tcp_test.cpp)

# Communication layer
add_backtester_test(
    communication_channel_tests
    comm/message_channel_test.cpp
)

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
    result_collector_tests experiment/result_collector_test.cpp
)
add_backtester_test(
    parallel_execution_tests experiment/parallel_execution_test.cpp
)

# Coordinator <-> worker wire protocol
add_backtester_test(protocol_tests protocol/protocol_test.cpp)

# Coordinator scheduling
add_backtester_test(job_queue_tests coordinator/job_queue_test.cpp)
add_backtester_test(job_state_tests coordinator/job_state_test.cpp)
add_backtester_test(
    persistent_state_tests coordinator/persistent_state_test.cpp
)

# Worker dataset cache
add_backtester_test(
    dataset_cache_tests worker/dataset_cache_test.cpp
)

# Worker loop, job execution, and development transport
add_backtester_test(
    worker_tests worker/worker_test.cpp
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

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
add_backtester_test(
    coordinator_tcp_test
    integration/coordinator_tcp_test.cpp
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
# Real sockets; bound the runtime if the accept or dispatch loop regresses.
set_tests_properties(coordinator_tcp_test PROPERTIES TIMEOUT 30)

add_backtester_test(
    coordinator_failure_test
    integration/coordinator_failure_test.cpp
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
# Real sockets; bound the runtime if failure handling regresses.
set_tests_properties(coordinator_failure_test PROPERTIES TIMEOUT 30)
add_backtester_test(
    coordinator_disconnect_test
    integration/coordinator_disconnect_test.cpp
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
# This test uses real sockets and would otherwise hang the suite forever if
# disconnect detection regressed.
set_tests_properties(coordinator_disconnect_test PROPERTIES TIMEOUT 30)

add_backtester_test(
    coordinator_retry_test
    integration/coordinator_retry_test.cpp
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
# Real sockets and retries; bound the runtime if a retry loop regresses.
set_tests_properties(coordinator_retry_test PROPERTIES TIMEOUT 30)

add_backtester_test(
    coordinator_duplicate_test
    integration/coordinator_duplicate_test.cpp
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
# Real sockets; bound the runtime if duplicate suppression regresses.
set_tests_properties(coordinator_duplicate_test PROPERTIES TIMEOUT 30)

add_backtester_test(
    coordinator_recovery_test
    integration/coordinator_recovery_test.cpp
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
# Real sockets and a partial state file; bound the runtime if recovery hangs.
set_tests_properties(coordinator_recovery_test PROPERTIES TIMEOUT 30)

add_backtester_test(
    coordinator_timeout_test
    integration/coordinator_timeout_test.cpp
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
# Real sockets and a deliberate non-reply; a missed timeout must not hang.
set_tests_properties(coordinator_timeout_test PROPERTIES TIMEOUT 30)

add_backtester_test(
    coordinator_uneven_workload_test
    integration/coordinator_uneven_workload_test.cpp
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
# Real sockets with intentional sleeps; bound the runtime under load.
set_tests_properties(coordinator_uneven_workload_test PROPERTIES TIMEOUT 60)

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
# Spawns a process and computes a real sweep; bound the runtime on slow hosts.
set_tests_properties(backtester_end_to_end_test PROPERTIES TIMEOUT 120)

# End-to-end test: drives the worker executable through its stdio transport.
add_backtester_test(
    worker_executable_test
    integration/worker_executable_test.cpp
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
target_compile_definitions(
    worker_executable_test
    PRIVATE "EXECUTABLE_PATH=\"$<TARGET_FILE:backtester_worker>\""
)
# Spawns a process; bound the runtime if stdio framing regresses.
set_tests_properties(worker_executable_test PROPERTIES TIMEOUT 60)

# End-to-end test: drives the worker executable through its TCP client path.
add_backtester_test(
    tcp_worker_executable_test
    integration/tcp_worker_executable_test.cpp
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
target_compile_definitions(
    tcp_worker_executable_test
    PRIVATE "EXECUTABLE_PATH=\"$<TARGET_FILE:backtester_worker>\""
)
# Spawns a process over TCP; bound the runtime if connect or framing regresses.
set_tests_properties(tcp_worker_executable_test PROPERTIES TIMEOUT 60)
