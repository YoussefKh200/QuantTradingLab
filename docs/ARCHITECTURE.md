# QuantTradingLab — Architecture Overview

## Module Dependency DAG

```
                        ┌──────────────────────────────┐
                        │          main.cpp            │
                        └──────────────┬───────────────┘
                                       │
              ┌────────────────────────▼──────────────────────────┐
              │               qtl_visualization (Phase 13)        │
              └────────────────────────┬──────────────────────────┘
                                       │
              ┌────────────────────────▼──────────────────────────┐
              │               qtl_options (Phase 9-10)            │
              │  blackscholes · greeks · iv_surface               │
              │  gamma_exposure · dealer_positioning               │
              └────────────────────────┬──────────────────────────┘
                                       │
              ┌────────────────────────▼──────────────────────────┐
              │               qtl_analytics (Phase 14)            │
              │  statistics · montecarlo · optimization · metrics  │
              └────────────────────────┬──────────────────────────┘
                                       │
              ┌────────────────────────▼──────────────────────────┐
              │               qtl_backtesting (Phase 6)           │
              │  engine · simulator · replay                       │
              └────────────────────────┬──────────────────────────┘
                                       │
         ┌─────────────────────────────▼──────────────────────┐
         │                    qtl_portfolio (Phase 11)         │
         │         pnl · positions · accounting                │
         └─────────────────────────────┬──────────────────────┘
                                       │
         ┌─────────────────────────────▼──────────────────────┐
         │                      qtl_risk (Phase 7)             │
         │          limits · exposure · kill_switch            │
         └─────────────────────────────┬──────────────────────┘
                                       │
         ┌─────────────────────────────▼──────────────────────┐
         │                   qtl_strategy (Phase 8)            │
         │  market_making · mean_reversion · momentum          │
         │  orderflow                                          │
         └─────────────────────────────┬──────────────────────┘
                                       │
         ┌─────────────────────────────▼──────────────────────┐
         │                   qtl_exchange (Phase 3-5)          │
         │  orderbook · matching · marketdata · execution      │
         └─────────────────────────────┬──────────────────────┘
                                       │
         ┌─────────────────────────────▼──────────────────────┐
         │                     qtl_core (Phase 1-2)            │
         │  events · clock · logger · threading · config       │
         └─────────────────────────────────────────────────────┘
```

## Module Responsibilities

| Module | Phase | Responsibility |
|--------|-------|---------------|
| `core/events` | 1–2 | Event hierarchy, EventQueue, EventDispatcher |
| `core/clock` | 1 | WallClock + SimClock abstraction |
| `core/logger` | 1 | Async structured logger with pluggable sinks |
| `core/threading` | 1, 12 | SPSC RingBuffer, ThreadPool |
| `core/config` | 1 | Key-value config store (INI loader) |
| `exchange/orderbook` | 3 | Price-level book, FIFO queue, depth display |
| `exchange/matching` | 4 | Price-time priority engine, fill reports |
| `exchange/marketdata` | 5 | TradeTick/QuoteTick, CSV replay engine |
| `exchange/execution` | 4 | Execution report builder |
| `strategy/*` | 8 | Abstract Strategy + 6 concrete implementations |
| `risk/*` | 7 | Position/loss limits, exposure tracking, kill switch |
| `portfolio/*` | 11 | Realised/unrealised P&L, multi-symbol positions |
| `backtesting/*` | 6 | Event-driven simulation, HTML report generation |
| `analytics/*` | 14 | Statistics, Monte Carlo, walk-forward, metrics |
| `options/*` | 9–10 | Black-Scholes, Greeks, IV surface, GEX/DEX/VEX |
| `visualization/*` | 13 | Dear ImGui trading dashboard |

## Event Flow (Simulation Loop)

```
  TickReplayer
      │  MarketEvent
      ▼
  EventQueue  ◄──────────────────────────────────────────┐
      │                                                   │
      ▼                                                   │
  EventDispatcher                                         │
      ├── MarketEvent ──► Strategy::onTick()              │
      │                      └── OrderEvent ─────────────►│
      │                                                   │
      ├── OrderEvent ───► MatchingEngine                  │
      │                      └── FillEvent ──────────────►│
      │                                                   │
      ├── FillEvent ────► Strategy::onFill()              │
      │                   PositionManager::onFill()       │
      │                   PnLTracker::onFill()            │
      │                                                   │
      ├── RiskEvent ────► KillSwitch                      │
      │                                                   │
      └── SignalEvent ──► Logger / Analytics              │
                                                          │
  RiskEngine ──── checks limits ────────────────────────►(RiskEvent)
```

## Build Commands

```bash
# Configure (Release)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build all targets
cmake --build build -j$(nproc)

# Run main binary
./build/bin/QuantTradingLab

# Run tests
cd build && ctest --output-on-failure

# Debug build (with ASan/UBSan)
cmake -B build_debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build_debug -j$(nproc)
```

## Coding Conventions

- All types in namespace `qtl`
- Headers: `.hpp`, sources: `.cpp`
- Smart pointers only — no raw `new`/`delete`
- RAII for all resources
- `[[nodiscard]]` on all query functions
- `noexcept` wherever exceptions cannot propagate
- `const` correctness enforced throughout
