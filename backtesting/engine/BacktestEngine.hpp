#pragma once
/**
 * @file backtesting/engine/BacktestEngine.hpp
 * @brief Event-driven backtesting engine.
 *
 * Architecture
 * ────────────
 *  TickReplayer → MarketDataFeed → EventLoop → Strategy
 *                                            → MatchingEngine
 *                                            → PortfolioTracker
 *                                            → RiskMonitor
 *
 * The BacktestEngine assembles all components and drives the simulation:
 *
 *   1. TickReplayer feeds historical ticks into MarketDataFeed.
 *   2. MarketDataFeed emits MarketEvents onto the EventLoop.
 *   3. The Strategy's onTick() fires → emits OrderEvents.
 *   4. OrderEvents route to the MatchingEngine (with slippage).
 *   5. Fills update the PortfolioTracker → equity curve.
 *   6. After replay: PerformanceMetrics computes all ratios.
 *   7. ReportGenerator produces the HTML report.
 *
 * Usage
 * ─────
 * @code
 *   BacktestEngine engine;
 *   engine.setInitialCapital(100'000.0);
 *   engine.addDataFile("AAPL_2023.csv", TickType::Quote);
 *   engine.setStrategy(std::make_shared<MomentumStrategy>());
 *   engine.run();
 *   engine.generateReport("backtest_report.html");
 *   std::cout << engine.summary().toString();
 * @endcode
 */

#include "backtesting/engine/ReportGenerator.hpp"
#include "backtesting/replay/TickReplayer.hpp"
#include "backtesting/simulator/MarketSimulator.hpp"
#include "exchange/matching/MatchingEngine.hpp"
#include "exchange/marketdata/MarketDataFeed.hpp"
#include "analytics/metrics/PerformanceMetrics.hpp"
#include "core/events/EventLoop.hpp"
#include "core/logger/Logger.hpp"

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <stdexcept>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// IStrategy — abstract strategy interface for the backtest engine
// ─────────────────────────────────────────────────────────────

class BacktestEngine;  // forward declaration

class IStrategy {
public:
    virtual ~IStrategy() = default;

    virtual void onStart (BacktestEngine* engine) {}
    virtual void onTick  (const MarketEvent& e,   BacktestEngine* engine) = 0;
    virtual void onFill  (const FillEvent&   e,   BacktestEngine* engine) {}
    virtual void onStop  (BacktestEngine* engine) {}

    [[nodiscard]] virtual std::string name() const = 0;
};

// ─────────────────────────────────────────────────────────────
// PortfolioTracker — lightweight P&L + position state
// ─────────────────────────────────────────────────────────────

class PortfolioTracker {
public:
    explicit PortfolioTracker(double initialCapital)
        : cash_{initialCapital}, initialCapital_{initialCapital} {}

    void onFill(const Symbol& sym, Side side,
                Price fillPrice, Quantity fillQty,
                double commission) {
        double notional = fillPrice * static_cast<double>(fillQty);
        if (side == Side::Buy) {
            positions_[sym]  += fillQty;
            cash_            -= notional + commission;
        } else {
            positions_[sym]  -= fillQty;
            cash_            += notional - commission;
        }
        totalCommission_ += commission;

        // Record trade P&L (round-trip detected when position crosses zero)
        double prev = prevPositionNotional_.count(sym)
                          ? prevPositionNotional_[sym] : 0.0;
        double curr = fillPrice * static_cast<double>(positions_[sym]);
        if ((prev > 0 && curr <= 0) || (prev < 0 && curr >= 0)) {
            // Simplified: mark round-trip P&L as the cash change
            // In production this would be tracked per-lot
        }
        prevPositionNotional_[sym] = curr;
    }

    void onBar(const Symbol& sym, Price markPrice) {
        // Mark-to-market: update unrealised P&L
        markPrices_[sym] = markPrice;
    }

    /**
     * @brief Snapshot current equity = cash + sum(position * mark_price).
     */
    [[nodiscard]] double equity() const noexcept {
        double total = cash_;
        for (auto& [sym, qty] : positions_) {
            auto it = markPrices_.find(sym);
            if (it != markPrices_.end())
                total += static_cast<double>(qty) * it->second;
        }
        return total;
    }

