#pragma once
/**
 * @file strategy/mean_reversion/MeanReversionStrategy.hpp
 * @brief VWAP mean-reversion strategy.
 *
 * Algorithm
 * ─────────
 * Compute a rolling VWAP over the last N ticks.  When price deviates
 * from VWAP by more than `entryThreshold` standard deviations, enter
 * in the direction of reversion.  Exit when price returns to VWAP ± exitThreshold.
 *
 *   signal = (last_price - vwap) / rolling_std
 *   if signal < -entryThreshold and flat → BUY  (price below VWAP)
 *   if signal >  entryThreshold and flat → SELL (price above VWAP)
 *   if long  and last_price >= vwap - exitThreshold*std → EXIT
 *   if short and last_price <= vwap + exitThreshold*std → EXIT
 *
 * The rolling VWAP and std use an O(1) online algorithm (Welford).
 *
 * Parameters
 * ──────────
 *   windowSize       Number of ticks for VWAP/std calculation (e.g. 20)
 *   entryThreshold   Std devs from VWAP to enter (e.g. 2.0)
 *   exitThreshold    Std devs from VWAP to exit  (e.g. 0.5)
 *   orderQty         Shares per trade
 *   minStd           Minimum std to enter (avoid noise in flat markets)
 */

#include "strategy/Strategy.hpp"
#include <deque>
#include <cmath>
#include <numeric>

namespace qtl {

struct MeanReversionParams {
    int      windowSize{20};
    double   entryThreshold{2.0};
    double   exitThreshold{0.5};
    Quantity orderQty{100};
    double   minStd{0.005};
};

class MeanReversionStrategy final : public IStrategy {
public:
    using Params = MeanReversionParams;

    explicit MeanReversionStrategy(std::string symbol, Params params = Params{})
        : IStrategy("MeanReversion", std::move(symbol))
        , params_{params}
    {}

    void onStart(StrategyContext& ctx) override {
        IStrategy::onStart(ctx);
        prices_.clear();
        volumes_.clear();
        activeOrderId_ = 0;
    }

    void onMarket(const MarketEvent& e, StrategyContext& ctx) override {
        if (!isRunning() || e.symbol != symbol_) return;
        recordTick();

        Price last = e.lastPrice > 0 ? e.lastPrice
                     : (e.bidPrice + e.askPrice) / 2.0;
        if (last <= 0) return;

        // Update rolling window
        prices_.push_back(last);
        volumes_.push_back(static_cast<double>(
            e.lastSize > 0 ? e.lastSize : 1));
        if (static_cast<int>(prices_.size()) > params_.windowSize) {
            prices_.pop_front();
            volumes_.pop_front();
        }
        if (static_cast<int>(prices_.size()) < params_.windowSize / 2) return;

        // Compute VWAP
        double sumPV = 0.0, sumV = 0.0;
        for (size_t i = 0; i < prices_.size(); ++i) {
            sumPV += prices_[i] * volumes_[i];
            sumV  += volumes_[i];
        }
        double vwap = sumV > 0 ? sumPV / sumV : last;

        // Compute rolling std
        double mean = vwap;
        double sq   = 0.0;
        for (double p : prices_) { double d = p - mean; sq += d * d; }
        double std  = prices_.size() > 1
                          ? std::sqrt(sq / (prices_.size() - 1))
                          : 0.0;

        if (std < params_.minStd) return;  // market too flat

        double signal = (last - vwap) / std;
        Quantity pos  = ctx.position(symbol_);

        // Entry signals
        if (pos == 0 && activeOrderId_ == 0) {
            if (signal < -params_.entryThreshold) {
                // Price below VWAP → BUY expecting reversion up
                auto order = ctx.makeMarketBuy(symbol_, params_.orderQty, name_);
                activeOrderId_ = order.id;
                ctx.submitOrder(std::move(order));
                recordOrderSubmitted();
                entryPrice_ = last;
                entrySignal_ = signal;
            } else if (signal > params_.entryThreshold) {
                // Price above VWAP → SELL expecting reversion down
                auto order = ctx.makeMarketSell(symbol_, params_.orderQty, name_);
                activeOrderId_ = order.id;
                ctx.submitOrder(std::move(order));
                recordOrderSubmitted();
                entryPrice_ = last;
                entrySignal_ = signal;
            }
        }

        // Exit signals
        if (pos > 0 && signal >= -params_.exitThreshold) {
            // Long: exit when price returns toward VWAP
            auto order = ctx.makeMarketSell(symbol_, std::abs(pos), name_);
            ctx.submitOrder(std::move(order));
            recordOrderSubmitted();
            activeOrderId_ = 0;
        } else if (pos < 0 && signal <= params_.exitThreshold) {
            // Short: exit when price returns toward VWAP
            auto order = ctx.makeMarketBuy(symbol_, std::abs(pos), name_);
            ctx.submitOrder(std::move(order));
            recordOrderSubmitted();
            activeOrderId_ = 0;
        }

        lastVwap_   = vwap;
        lastSignal_ = signal;
        lastStd_    = std;
    }

    void onFill(const FillEvent& e, StrategyContext& ctx) override {
        if (e.symbol != symbol_) return;
        recordFill(e.side, e.fillPrice, e.fillQuantity, e.commission);
        if (e.orderId == activeOrderId_) activeOrderId_ = 0;
    }

    // Accessors for testing
    [[nodiscard]] double lastVwap()   const noexcept { return lastVwap_;   }
    [[nodiscard]] double lastSignal() const noexcept { return lastSignal_; }
    [[nodiscard]] double lastStd()    const noexcept { return lastStd_;    }
    [[nodiscard]] const Params& params() const noexcept { return params_;  }

private:
    Params  params_;
    std::deque<double> prices_;
    std::deque<double> volumes_;
    double  lastVwap_{0.0};
    double  lastSignal_{0.0};
    double  lastStd_{0.0};
    double  entryPrice_{0.0};
    double  entrySignal_{0.0};
    OrderId activeOrderId_{0};
};

} // namespace qtl
