# ─────────────────────────────────────────────────────────────
# options — derivatives pricing and dealer positioning
# ─────────────────────────────────────────────────────────────
add_library(qtl_options STATIC
    blackscholes/BlackScholes.cpp
    greeks/Greeks.cpp
    iv_surface/VolatilitySurface.cpp
    gamma_exposure/GammaExposure.cpp
    dealer_positioning/DealerPositioning.cpp
)

target_include_directories(qtl_options PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(qtl_options PUBLIC qtl_core qtl_analytics)
