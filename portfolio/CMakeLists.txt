# ─────────────────────────────────────────────────────────────
# portfolio — position and P&L accounting
# ─────────────────────────────────────────────────────────────
add_library(qtl_portfolio STATIC
    pnl/PnLTracker.cpp
    positions/PositionManager.cpp
    accounting/PortfolioAccountant.cpp
)

target_include_directories(qtl_portfolio PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(qtl_portfolio PUBLIC qtl_core qtl_risk)
