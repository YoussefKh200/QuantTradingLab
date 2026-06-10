# ─────────────────────────────────────────────────────────────
# tests — unit and integration tests
# Uses minimal hand-rolled test runner (no external dependency
# in Phase 1). GoogleTest will be wired in Phase 2 onwards.
# ─────────────────────────────────────────────────────────────
add_executable(qtl_tests
    test_main.cpp
    test_event_system.cpp
    test_order_book.cpp
    test_matching_engine.cpp
    test_risk_engine.cpp
    test_portfolio.cpp
    test_black_scholes.cpp
    test_performance_metrics.cpp
)

target_include_directories(qtl_tests PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(qtl_tests
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

add_test(NAME QuantTradingLabTests COMMAND qtl_tests)
