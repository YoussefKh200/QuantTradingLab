add_library(qtl_analytics STATIC
    statistics/Statistics.cpp
    montecarlo/MonteCarlo.cpp
    optimization/ParameterOptimizer.cpp
    metrics/PerformanceMetrics.cpp
)
target_include_directories(qtl_analytics PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(qtl_analytics PUBLIC qtl_core)