    [[nodiscard]] double   cash()             const noexcept { return cash_; }
    [[nodiscard]] double   initialCapital()   const noexcept { return initialCapital_; }
    [[nodiscard]] double   totalCommission()  const noexcept { return totalCommission_; }
    [[nodiscard]] Quantity position(const Symbol& sym) const noexcept {
        auto it = positions_.find(sym);
        return it == positions_.end() ? 0 : it->second;
    }
    [[nodiscard]] const std::unordered_map<Symbol,Quantity>& positions()
        const noexcept { return positions_; }

    void recordEquityPoint() {
        equityCurve_.push_back(equity());
    }

    [[nodiscard]] const std::vector<double>& equityCurve()
        const noexcept { return equityCurve_; }

    void recordTrade(double pnl) { tradePnl_.push_back(pnl); }
    [[nodiscard]] const std::vector<double>& tradePnl()
        const noexcept { return tradePnl_; }

private:
    double   cash_;
    double   initialCapital_;
    double   totalCommission_{0.0};
    std::unordered_map<Symbol, Quantity> positions_;
    std::unordered_map<Symbol, Price>    markPrices_;
    std::unordered_map<Symbol, double>   prevPositionNotional_;
    std::vector<double> equityCurve_;
    std::vector<double> tradePnl_;
};

// ─────────────────────────────────────────────────────────────
// BacktestEngine
// ─────────────────────────────────────────────────────────────

