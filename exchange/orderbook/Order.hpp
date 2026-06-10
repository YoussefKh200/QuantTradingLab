#pragma once
/**
 * @file exchange/orderbook/Order.hpp
 * @brief Order data structures (stub for Phase 1 — fully implemented Phase 3).
 */
#include "core/Types.hpp"
#include <string>

namespace qtl {

struct Order {
    OrderId     id{0};
    Symbol      symbol;
    Side        side{Side::Buy};
    OrderType   type{OrderType::Limit};
    Price       price{0.0};
    Quantity    quantity{0};
    Quantity    filledQty{0};
    OrderStatus status{OrderStatus::New};
    Timestamp   timestamp{0};
    std::string strategyId;

    [[nodiscard]] Quantity remainingQty() const noexcept {
        return quantity - filledQty;
    }
    [[nodiscard]] bool isActive() const noexcept {
        return status == OrderStatus::New ||
               status == OrderStatus::PartiallyFilled;
    }
};

} // namespace qtl
