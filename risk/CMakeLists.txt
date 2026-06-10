# ─────────────────────────────────────────────────────────────
# risk — real-time risk management
# ─────────────────────────────────────────────────────────────
add_library(qtl_risk STATIC
    limits/RiskLimits.cpp
    exposure/ExposureTracker.cpp
    kill_switch/KillSwitch.cpp
)

target_include_directories(qtl_risk PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(qtl_risk PUBLIC qtl_core)
