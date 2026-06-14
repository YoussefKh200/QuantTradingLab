#pragma once
/**
 * @file strategy/momentum/MomentumStrategy.hpp
 * @brief Dual-EMA momentum / trend-following strategy.
 *
 * Algorithm
 * ─────────
 * Maintain a fast EMA and a slow EMA over last price.
 * Generate signals on crossovers:
 *
 *   fast_ema crosses above slow_ema → BUY  (bullish momentum)
 *   fast_ema crosses below slow_ema → SELL / EXIT long (bearish)
 *
 * Trend filter: only trade in the direction of a long-period trend EMA.
 *
 *   EMA update: ema = alpha * price + (1 - alpha) * ema
 *   alpha = 2 / (period + 1)
 *
 * Risk management:
 *   - ATR-based stop loss: exit if price moves against by atrMultiple * ATR
 *   - Maximum holding period: exit after maxHoldBars bars
 *
 * Parameters
 * ──────────
 *   fastPeriod      Fast EMA period (e.g. 8)
 *   slowPeriod      Slow EMA period (e.g. 21)
 *   trendPeriod     Trend filter EMA period (e.g. 50)
 *   atrPeriod       ATR lookback (e.g. 14)
 *   atrMultiple     Stop loss multiplier (e.g. 2.0)
 *   orderQty        Shares per trade
 *   maxHoldBars     Max ticks before forced exit
 */

#include "strategy/Strategy.hpp"
#include <deque>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace qtl {

struct MomentumParams {
    int      fastPeriod{8};
    int      slowPeriod{21};
    int      trendPeriod{50};
    int      atrPeriod{14};
    double   atrMultiple{2.0};
    Quantity orderQty{100};
    int      maxHoldBars{100};
    bool     useTrendFilter{true};
};

class MomentumStrategy final : public IStrategy {
public:
    using Params = MomentumParams;

    explicit MomentumStrategy(std::string symbol, Params params = Params{})
        : IStrategy("Momentum", std::move(symbol))
        , params_{params}
        , fastAlpha_{2.0 / (params.fastPeriod  + 1)}
        , slowAlpha_{2.0 / (params.slowPeriod  + 1)}
        , trendAlpha_{2.0 / (params.trendPeriod + 1)}
    {}

    void onStart(StrategyContext& ctx) override {
        IStrategy::onStart(ctx);
        fastEma_  = slowEma_  = trendEma_ = 0.0;
        prevFastEma_ = prevSlowEma_ = 0.0;
        prevPrice_ = 0.0;
        atrValues_.clear();
        barsHeld_  = 0;
        activeOrderId_ = 0;
        initialised_   = false;
    }

    void onMarket(const MarketEvent& e, StrategyContext& ctx) override {
        if (!isRunning() || e.symbol != symbol_) return;
        recordTick();

        Price price = e.lastPrice > 0 ? e.lastPrice
                      : (e.bidPrice + e.askPrice) / 2.0;
        if (price <= 0) return;

        // Update ATR (simplified: use |price - prevPrice| as proxy for range)
        if (prevPrice_ > 0) {
            double range = std::abs(price - prevPrice_);
            atrValues_.push_back(range);
            if (static_cast<int>(atrValues_.size()) > params_.atrPeriod)
                atrValues_.pop_front();
        }
        double atr = atrValues_.empty() ? 0.0
                     : std::accumulate(atrValues_.begin(), atrValues_.end(), 0.0)
                       / atrValues_.size();

        // Initialise EMAs on first tick
        if (!initialised_) {
            fastEma_  = slowEma_  = trendEma_ = price;
            initialised_ = true;
            prevPrice_   = price;
            return;
        }

        // Save previous for crossover detection
        prevFastEma_ = fastEma_;
        prevSlowEma_ = slowEma_;

        // Update EMAs
        fastEma_   = fastAlpha_  * price + (1.0 - fastAlpha_)  * fastEma_;
        slowEma_   = slowAlpha_  * price + (1.0 - slowAlpha_)  * slowEma_;
        trendEma_  = trendAlpha_ * price + (1.0 - trendAlpha_) * trendEma_;
        prevPrice_ = price;

        Quantity pos = ctx.position(symbol_);

        // ── Stop loss check ───────────────────────────────────
        if (pos != 0 && atr > 0) {
            double stopDist = params_.atrMultiple * atr;
            bool stopHit = (pos > 0 && price < entryPrice_ - stopDist) ||
                           (pos < 0 && price > entryPrice_ + stopDist);
            if (stopHit) {
                exitPosition(ctx, pos, "StopLoss");
                return;
            }
        }

        // ── Max hold period ───────────────────────────────────
        if (pos != 0) {
            ++barsHeld_;
            if (barsHeld_ >= params_.maxHoldBars) {
                exitPosition(ctx, pos, "MaxHold");
                return;
            }
        }

        // ── EMA crossover signals ────────────────────────────
        bool bullCross = (prevFastEma_ <= prevSlowEma_) &&
                         (fastEma_     >  slowEma_);
        bool bearCross = (prevFastEma_ >= prevSlowEma_) &&
                         (fastEma_     <  slowEma_);

        // Trend filter: only trade in trend direction
        bool trendUp   = !params_.useTrendFilter || price > trendEma_;
        bool trendDown = !params_.useTrendFilter || price < trendEma_;

        if (bullCross && trendUp && pos == 0) {
            auto order = ctx.makeMarketBuy(symbol_, params_.orderQty, name_);
            activeOrderId_ = order.id;
            ctx.submitOrder(std::move(order));
            recordOrderSubmitted();
            entryPrice_ = price;
            barsHeld_   = 0;
        } else if (bearCross && trendDown && pos == 0) {
            auto order = ctx.makeMarketSell(symbol_, params_.orderQty, name_);
            activeOrderId_ = order.id;
            ctx.submitOrder(std::move(order));
            recordOrderSubmitted();
            entryPrice_ = price;
            barsHeld_   = 0;
        } else if (bearCross && pos > 0) {
            exitPosition(ctx, pos, "CrossExit");
        } else if (bullCross && pos < 0) {
            exitPosition(ctx, pos, "CrossExit");
        }
    }

    void onFill(const FillEvent& e, StrategyContext& ctx) override {
        if (e.symbol != symbol_) return;
        recordFill(e.side, e.fillPrice, e.fillQuantity, e.commission);
    }

    [[nodiscard]] double fastEma()  const noexcept { return fastEma_;  }
    [[nodiscard]] double slowEma()  const noexcept { return slowEma_;  }
    [[nodiscard]] double trendEma() const noexcept { return trendEma_; }
    [[nodiscard]] const Params& params() const noexcept { return params_; }

private:
    void exitPosition(StrategyContext& ctx, Quantity pos,
                       const std::string& reason) {
        if (pos > 0) {
            ctx.submitOrder(ctx.makeMarketSell(symbol_, pos, name_));
        } else if (pos < 0) {
            ctx.submitOrder(ctx.makeMarketBuy(symbol_, -pos, name_));
        }
        recordOrderSubmitted();
        barsHeld_      = 0;
        activeOrderId_ = 0;
    }

    Params  params_;
    double  fastAlpha_, slowAlpha_, trendAlpha_;
    double  fastEma_{0.0}, slowEma_{0.0}, trendEma_{0.0};
    double  prevFastEma_{0.0}, prevSlowEma_{0.0};
    double  prevPrice_{0.0};
    double  entryPrice_{0.0};
    std::deque<double> atrValues_;
    int     barsHeld_{0};
    bool    initialised_{false};
    OrderId activeOrderId_{0};
};

} // namespace qtl
