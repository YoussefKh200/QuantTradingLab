#pragma once
/**
 * @file exchange/execution/ExecutionReport.hpp
 * @brief FIX-style execution report produced for every order lifecycle event.
 *
 * Architecture
 * ────────────
 * In FIX protocol, an ExecutionReport (MsgType=8) covers:
 *   - New order acknowledgement  (ExecType=0)
 *   - Partial fill               (ExecType=F, OrdStatus=1)
 *   - Full fill                  (ExecType=F, OrdStatus=2)
 *   - Cancel confirmation        (ExecType=4)
 *   - Reject                     (ExecType=8)
 *   - Pending cancel             (ExecType=6)
 *
 * We mirror this closely without the wire-format overhead.
 * The MatchingEngine creates one ExecutionReport per lifecycle transition
 * and routes it to:
 *   (a) the originating strategy (via FillEvent / EventLoop)
 *   (b) the PortfolioManager  (for P&L and position update)
 *   (c) the RiskEngine        (for real-time limit checking)
 *   (d) the Logger            (for audit trail)
 *
 * Key fields
 * ──────────
 *   execType   — nature of this report
 *   ordStatus  — current cumulative status of the order
 *   leavesQty  — quantity still open (0 = done)
 *   cumQty     — total filled so far
 *   lastQty    — quantity filled in THIS report (0 for non-fill types)
 *   lastPx     — price of THIS fill (0 for non-fill types)
 *   avgPx      — volume-weighted average fill price so far
 *   commission — cumulative brokerage cost
 *   latencyNs  — nanoseconds from order submission to this report
 */

#include "core/Types.hpp"
#include "exchange/orderbook/Order.hpp"
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdint>
#include <atomic>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// ExecType — nature of the execution report
// ─────────────────────────────────────────────────────────────

enum class ExecType : uint8_t {
    New           = 0,   ///< Order acknowledged by exchange
    PartialFill   = 1,   ///< Partial execution
    Fill          = 2,   ///< Full execution (order done)
    Cancelled     = 4,   ///< Cancel confirmed
    PendingCancel = 6,   ///< Cancel request accepted, pending confirmation
    Rejected      = 8,   ///< Order rejected (risk check, bad params, etc.)
    Replaced      = 5,   ///< Modify/replace confirmed
};

inline std::string execTypeName(ExecType t) {
    switch (t) {
        case ExecType::New:           return "NEW";
        case ExecType::PartialFill:   return "PARTIAL_FILL";
        case ExecType::Fill:          return "FILL";
        case ExecType::Cancelled:     return "CANCELLED";
        case ExecType::PendingCancel: return "PENDING_CANCEL";
        case ExecType::Rejected:      return "REJECTED";
        case ExecType::Replaced:      return "REPLACED";
    }
    return "UNKNOWN";
}

inline std::string orderStatusName(OrderStatus s) {
    switch (s) {
        case OrderStatus::New:              return "NEW";
        case OrderStatus::PartiallyFilled:  return "PARTIALLY_FILLED";
        case OrderStatus::Filled:           return "FILLED";
        case OrderStatus::Cancelled:        return "CANCELLED";
        case OrderStatus::Rejected:         return "REJECTED";
        case OrderStatus::PendingCancel:    return "PENDING_CANCEL";
    }
    return "UNKNOWN";
}

// ─────────────────────────────────────────────────────────────
// RejectionReason — why an order was rejected
// ─────────────────────────────────────────────────────────────

enum class RejectionReason : uint8_t {
    None             = 0,
    DuplicateOrderId,
    InvalidQuantity,
    InvalidPrice,
    InvalidSymbol,
    RiskLimitBreach,
    InsufficientLiquidity,  // FOK
    UnknownOrder,           // cancel/modify of non-existent order
    MarketClosed,
};

