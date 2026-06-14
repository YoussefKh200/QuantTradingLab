#pragma once
/**
 * @file strategy/market_making/MarketMakingStrategy.hpp
 * @brief Symmetric market-making strategy with inventory skew.
 *
 * Algorithm
 * ─────────
 * Post a two-sided quote (bid + ask) symmetrically around the mid-price,
 * skewed by current inventory to reduce directional risk:
 *
 *   quote_mid   = last_mid + inventory_skew * netQty / maxInventory
 *   bid_price   = quote_mid - halfSpread
 *   ask_price   = quote_mid + halfSpread
 *
 * On each tick:
 *   1. Cancel stale quotes (if mid has moved by > requote_threshold).
 *   2. Post fresh bid + ask if flat (no open orders on that side).
 *   3. On fill: immediately re-post on the filled side.
 *
 * Inventory management:
 *   If |netQty| > maxInventory * 0.8:
 *     Widen spread on the heavy side to discourage more inventory.
 *     Narrow spread on the light side to encourage reversion.
 *
 * Parameters
 * ──────────
 *   halfSpread          Half the target bid-ask spread (e.g. 0.01)
 *   orderQty            Quantity per side (e.g. 100)
 *   maxInventory        Position cap (kill passive posting above this)
 *   requoteThreshold    Mid-price move that triggers re-quote (e.g. 0.02)
 *   inventorySkewFactor Controls how much inventory skews the quote (e.g. 0.5)
 */

#include "strategy/Strategy.hpp"
#include <cmath>
#include <unordered_map>

namespace qtl {

struct MarketMakingParams {
    double   halfSpread{0.02};
    Quantity orderQty{100};
    Quantity maxInventory{1000};
    double   requoteThreshold{0.03};
    double   inventorySkewFactor{0.5};
    bool     cancelOnStop{true};
};

class MarketMakingStrategy final : public IStrategy {
public:
    using Params = MarketMakingParams;

    explicit MarketMakingStrategy(std::string symbol, Params params = Params{})
        : IStrategy("MarketMaking", std::move(symbol))
        , params_{params}
    {}

    void onStart(StrategyContext& ctx) override {
        IStrategy::onStart(ctx);
        lastMid_ = 0.0;
        bidOrderId_ = askOrderId_ = 0;
    }

    void onStop(StrategyContext& ctx) override {
        if (params_.cancelOnStop) cancelAllQuotes(ctx);
        IStrategy::onStop(ctx);
    }

    void onMarket(const MarketEvent& e, StrategyContext& ctx) override {
        if (!isRunning() || e.symbol != symbol_) return;
        recordTick();

        Price bid = e.bidPrice, ask = e.askPrice;
        if (bid <= 0 || ask <= 0 || ask <= bid) return;

        Price mid = (bid + ask) / 2.0;

        // Requote if mid has moved significantly
        bool shouldRequote = (lastMid_ == 0.0) ||
                             (std::abs(mid - lastMid_) >= params_.requoteThreshold);
        if (shouldRequote) {
            cancelAllQuotes(ctx);
            postQuotes(mid, ctx);
            lastMid_ = mid;
        }
    }

    void onFill(const FillEvent& e, StrategyContext& ctx) override {
        if (e.symbol != symbol_) return;
        recordFill(e.side, e.fillPrice, e.fillQuantity, e.commission);

        // Clear the filled order ID
        if (e.orderId == bidOrderId_) bidOrderId_ = 0;
        if (e.orderId == askOrderId_) askOrderId_ = 0;

        // Immediately re-post on the filled side
        if (lastMid_ > 0) {
            Quantity pos = ctx.position(symbol_);
            postQuotes(lastMid_, ctx, pos);
        }
    }

    // Accessors for testing
    [[nodiscard]] OrderId bidOrderId() const noexcept { return bidOrderId_; }
    [[nodiscard]] OrderId askOrderId() const noexcept { return askOrderId_; }
    [[nodiscard]] Price   lastMid()    const noexcept { return lastMid_;    }
    [[nodiscard]] const Params& params() const noexcept { return params_;  }

private:
    void cancelAllQuotes(StrategyContext& ctx) {
        if (bidOrderId_ != 0) {
            ctx.cancelOrder(bidOrderId_, symbol_);
            ++stats_.ordersCancelled;
            bidOrderId_ = 0;
        }
        if (askOrderId_ != 0) {
            ctx.cancelOrder(askOrderId_, symbol_);
            ++stats_.ordersCancelled;
            askOrderId_ = 0;
        }
    }

    void postQuotes(Price mid, StrategyContext& ctx,
                    Quantity inventoryHint = 0) {
        Quantity pos = inventoryHint != 0
                           ? inventoryHint
                           : ctx.position(symbol_);

        // Inventory skew: shift mid quote against inventory
        double skew = 0.0;
        if (params_.maxInventory > 0) {
            skew = params_.inventorySkewFactor *
                   params_.halfSpread *
                   static_cast<double>(pos) /
                   static_cast<double>(params_.maxInventory);
        }

        Price quoteMid = mid - skew;  // negative when long (discourage more buys)

        // Widen spread on heavy side
        double bidSpread = params_.halfSpread;
        double askSpread = params_.halfSpread;
        double heavyFrac = params_.maxInventory > 0
            ? std::abs(static_cast<double>(pos)) /
              static_cast<double>(params_.maxInventory)
            : 0.0;
        if (pos > 0) bidSpread *= (1.0 + heavyFrac); // long → wider bid
        if (pos < 0) askSpread *= (1.0 + heavyFrac); // short → wider ask

        Price bidPx = quoteMid - bidSpread;
        Price askPx = quoteMid + askSpread;

        // Only post if inventory allows
        if (pos < params_.maxInventory && bidOrderId_ == 0) {
            auto order = ctx.makeLimitBuy(symbol_, bidPx,
                                           params_.orderQty, name_);
            bidOrderId_ = order.id;
            ctx.submitOrder(std::move(order));
            recordOrderSubmitted();
        }

        if (pos > -params_.maxInventory && askOrderId_ == 0) {
            auto order = ctx.makeLimitSell(symbol_, askPx,
                                            params_.orderQty, name_);
            askOrderId_ = order.id;
            ctx.submitOrder(std::move(order));
            recordOrderSubmitted();
        }
    }

    Params  params_;
    Price   lastMid_{0.0};
    OrderId bidOrderId_{0};
    OrderId askOrderId_{0};
};

} // namespace qtl
