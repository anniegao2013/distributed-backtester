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

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(backtester PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()