inline std::string rejectionReasonName(RejectionReason r) {
    switch (r) {
        case RejectionReason::None:                  return "NONE";
        case RejectionReason::DuplicateOrderId:      return "DUPLICATE_ORDER_ID";
        case RejectionReason::InvalidQuantity:       return "INVALID_QUANTITY";
        case RejectionReason::InvalidPrice:          return "INVALID_PRICE";
        case RejectionReason::InvalidSymbol:         return "INVALID_SYMBOL";
        case RejectionReason::RiskLimitBreach:       return "RISK_LIMIT_BREACH";
        case RejectionReason::InsufficientLiquidity: return "INSUFFICIENT_LIQUIDITY";
        case RejectionReason::UnknownOrder:          return "UNKNOWN_ORDER";
        case RejectionReason::MarketClosed:          return "MARKET_CLOSED";
    }
    return "UNKNOWN";
}

// ─────────────────────────────────────────────────────────────
// ExecutionReport
// ─────────────────────────────────────────────────────────────

struct ExecutionReport {
    // ── Identity ─────────────────────────────────────────────
    OrderId     orderId{0};
    TradeId     execId{0};        ///< Unique ID for this report
    Symbol      symbol;
    std::string strategyId;

    // ── Order specification (echoed back) ────────────────────
    Side        side{Side::Buy};
    OrderType   orderType{OrderType::Limit};
    Price       orderPrice{0.0};  ///< Original limit price submitted
    Quantity    orderQty{0};      ///< Original quantity submitted

    // ── Execution type ───────────────────────────────────────
    ExecType    execType{ExecType::New};
    OrderStatus ordStatus{OrderStatus::New};
    RejectionReason rejectReason{RejectionReason::None};

    // ── Fill fields (non-zero only for fill reports) ─────────
    Price       lastPx{0.0};     ///< Fill price of THIS execution
    Quantity    lastQty{0};       ///< Fill quantity of THIS execution
    Price       avgPx{0.0};      ///< VWAP of all fills so far
    Quantity    cumQty{0};        ///< Total quantity filled so far
    Quantity    leavesQty{0};     ///< Remaining open quantity
    double      lastCommission{0.0}; ///< Commission for THIS fill
    double      cumCommission{0.0};  ///< Total commission so far
    bool        isTaker{false};   ///< Was this fill aggressive (taker)?

    // ── Timing ───────────────────────────────────────────────
    Timestamp   submitTime{0};    ///< When order was submitted
    Timestamp   reportTime{0};    ///< When this report was generated
    int64_t     latencyNs{0};     ///< reportTime - submitTime

    // ── Helpers ──────────────────────────────────────────────

    [[nodiscard]] bool isFill() const noexcept {
        return execType == ExecType::PartialFill ||
               execType == ExecType::Fill;
    }

    [[nodiscard]] bool isDone() const noexcept {
        return ordStatus == OrderStatus::Filled ||
               ordStatus == OrderStatus::Cancelled ||
               ordStatus == OrderStatus::Rejected;
    }

    // ── Pretty-print ─────────────────────────────────────────

    [[nodiscard]] std::string toString() const {
        std::ostringstream oss;
        oss << "ExecRpt["
            << " id="       << orderId
            << " sym="      << symbol
            << " side="     << (side == Side::Buy ? "BUY" : "SELL")
            << " type="     << execTypeName(execType)
            << " status="   << orderStatusName(ordStatus);
        if (isFill()) {
            oss << " lastPx="  << std::fixed << std::setprecision(4) << lastPx
                << " lastQty=" << lastQty
                << " avgPx="   << std::fixed << std::setprecision(4) << avgPx
                << " cumQty="  << cumQty
                << " leaves="  << leavesQty
                << " comm="    << std::fixed << std::setprecision(4) << lastCommission
                << " taker="   << (isTaker ? "Y" : "N");
        }
        if (execType == ExecType::Rejected) {
            oss << " reject=" << rejectionReasonName(rejectReason);
        }
        oss << " lat=" << latencyNs << "ns ]";
        return oss.str();
    }
};

// ─────────────────────────────────────────────────────────────
// ExecutionReportBuilder — factory methods for each exec type
// ─────────────────────────────────────────────────────────────

class ExecutionReportBuilder {
public:
    /// Monotonically increasing exec ID (shared across all symbols).
    static uint64_t nextExecId() noexcept {
        static std::atomic<uint64_t> gen{1};
        return gen.fetch_add(1, std::memory_order_relaxed);
    }

