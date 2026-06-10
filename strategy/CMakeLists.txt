# ─────────────────────────────────────────────────────────────
# strategy — pluggable strategy framework
# ─────────────────────────────────────────────────────────────
add_library(qtl_strategy STATIC
    market_making/MarketMakingStrategy.cpp
    mean_reversion/MeanReversionStrategy.cpp
    momentum/MomentumStrategy.cpp
    orderflow/OrderFlowStrategy.cpp
)

target_include_directories(qtl_strategy PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(qtl_strategy PUBLIC qtl_core qtl_exchange)
