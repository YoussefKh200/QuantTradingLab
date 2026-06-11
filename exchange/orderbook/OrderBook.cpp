/**
 * @file exchange/orderbook/OrderBook.cpp
 * @brief Limit order book — full price-time priority implementation.
 */

#include "exchange/orderbook/OrderBook.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cassert>

namespace qtl {

OrderBook::OrderBook(Symbol symbol)
    : symbol_{std::move(symbol)}
{}

// ─────────────────────────────────────────────────────────────
// addOrder
// ─────────────────────────────────────────────────────────────

AddResult OrderBook::addOrder(Order order) {
    AddResult result;
    if (order.quantity <= 0 || order.id == 0) return result;
    if (orders_.count(order.id))              return result;  // duplicate

    order.seqNo     = seqGen_++;
    order.createdAt = nowNs();
    order.symbol    = symbol_;
    ++ordersAdded_;
    result.accepted = true;

    // FOK pre-check: enough liquidity to fill completely?
    if (order.tif == TimeInForce::FOK) {
        Quantity avail = 0;
        if (order.isBuy()) {
            for (auto& [p, lvl] : asks_) {
                if (order.isLimit() && p > order.price) break;
                avail += lvl.totalQty();
                if (avail >= order.quantity) break;
            }
        } else {
            for (auto& [p, lvl] : bids_) {
                if (order.isLimit() && p < order.price) break;
                avail += lvl.totalQty();
                if (avail >= order.quantity) break;
            }
        }
        if (avail < order.quantity) {
            result.accepted = false;
            return result;
        }
    }

    // Aggressive matching against opposite side
    result.trades = matchAgainst(order);

    // Rest remainder for resting limit orders
    if (order.isActive()) {
        if (order.isLimit() &&
            order.tif != TimeInForce::IOC &&
            order.tif != TimeInForce::FOK) {
            auto [it, ok] = orders_.emplace(order.id, std::move(order));
            restOrder(it->second);
        } else {
            order.status = OrderStatus::Cancelled;
        }
    }

    result.immediatelyFilled = !result.trades.empty();
    return result;
}

// ─────────────────────────────────────────────────────────────
// cancelOrder
// ─────────────────────────────────────────────────────────────

bool OrderBook::cancelOrder(OrderId id) {
    auto oit = orders_.find(id);
    if (oit == orders_.end())      return false;
    if (!oit->second.isActive())   return false;

    oit->second.status = OrderStatus::Cancelled;
    removeFromLevel(id);
    orders_.erase(oit);
    return true;
}

// ─────────────────────────────────────────────────────────────
// modifyOrder
// ─────────────────────────────────────────────────────────────

bool OrderBook::modifyOrder(const ModifyRequest& req) {
    auto oit = orders_.find(req.orderId);
    if (oit == orders_.end())    return false;
    if (!oit->second.isActive()) return false;

    Order& existing  = oit->second;
    bool priceChange = (req.newPrice > 0 && req.newPrice != existing.price);
    bool qtyIncrease = (req.newQuantity > 0 &&
                        req.newQuantity > existing.remainingQty());
    bool qtyDecrease = (req.newQuantity > 0 &&
                        req.newQuantity < existing.quantity &&
                        !priceChange);

    if (priceChange || qtyIncrease) {
        // Lose queue priority — cancel and re-submit
        Order modified      = existing;
        if (req.newPrice    > 0) modified.price    = req.newPrice;
        if (req.newQuantity > 0) {
            modified.quantity  = req.newQuantity;
            modified.filledQty = 0;
        }
        modified.status = OrderStatus::New;
        removeFromLevel(req.orderId);
        orders_.erase(oit);
        return addOrder(std::move(modified)).accepted;
    }

    if (qtyDecrease) {
        // Preserve priority — shrink quantity in-place
        Quantity delta = existing.quantity - req.newQuantity;
        existing.quantity = req.newQuantity;
        if (existing.isBuy()) {
            auto lit = bids_.find(existing.price);
            if (lit != bids_.end()) lit->second.reduceQty(delta);
        } else {
            auto lit = asks_.find(existing.price);
            if (lit != asks_.end()) lit->second.reduceQty(delta);
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────
// Queries
// ─────────────────────────────────────────────────────────────

Price OrderBook::bestBid() const noexcept {
    return bids_.empty() ? 0.0 : bids_.begin()->first;
}
Price OrderBook::bestAsk() const noexcept {
    return asks_.empty() ? 0.0 : asks_.begin()->first;
}
Price OrderBook::midPrice() const noexcept {
    Price b = bestBid(), a = bestAsk();
    return (b > 0 && a > 0) ? (b + a) / 2.0 : 0.0;
}
Price OrderBook::spread() const noexcept {
    Price b = bestBid(), a = bestAsk();
    return (b > 0 && a > 0) ? a - b : 0.0;
}
const Order* OrderBook::findOrder(OrderId id) const noexcept {
    auto it = orders_.find(id);
    return it == orders_.end() ? nullptr : &it->second;
}
Quantity OrderBook::totalBidQty() const noexcept {
    Quantity t = 0;
    for (auto& [p, lvl] : bids_) t += lvl.totalQty();
    return t;
}
Quantity OrderBook::totalAskQty() const noexcept {
    Quantity t = 0;
    for (auto& [p, lvl] : asks_) t += lvl.totalQty();
    return t;
}

OrderBook::DepthSnapshot OrderBook::snapshot(size_t depth) const {
    DepthSnapshot snap;
    size_t n = 0;
    for (auto& [p, lvl] : bids_) {
        if (depth && n++ >= depth) break;
        snap.bids.emplace_back(p, lvl.totalQty());
    }
    n = 0;
    for (auto& [p, lvl] : asks_) {
        if (depth && n++ >= depth) break;
        snap.asks.emplace_back(p, lvl.totalQty());
    }
    return snap;
}

// ─────────────────────────────────────────────────────────────
// printBook
// ─────────────────────────────────────────────────────────────

std::string OrderBook::printBook(size_t depth) const {
    std::ostringstream oss;

    Quantity maxQty = 1;
    size_t n = 0;
    for (auto& [p, lvl] : asks_) {
        if (depth && n++ >= depth) break;
        if (lvl.totalQty() > maxQty) maxQty = lvl.totalQty();
    }
    n = 0;
    for (auto& [p, lvl] : bids_) {
        if (depth && n++ >= depth) break;
        if (lvl.totalQty() > maxQty) maxQty = lvl.totalQty();
    }

    constexpr int kBarMax = 24;
    auto bar = [&](Quantity qty) {
        int len = static_cast<int>(
            static_cast<double>(qty) / static_cast<double>(maxQty) * kBarMax);
        return std::string(static_cast<size_t>(std::max(1,len)), '|');
    };

    oss << "\n+----------------------------------------------------------+\n";
    oss << "|  " << std::left << std::setw(54) << (" BOOK: " + symbol_) << "|\n";
    oss << "+----------+----------+----------+------------------------+\n";
    oss << "| SIDE     |     QTY  |    PRICE | DEPTH                  |\n";
    oss << "+----------+----------+----------+------------------------+\n";

    // Collect asks, print worst→best
    std::vector<std::pair<Price,Quantity>> askRows;
    n = 0;
    for (auto& [p, lvl] : asks_) {
        if (depth && n++ >= depth) break;
        askRows.emplace_back(p, lvl.totalQty());
    }
    for (auto it = askRows.rbegin(); it != askRows.rend(); ++it) {
        oss << "| ASK      |" << std::setw(9) << it->second << " |"
            << std::fixed << std::setprecision(4) << std::setw(9) << it->first << " | "
            << std::left << std::setw(23) << bar(it->second) << "|\n";
    }

    // Mid
    oss << "+----------+----------+----------+------------------------+\n";
    std::ostringstream ms;
    ms << std::fixed << std::setprecision(4) << midPrice();
    oss << "|          |          | mid " << std::setw(7) << ms.str()
        << " |                        |\n";
    oss << "+----------+----------+----------+------------------------+\n";

    // Bids best→worst
    n = 0;
    for (auto& [p, lvl] : bids_) {
        if (depth && n++ >= depth) break;
        oss << "| BID      |" << std::setw(9) << lvl.totalQty() << " |"
            << std::fixed << std::setprecision(4) << std::setw(9) << p << " | "
            << std::left << std::setw(23) << bar(lvl.totalQty()) << "|\n";
    }

    oss << "+----------+----------+----------+------------------------+\n";
    oss << "| Orders: " << std::setw(5) << (bidOrderCount_ + askOrderCount_)
        << "  Trades: " << std::setw(5) << tradeCount_
        << "  Spread: " << std::fixed << std::setprecision(4) << spread()
        << std::string(6,' ') << "|\n";
    oss << "+----------------------------------------------------------+\n";
    return oss.str();
}

// ─────────────────────────────────────────────────────────────
// matchAgainst — walk opposite side FIFO
// ─────────────────────────────────────────────────────────────

static void matchOneSide(
        Order& aggressor,
        std::map<Price, PriceLevel, std::less<Price>>& oppSide,
        bool buyerIsAggressor,
        std::unordered_map<OrderId,Order>& orders,
        std::unordered_map<OrderId,PriceLevel::Iterator>& iterMap,
        size_t& askCnt,
        size_t& bidCnt,
        std::vector<TradeReport>& trades,
        std::function<TradeReport(Order&,Order&,Quantity)> exec)
{
    while (aggressor.remainingQty() > 0 && !oppSide.empty()) {
        auto levelIt   = oppSide.begin();
        Price lvlPrice = levelIt->first;
        if (aggressor.isLimit()) {
            if (buyerIsAggressor  && lvlPrice > aggressor.price) break;
            if (!buyerIsAggressor && lvlPrice < aggressor.price) break;
        }
        PriceLevel& level = levelIt->second;
        auto& queue = level.orders();
        auto qIt = queue.begin();
        while (qIt != queue.end() && aggressor.remainingQty() > 0) {
            Order* maker = *qIt;
            if (!maker->isActive()) { qIt = queue.erase(qIt); continue; }
            Quantity fillQty = std::min(aggressor.remainingQty(), maker->remainingQty());
            level.reduceQty(fillQty);
            TradeReport tr = exec(*maker, aggressor, fillQty);
            trades.push_back(tr);
            if (!maker->isActive()) {
                OrderId mid = maker->id;
                qIt = queue.erase(qIt);
                iterMap.erase(mid);
                orders.erase(mid);
                if (buyerIsAggressor) --askCnt;
                else                  --bidCnt;
            } else { ++qIt; }
        }
        if (level.empty()) oppSide.erase(levelIt);
    }
}

static void matchOneSideBid(
        Order& aggressor,
        std::map<Price, PriceLevel, std::greater<Price>>& oppSide,
        bool buyerIsAggressor,
        std::unordered_map<OrderId,Order>& orders,
        std::unordered_map<OrderId,PriceLevel::Iterator>& iterMap,
        size_t& askCnt,
        size_t& bidCnt,
        std::vector<TradeReport>& trades,
        std::function<TradeReport(Order&,Order&,Quantity)> exec)
{
    while (aggressor.remainingQty() > 0 && !oppSide.empty()) {
        auto levelIt   = oppSide.begin();
        Price lvlPrice = levelIt->first;
        if (aggressor.isLimit()) {
            if (!buyerIsAggressor && lvlPrice < aggressor.price) break;
        }
        PriceLevel& level = levelIt->second;
        auto& queue = level.orders();
        auto qIt = queue.begin();
        while (qIt != queue.end() && aggressor.remainingQty() > 0) {
            Order* maker = *qIt;
            if (!maker->isActive()) { qIt = queue.erase(qIt); continue; }
            Quantity fillQty = std::min(aggressor.remainingQty(), maker->remainingQty());
            level.reduceQty(fillQty);
            TradeReport tr = exec(*maker, aggressor, fillQty);
            trades.push_back(tr);
            if (!maker->isActive()) {
                OrderId mid = maker->id;
                qIt = queue.erase(qIt);
                iterMap.erase(mid);
                orders.erase(mid);
                if (!buyerIsAggressor) --bidCnt;
                else                   --askCnt;
            } else { ++qIt; }
        }
        if (level.empty()) oppSide.erase(levelIt);
    }
}

std::vector<TradeReport> OrderBook::matchAgainst(Order& aggressor) {
    std::vector<TradeReport> trades;
    auto execFn = [this](Order& m, Order& t, Quantity q) -> TradeReport {
        return this->executeTrade(m, t, q);
    };
    if (aggressor.isBuy()) {
        matchOneSide(aggressor, asks_, true,
                     orders_, iterMap_, askOrderCount_, bidOrderCount_,
                     trades, execFn);
    } else {
        matchOneSideBid(aggressor, bids_, false,
                        orders_, iterMap_, askOrderCount_, bidOrderCount_,
                        trades, execFn);
    }
    return trades;
}

// ─────────────────────────────────────────────────────────────
// executeTrade
// ─────────────────────────────────────────────────────────────

TradeReport OrderBook::executeTrade(Order& maker, Order& taker, Quantity qty) {
    maker.filledQty += qty;
    maker.status = (maker.remainingQty() == 0)
                       ? OrderStatus::Filled : OrderStatus::PartiallyFilled;

    taker.filledQty += qty;
    taker.status = (taker.remainingQty() == 0)
                       ? OrderStatus::Filled : OrderStatus::PartiallyFilled;

    TradeReport tr;
    tr.tradeId        = tradeIdGen_++;
    tr.symbol         = symbol_;
    tr.price          = maker.price;
    tr.quantity       = qty;
    tr.makerOrderId   = maker.id;
    tr.takerOrderId   = taker.id;
    tr.takerSide      = taker.side;
    tr.timestamp      = nowNs();
    tr.matchLatencyNs = tr.timestamp - taker.createdAt;

    ++tradeCount_;
    if (onTrade_) onTrade_(tr);
    return tr;
}

// ─────────────────────────────────────────────────────────────
// restOrder
// ─────────────────────────────────────────────────────────────

void OrderBook::restOrder(Order& order) {
    if (order.isBuy()) {
        auto [it, _] = bids_.try_emplace(order.price, order.price);
        iterMap_[order.id] = it->second.add(&order);
        ++bidOrderCount_;
    } else {
        auto [it, _] = asks_.try_emplace(order.price, order.price);
        iterMap_[order.id] = it->second.add(&order);
        ++askOrderCount_;
    }
}

// ─────────────────────────────────────────────────────────────
// removeFromLevel
// ─────────────────────────────────────────────────────────────

void OrderBook::removeFromLevel(OrderId id) {
    auto mit = iterMap_.find(id);
    if (mit == iterMap_.end()) return;

    auto& order = orders_.at(id);
    if (order.isBuy()) {
        auto lit = bids_.find(order.price);
        if (lit != bids_.end()) {
            lit->second.remove(mit->second);
            if (lit->second.empty()) bids_.erase(lit);
        }
        --bidOrderCount_;
    } else {
        auto lit = asks_.find(order.price);
        if (lit != asks_.end()) {
            lit->second.remove(mit->second);
            if (lit->second.empty()) asks_.erase(lit);
        }
        --askOrderCount_;
    }
    iterMap_.erase(mit);
}

} // namespace qtl