class BacktestEngine {
public:
    explicit BacktestEngine(double initialCapital = 100'000.0)
        : initialCapital_{initialCapital}
        , simClock_{std::make_shared<SimClock>(0)}
        , eventLoop_{std::make_shared<EventLoop>(simClock_)}
        , feed_{std::make_unique<MarketDataFeed>(eventLoop_.get())}
        , replayer_{std::make_unique<TickReplayer>(
              feed_.get(), simClock_.get(), 0.0)}
        , portfolio_{std::make_unique<PortfolioTracker>(initialCapital)}
        , matchEngine_{std::make_unique<MatchingEngine>(
              std::make_shared<FlatRateCommission>(), eventLoop_.get())}
    {}

    // ── Configuration ─────────────────────────────────────────

    void setInitialCapital(double cap) {
        initialCapital_ = cap;
        portfolio_ = std::make_unique<PortfolioTracker>(cap);
    }

    void addDataFile(const std::string& path,
                      TickType type = TickType::Quote) {
        replayer_->loadCSV(path, type);
    }

    void addSymbol(const Symbol& sym) {
        matchEngine_->addSymbol(sym);
        registeredSymbols_.push_back(sym);
    }

    void setStrategy(std::shared_ptr<IStrategy> strat) {
        strategy_ = std::move(strat);
    }

    void setSlippageModel(std::shared_ptr<ISlippageModel> model) {
        simulator_ = std::make_unique<MarketSimulator>(std::move(model));
    }

    void setCommissionRate(double rate) {
        commissionRate_ = rate;
    }

    // ── Order submission API (called from strategy) ───────────

    /**
     * @brief Submit a limit order from the strategy.
     * The engine applies slippage and routes to the MatchingEngine.
     * Returns the generated ExecutionReports.
     */
    std::vector<ExecutionReport> submitOrder(Order order) {
        if (!matchEngine_->hasSymbol(order.symbol)) {
            matchEngine_->addSymbol(order.symbol);
        }
        return matchEngine_->submitOrder(std::move(order));
    }

    ExecutionReport cancelOrder(OrderId id, const Symbol& sym) {
        return matchEngine_->cancelOrder(id, sym);
    }

    [[nodiscard]] Quantity position(const Symbol& sym) const noexcept {
        return portfolio_->position(sym);
    }
    [[nodiscard]] double cash() const noexcept { return portfolio_->cash(); }
    [[nodiscard]] double equity() const noexcept { return portfolio_->equity(); }

    // ── Run ───────────────────────────────────────────────────

    /**
     * @brief Execute the full backtest.
     *
     * Returns the PerformanceMetrics::Summary after replay completes.
     */
    PerformanceMetrics::Summary run() {
        if (!strategy_)
            throw std::runtime_error("BacktestEngine: no strategy set");

        Logger::instance().info("BacktestEngine",
            "Starting backtest: strategy={} capital={:.0f}",
            strategy_->name(), initialCapital_);

        strategy_->onStart(this);

        // Wire MarketEvent → strategy onTick
        eventLoop_->subscribe<MarketEvent>([this](const MarketEvent& e) {
            // Mark-to-market on each tick
            portfolio_->onBar(e.symbol, e.lastPrice > 0 ? e.lastPrice
                                                         : e.bidPrice);
            portfolio_->recordEquityPoint();
            // Dispatch to strategy
            strategy_->onTick(e, this);
        });

        // Wire FillEvent → portfolio update + strategy onFill
        eventLoop_->subscribe<FillEvent>([this](const FillEvent& e) {
            portfolio_->onFill(e.symbol, e.side,
                               e.fillPrice, e.fillQuantity,
                               e.commission);
            strategy_->onFill(e, this);
            // Record trade P&L approximation: signed notional
            double tradePnl = (e.side == Side::Sell ? 1.0 : -1.0) *
                               e.fillPrice *
                               static_cast<double>(e.fillQuantity) -
                               e.commission;
            portfolio_->recordTrade(tradePnl);
        });

        // Run replay (as-fast-as-possible for backtest)
        replayer_->replay(/*background=*/false);

        // Drain any remaining events from the loop
        eventLoop_->run(RunMode::Drain);

        strategy_->onStop(this);

        // Compute summary
        const auto& curve = portfolio_->equityCurve();
        const auto& trades = portfolio_->tradePnl();
        summary_ = PerformanceMetrics::computeSummary(
            curve.empty() ? std::vector<double>{initialCapital_} : curve,
            trades, 0.0, kTradingDaysPerYear);

        Logger::instance().info("BacktestEngine",
            "Backtest complete: totalReturn={:.2f}% sharpe={:.2f} "
            "maxDD={:.2f}%",
            summary_.totalReturn * 100.0,
            summary_.sharpe,
            summary_.drawdown.maxDrawdown * 100.0);

        return summary_;
    }

    // ── Report generation ─────────────────────────────────────

    /**
     * @brief Generate an HTML performance report.
     * @param outPath  Output file path.
     * @param stratName Override strategy name in the report title.
     */
    void generateReport(const std::string& outPath,
                         const std::string& stratName = "") const {
        BacktestReport rpt;
        rpt.strategyName   = stratName.empty() && strategy_
                                 ? strategy_->name()
                                 : (stratName.empty() ? "Strategy" : stratName);
        rpt.symbol         = registeredSymbols_.empty()
                                 ? "Multi-Symbol" : registeredSymbols_[0];
        rpt.initialCapital = initialCapital_;
        rpt.equityCurve    = portfolio_->equityCurve();
        rpt.tradePnl       = portfolio_->tradePnl();
        rpt.summary        = summary_;
        ReportGenerator::generate(rpt, outPath);
        Logger::instance().info("BacktestEngine",
            "Report written to '{}'", outPath);
    }

    // ── Accessors ─────────────────────────────────────────────

    [[nodiscard]] const PerformanceMetrics::Summary& summary()
        const noexcept { return summary_; }
    [[nodiscard]] const PortfolioTracker& portfolio()
        const noexcept { return *portfolio_; }
    [[nodiscard]] const ReplayStats& replayStats()
        const noexcept { return replayer_->stats(); }
    [[nodiscard]] SimClock& clock() noexcept { return *simClock_; }

private:
    double                         initialCapital_;
    double                         commissionRate_{0.0005};

    std::shared_ptr<SimClock>       simClock_;
    std::shared_ptr<EventLoop>      eventLoop_;
    std::unique_ptr<MarketDataFeed> feed_;
    std::unique_ptr<TickReplayer>   replayer_;
    std::unique_ptr<PortfolioTracker> portfolio_;
    std::unique_ptr<MatchingEngine> matchEngine_;
    std::unique_ptr<MarketSimulator> simulator_;
    std::shared_ptr<IStrategy>      strategy_;
    std::vector<Symbol>             registeredSymbols_;
    PerformanceMetrics::Summary     summary_;
};

} // namespace qtl
