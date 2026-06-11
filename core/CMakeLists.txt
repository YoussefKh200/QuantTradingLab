# ─────────────────────────────────────────────────────────────
# core — foundational infrastructure
#   events   : event hierarchy, queue, dispatcher
#   clock    : high-resolution simulation/wall clock
#   logger   : thread-safe structured logger
#   threading: lock-free primitives, thread pool
#   config   : key-value configuration store
# ─────────────────────────────────────────────────────────────
add_library(qtl_core STATIC
    # Phase 1
    events/Event.cpp
    events/EventQueue.cpp
    events/EventDispatcher.cpp
    clock/Clock.cpp
    logger/Logger.cpp
    threading/ThreadPool.cpp
    threading/RingBuffer.cpp
    config/Config.cpp
    # Phase 2
    events/EventPool.cpp
    events/EventLoop.cpp
    events/SPSCEventQueue.cpp
)

target_include_directories(qtl_core PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(qtl_core PUBLIC Threads::Threads)
