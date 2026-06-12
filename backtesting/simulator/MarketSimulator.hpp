#pragma once
/**
 * @file backtesting/simulator/MarketSimulator.hpp
 * @brief Market impact and slippage models for realistic backtest simulation.
 *
 * Models implemented
 * ──────────────────
 *  FixedSlippage    — constant slippage in ticks/price units
 *  VolumeSlippage   — slippage scales with order size / market volume
 *  PercentSlippage  — slippage as fixed % of price
 *  SquareRootImpact — market impact ~ sqrt(participation_rate)
 *                     (Almgren-Chriss style)
 *
 * The SimulatedFill applies the slippage model, checks fill probability
 * (market orders always fill; limit orders fill if price is crossed),
 * and returns a FillPrice + ActualQuantity.
 */

#include "core/Types.hpp"
#include "exchange/orderbook/Order.hpp"
#include <cmath>
#include <random>
#include <memory>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// ISlippageModel interface
// ─────────────────────────────────────────────────────────────

class ISlippageModel {
public:
    virtual ~ISlippageModel() = default;

    /**
     * @brief Compute execution price after slippage.
     * @param order       The order being filled
     * @param marketPrice Current best price on the opposite side
     * @param marketVol   Market volume at this bar (for volume-based models)
     * @return Adjusted fill price
     */
    [[nodiscard]] virtual Price apply(const Order& order,
                                       Price marketPrice,
                                       Quantity marketVol) const noexcept = 0;
};

// ─────────────────────────────────────────────────────────────
// Concrete slippage models
// ─────────────────────────────────────────────────────────────

/// Constant slippage in price units (e.g. $0.01 per share).
class FixedSlippage final : public ISlippageModel {
public:
    explicit FixedSlippage(double slippagePx = 0.01) noexcept
        : slip_{slippagePx} {}

    [[nodiscard]] Price apply(const Order& order,
                               Price marketPrice,
                               Quantity) const noexcept override {
        return order.isBuy() ? marketPrice + slip_
                             : marketPrice - slip_;
    }
private:
    double slip_;
};

/// Slippage as a fixed percentage of price.
class PercentSlippage final : public ISlippageModel {
public:
    explicit PercentSlippage(double pct = 0.0005) noexcept
        : pct_{pct} {}

    [[nodiscard]] Price apply(const Order& order,
                               Price marketPrice,
                               Quantity) const noexcept override {
        double slip = marketPrice * pct_;
        return order.isBuy() ? marketPrice + slip
                             : marketPrice - slip;
    }
private:
    double pct_;
};

/// Volume-based slippage: slip = base_slip * sqrt(order_qty / market_vol).
class VolumeSlippage final : public ISlippageModel {
public:
    explicit VolumeSlippage(double baseSlip = 0.01) noexcept
        : base_{baseSlip} {}

    [[nodiscard]] Price apply(const Order& order,
                               Price marketPrice,
                               Quantity marketVol) const noexcept override {
        double participation = (marketVol > 0)
            ? static_cast<double>(order.quantity) /
              static_cast<double>(marketVol)
            : 0.01;
        double slip = base_ * std::sqrt(participation);
        return order.isBuy() ? marketPrice + slip
                             : marketPrice - slip;
    }
private:
    double base_;
};

/// Square-root market impact (Almgren-Chriss simplified).
class SquareRootImpact final : public ISlippageModel {
public:
    /// @param eta  Market impact coefficient (typical: 0.1)
    /// @param sigma Daily volatility of the asset
    explicit SquareRootImpact(double eta = 0.1, double sigma = 0.02) noexcept
        : eta_{eta}, sigma_{sigma} {}

    [[nodiscard]] Price apply(const Order& order,
                               Price marketPrice,
                               Quantity marketVol) const noexcept override {
        double adv = (marketVol > 0) ? static_cast<double>(marketVol) : 1000.0;
        double x   = static_cast<double>(order.quantity) / adv;
        double impact = eta_ * sigma_ * marketPrice * std::sqrt(x);
        return order.isBuy() ? marketPrice + impact
                             : marketPrice - impact;
    }
private:
    double eta_;
    double sigma_;
};

// ─────────────────────────────────────────────────────────────
// SimulatedFill — result of MarketSimulator::fill()
// ─────────────────────────────────────────────────────────────

struct SimulatedFill {
    bool     filled{false};
    Price    fillPrice{0.0};
    Quantity fillQty{0};
    double   slippage{0.0};   ///< Cost of slippage in $ (fillPrice - idealPrice) * qty
    double   commission{0.0};
};

// ─────────────────────────────────────────────────────────────
// MarketSimulator
// ─────────────────────────────────────────────────────────────

class MarketSimulator {
public:
    explicit MarketSimulator(
        std::shared_ptr<ISlippageModel> slippage =
            std::make_shared<FixedSlippage>(0.01),
        double commissionRate = 0.0005)
        : slippage_{std::move(slippage)}
        , commRate_{commissionRate}
    {}

    /**
     * @brief Attempt to fill an order given current market conditions.
     *
     * Fill logic:
     *  - Market order: always filled at marketPrice + slippage
     *  - Limit buy:    filled if marketPrice <= order.price
     *  - Limit sell:   filled if marketPrice >= order.price
     *  - IOC/FOK:      same as market for simulation purposes
     *
     * @param order       The incoming order
     * @param bestBid     Current best bid price
     * @param bestAsk     Current best ask price
     * @param marketVol   Current bar volume (for volume-slippage)
     */
    [[nodiscard]] SimulatedFill fill(const Order& order,
                                      Price bestBid, Price bestAsk,
                                      Quantity marketVol = 1000) const noexcept {
        SimulatedFill result;
        Price idealPrice  = order.isBuy() ? bestAsk : bestBid;
        if (idealPrice <= 0.0) return result;  // no quote

        // Limit order fill check
        if (order.isLimit()) {
            bool canFill = order.isBuy()
                ? (idealPrice <= order.price)
                : (idealPrice >= order.price);
            if (!canFill) return result;
        }

        Price fillPrice = slippage_->apply(order, idealPrice, marketVol);
        double slip     = std::abs(fillPrice - idealPrice) *
                          static_cast<double>(order.quantity);
        double comm     = fillPrice * static_cast<double>(order.quantity) * commRate_;

        result.filled    = true;
        result.fillPrice = fillPrice;
        result.fillQty   = order.quantity;
        result.slippage  = slip;
        result.commission= comm;
        return result;
    }

    void setSlippageModel(std::shared_ptr<ISlippageModel> m) {
        slippage_ = std::move(m);
    }
    void setCommissionRate(double r) noexcept { commRate_ = r; }

private:
    std::shared_ptr<ISlippageModel> slippage_;
    double                          commRate_;
};

} // namespace qtl
