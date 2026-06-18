cmake_minimum_required(VERSION 3.22)

project(QuantTradingLab
    VERSION 1.0.0
    DESCRIPTION "Institutional-Grade Quantitative Trading Laboratory"
    LANGUAGES CXX
)

# ─────────────────────────────────────────────────────────────
# Global C++20 standard — required for concepts, ranges, span,
# std::atomic<>, jthread, and coroutine foundations.
# ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # -std=c++20 not gnu++20

# ─────────────────────────────────────────────────────────────
# Build type defaults
# ─────────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ─────────────────────────────────────────────────────────────
# Compiler flags
# ─────────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
        -march=native           # Allow SIMD / CPU-specific optimisations
        -ffast-math             # Aggressive FP optimisation for analytics
    )

    # Per-config optimisation flags (generator expressions, one flag per call)
    add_compile_options($<$<CONFIG:Release>:-O3>)
    add_compile_options($<$<CONFIG:RelWithDebInfo>:-O2>)
    add_compile_options($<$<CONFIG:Debug>:-O0>)
    add_compile_options($<$<CONFIG:Debug>:-g3>)
endif()

# ─────────────────────────────────────────────────────────────
# Output directories — keep all binaries / libs in one place
# ─────────────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ─────────────────────────────────────────────────────────────
# Find required packages
# ─────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ─────────────────────────────────────────────────────────────
# Optional packages for visualization
# ─────────────────────────────────────────────────────────────
option(QTL_ENABLE_VISUALIZATION "Enable visualization module (requires OpenGL + Dear ImGui)" OFF)

if(QTL_ENABLE_VISUALIZATION)
    find_package(OpenGL QUIET)
    find_package(glfw3 QUIET)
    
    if(OpenGL_FOUND AND glfw3_FOUND)
        set(QTL_VISUALIZATION_AVAILABLE TRUE)
        message(STATUS "Visualization module enabled")
    else()
        set(QTL_VISUALIZATION_AVAILABLE FALSE)
        message(WARNING "Visualization dependencies not found (OpenGL/GLFW), disabling visualization")
        set(QTL_ENABLE_VISUALIZATION OFF CACHE BOOL "Enable visualization module" FORCE)
    endif()
else()
    set(QTL_VISUALIZATION_AVAILABLE FALSE)
    message(STATUS "Visualization module disabled")
endif()

# ─────────────────────────────────────────────────────────────
# Global include path — every sub-module can #include relative
# to the project root (e.g. #include "core/events/Event.hpp")
# ─────────────────────────────────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR})

# ─────────────────────────────────────────────────────────────
# Sub-modules — each adds its own static library target.
# Dependencies flow bottom-up:
#   core → exchange → strategy → risk → portfolio →
#   backtesting → analytics → options → visualization
# ─────────────────────────────────────────────────────────────
add_subdirectory(core)
add_subdirectory(exchange)
add_subdirectory(strategy)
add_subdirectory(risk)
add_subdirectory(portfolio)
add_subdirectory(backtesting)
add_subdirectory(analytics)
add_subdirectory(options)
# visualization is optional (requires OpenGL + Dear ImGui)
if(QTL_VISUALIZATION_AVAILABLE)
    add_subdirectory(visualization)
endif()

# ─────────────────────────────────────────────────────────────
# Tests
# ─────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)

# ─────────────────────────────────────────────────────────────
# Main entry-point binary
# ─────────────────────────────────────────────────────────────
add_executable(QuantTradingLab main.cpp)

target_link_libraries(QuantTradingLab
    PRIVATE
        qtl_core
        qtl_exchange
        qtl_strategy
        qtl_risk
        qtl_portfolio
        qtl_backtesting
        qtl_analytics
        qtl_options
        Threads::Threads
)

# Conditionally link visualization library if available
if(QTL_VISUALIZATION_AVAILABLE)
    target_link_libraries(QuantTradingLab PRIVATE qtl_visualization)
    target_compile_definitions(QuantTradingLab PRIVATE QTL_VISUALIZATION_ENABLED)
endif()

# ─────────────────────────────────────────────────────────────
# Install rules
# ─────────────────────────────────────────────────────────────
install(TARGETS QuantTradingLab DESTINATION bin)
