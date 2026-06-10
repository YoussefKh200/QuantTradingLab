# ─────────────────────────────────────────────────────────────
# exchange — simulated exchange infrastructure
#   orderbook  : limit order book with price-level management
#   matching   : price-time priority matching engine
#   marketdata : tick replay, quote/trade feed
#   execution  : execution report generation
# ─────────────────────────────────────────────────────────────
add_library(qtl_exchange STATIC
    orderbook/Order.cpp
    orderbook/OrderBook.cpp
    matching/MatchingEngine.cpp
    marketdata/MarketDataFeed.cpp
    execution/ExecutionReport.cpp
)

target_include_directories(qtl_exchange PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(qtl_exchange PUBLIC qtl_core)
