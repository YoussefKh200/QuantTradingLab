add_library(qtl_backtesting STATIC
    engine/BacktestEngine.cpp
    engine/ReportGenerator.cpp
    simulator/MarketSimulator.cpp
    replay/TickReplayer.cpp
)
target_include_directories(qtl_backtesting PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(qtl_backtesting PUBLIC
    qtl_core qtl_exchange qtl_analytics)