    static ExecutionReport makeNew(const Order& order) {
        ExecutionReport r;
        r.execId      = nextExecId();
        r.orderId     = order.id;
        r.symbol      = order.symbol;
        r.strategyId  = order.strategyId;
        r.side        = order.side;
        r.orderType   = order.type;
        r.orderPrice  = order.price;
        r.orderQty    = order.quantity;
        r.execType    = ExecType::New;
        r.ordStatus   = OrderStatus::New;
        r.leavesQty   = order.quantity;
        r.submitTime  = order.createdAt;
        r.reportTime  = nowNs();
        r.latencyNs   = r.reportTime - r.submitTime;
        return r;
    }

    static ExecutionReport makeFill(const Order& order,
                                     const TradeReport& trade,
                                     double commissionRate,
                                     bool taker) {
        ExecutionReport r;
        r.execId       = nextExecId();
        r.orderId      = order.id;
        r.symbol       = order.symbol;
        r.strategyId   = order.strategyId;
        r.side         = order.side;
        r.orderType    = order.type;
        r.orderPrice   = order.price;
        r.orderQty     = order.quantity;
        r.lastPx       = trade.price;
        r.lastQty      = trade.quantity;
        r.cumQty       = order.filledQty;
        r.leavesQty    = order.remainingQty();
        r.isTaker      = taker;
        r.lastCommission = trade.price * trade.quantity * commissionRate;
        r.cumCommission  = r.lastCommission; // incremented by engine
        r.submitTime   = order.createdAt;
        r.reportTime   = trade.timestamp;
        r.latencyNs    = r.reportTime - r.submitTime;
        r.execType     = (r.leavesQty == 0)
                             ? ExecType::Fill
                             : ExecType::PartialFill;
        r.ordStatus    = (r.leavesQty == 0)
                             ? OrderStatus::Filled
                             : OrderStatus::PartiallyFilled;
        // VWAP: simplified (caller can track running total externally)
        r.avgPx        = trade.price;
        return r;
    }

    static ExecutionReport makeCancel(const Order& order) {
        ExecutionReport r;
        r.execId      = nextExecId();
        r.orderId     = order.id;
        r.symbol      = order.symbol;
        r.strategyId  = order.strategyId;
        r.side        = order.side;
        r.orderType   = order.type;
        r.orderPrice  = order.price;
        r.orderQty    = order.quantity;
        r.execType    = ExecType::Cancelled;
        r.ordStatus   = OrderStatus::Cancelled;
        r.cumQty      = order.filledQty;
        r.leavesQty   = 0;
        r.submitTime  = order.createdAt;
        r.reportTime  = nowNs();
        r.latencyNs   = r.reportTime - r.submitTime;
        return r;
    }

    static ExecutionReport makeReject(OrderId id,
                                       const Symbol& sym,
                                       const std::string& strat,
                                       RejectionReason reason) {
        ExecutionReport r;
        r.execId       = nextExecId();
        r.orderId      = id;
        r.symbol       = sym;
        r.strategyId   = strat;
        r.execType     = ExecType::Rejected;
        r.ordStatus    = OrderStatus::Rejected;
        r.rejectReason = reason;
        r.reportTime   = nowNs();
        return r;
    }

    static ExecutionReport makeReplace(const Order& order) {
        ExecutionReport r;
        r.execId     = nextExecId();
        r.orderId    = order.id;
        r.symbol     = order.symbol;
        r.strategyId = order.strategyId;
        r.side       = order.side;
        r.orderType  = order.type;
        r.orderPrice = order.price;
        r.orderQty   = order.quantity;
        r.execType   = ExecType::Replaced;
        r.ordStatus  = OrderStatus::New;
        r.leavesQty  = order.remainingQty();
        r.cumQty     = order.filledQty;
        r.reportTime = nowNs();
        r.submitTime = order.createdAt;
        r.latencyNs  = r.reportTime - r.submitTime;
        return r;
    }
};

} // namespace qtl
