# ─────────────────────────────────────────────────────────────
# exchange — simulated exchange infrastructure
# ─────────────────────────────────────────────────────────────
add_library(qtl_exchange STATIC
    # Phase 3 — Order Book
    orderbook/Order.cpp
    orderbook/OrderBook.cpp
    # Phase 4 — Matching Engine
    matching/MatchingEngine.cpp
    execution/ExecutionReport.cpp
    # Phase 5 — Market Data
    marketdata/Tick.cpp
    marketdata/TickParser.cpp
    marketdata/MarketDataFeed.cpp
)

target_include_directories(qtl_exchange PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(qtl_exchange PUBLIC qtl_core)
