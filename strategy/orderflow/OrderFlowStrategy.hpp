#pragma once
/**
 * @file strategy/orderflow/OrderFlowStrategy.hpp
 * @brief Order-flow imbalance (OFI) strategy.
 *
 * Algorithm
 * ─────────
 * Order-flow imbalance measures the net buying vs selling pressure
 * from the limit order book and trade tape:
 *
 *   OFI = sum over last N ticks of:
 *           +qty if aggressor = Buy  (buyer lifts offer)
 *           -qty if aggressor = Sell (seller hits bid)
 *
 * Normalised: ofi_signal = OFI / total_volume
 *
 * Entry rule:
 *   if ofi_signal > entryThreshold  → BUY  (buy-side pressure dominant)
 *   if ofi_signal < -entryThreshold → SELL (sell-side pressure dominant)
 *
 * Exit rule:
 *   Exit when OFI signal decays below exitThreshold magnitude.
 *   Alternatively: time-based exit after maxHoldTicks.
 *
 * Quote imbalance (secondary signal):
 *   bid_size / (bid_size + ask_size) — high value = buy pressure
 *
 * Parameters
 * ──────────
 *   ofiWindow       Rolling window for OFI accumulation (ticks)
 *   entryThreshold  Normalised OFI to enter (e.g. 0.6 = 60% imbalance)
 *   exitThreshold   OFI signal to exit (e.g. 0.1)
 *   orderQty        Shares per trade
 *   maxHoldTicks    Maximum holding period in ticks
 *   useQuoteImbal   Also weight quote-side imbalance
 */

#include "strategy/Strategy.hpp"
#include <deque>
#include <cmath>
#include <numeric>

namespace qtl {

struct OrderFlowParams {
    int      ofiWindow{30};
    double   entryThreshold{0.60};
    double   exitThreshold{0.10};
    Quantity orderQty{100};
    int      maxHoldTicks{50};
    bool     useQuoteImbalance{true};
    double   quoteImbalWeight{0.3};
};

class OrderFlowStrategy final : public IStrategy {
public:
    using Params = OrderFlowParams;

    explicit OrderFlowStrategy(std::string symbol, Params params = Params{})
        : IStrategy("OrderFlow", std::move(symbol))
        , params_{params}
    {}

    void onStart(StrategyContext& ctx) override {
        IStrategy::onStart(ctx);
        ofiBuffer_.clear();
        volBuffer_.clear();
        ticksHeld_     = 0;
        activeOrderId_ = 0;
        lastOfi_       = 0.0;
    }

    void onMarket(const MarketEvent& e, StrategyContext& ctx) override {
        if (!isRunning() || e.symbol != symbol_) return;
        recordTick();

        // ── OFI update ─────────────────────────────────────────
        double tickFlow = 0.0;
        double tickVol  = 0.0;

        if (e.lastSize > 0) {
            // Classify aggressor: if last_price == ask → buyer aggressor
            // Simplified: use bid/ask comparison to last price
            bool buyerAggressor = (e.lastPrice > 0 && e.askPrice > 0)
                ? (e.lastPrice >= e.askPrice)
                : true;
            tickFlow = buyerAggressor
                ? +static_cast<double>(e.lastSize)
                : -static_cast<double>(e.lastSize);
            tickVol  = static_cast<double>(e.lastSize);
        }

        ofiBuffer_.push_back(tickFlow);
        volBuffer_.push_back(tickVol);
        if (static_cast<int>(ofiBuffer_.size()) > params_.ofiWindow) {
            ofiBuffer_.pop_front();
            volBuffer_.pop_front();
        }

        // Compute normalised OFI signal
        double totalFlow = std::accumulate(ofiBuffer_.begin(), ofiBuffer_.end(), 0.0);
        double totalVol  = std::accumulate(volBuffer_.begin(),  volBuffer_.end(),  0.0);
        double ofiSignal = totalVol > 0 ? totalFlow / totalVol : 0.0;

        // ── Quote imbalance (secondary signal) ────────────────
        double quoteImbal = 0.0;
        if (params_.useQuoteImbalance &&
            e.bidSize > 0 && e.askSize > 0) {
            double totalSz = static_cast<double>(e.bidSize + e.askSize);
            quoteImbal = (static_cast<double>(e.bidSize) / totalSz) * 2.0 - 1.0;
            // +1 = all bid, -1 = all ask
        }

        // Blend signals
        double w = params_.useQuoteImbalance ? params_.quoteImbalWeight : 0.0;
        double combined = (1.0 - w) * ofiSignal + w * quoteImbal;
        lastOfi_ = combined;

        Quantity pos = ctx.position(symbol_);

        // ── Exit check ─────────────────────────────────────────
        if (pos != 0) {
            ++ticksHeld_;
            bool signalDecayed = (pos > 0 && combined < params_.exitThreshold)  ||
                                  (pos < 0 && combined > -params_.exitThreshold);
            bool maxHold       = ticksHeld_ >= params_.maxHoldTicks;
            if (signalDecayed || maxHold) {
                if (pos > 0) ctx.submitOrder(ctx.makeMarketSell(symbol_, pos, name_));
                else          ctx.submitOrder(ctx.makeMarketBuy( symbol_, -pos, name_));
                recordOrderSubmitted();
                ticksHeld_ = 0;
                activeOrderId_ = 0;
            }
        }

        // ── Entry check ────────────────────────────────────────
        if (pos == 0 && activeOrderId_ == 0 &&
            static_cast<int>(ofiBuffer_.size()) >= params_.ofiWindow / 2) {
            if (combined > params_.entryThreshold) {
                auto order = ctx.makeMarketBuy(symbol_, params_.orderQty, name_);
                activeOrderId_ = order.id;
                ctx.submitOrder(std::move(order));
                recordOrderSubmitted();
                ticksHeld_ = 0;
            } else if (combined < -params_.entryThreshold) {
                auto order = ctx.makeMarketSell(symbol_, params_.orderQty, name_);
                activeOrderId_ = order.id;
                ctx.submitOrder(std::move(order));
                recordOrderSubmitted();
                ticksHeld_ = 0;
            }
        }
    }

    void onFill(const FillEvent& e, StrategyContext& ctx) override {
        if (e.symbol != symbol_) return;
        recordFill(e.side, e.fillPrice, e.fillQuantity, e.commission);
        if (e.orderId == activeOrderId_) activeOrderId_ = 0;
    }

    [[nodiscard]] double lastOfi() const noexcept { return lastOfi_; }
    [[nodiscard]] const Params& params() const noexcept { return params_; }

private:
    Params  params_;
    std::deque<double> ofiBuffer_;
    std::deque<double> volBuffer_;
    double  lastOfi_{0.0};
    int     ticksHeld_{0};
    OrderId activeOrderId_{0};
};

} // namespace qtl
