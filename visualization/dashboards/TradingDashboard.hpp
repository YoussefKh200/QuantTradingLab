#pragma once
/**
 * @file visualization/dashboards/TradingDashboard.hpp
 * @brief Comprehensive trading dashboard for quantitative trading.
 *
 * Features:
 *  - P&L tracking and visualization
 *  - Strategy performance metrics
 *  - Position monitoring
 *  - Risk metrics display
 *  - Real-time order flow
 *  - Trade history
 */

#include <vector>
#include <string>
#include <map>
#include <memory>
#include <functional>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// P&L Dashboard
// ─────────────────────────────────────────────────────────────

struct PnLData {
    double timestamp;
    double realizedPnL;
    double unrealizedPnL;
    double totalPnL;
    double dailyPnL;
};

class PnLDashboard {
public:
    struct Config {
        bool showRealized{true};
        bool showUnrealized{true};
        bool showDaily{true};
        bool showCumulative{true};
        std::string title{"P&L Dashboard"};
    };

    explicit PnLDashboard(const Config& config = Config{});
    ~PnLDashboard() = default;

    /**
     * @brief Update P&L data
     */
    void updatePnL(const PnLData& data);

    /**
     * @brief Update P&L history
     */
    void updatePnLHistory(const std::vector<PnLData>& history);

    /**
     * @brief Clear data
     */
    void clear();

    /**
     * @brief Render the P&L dashboard
     */
    void render();

    /**
     * @brief Get current total P&L
     */
    [[nodiscard]] double getTotalPnL() const;

    /**
     * @brief Get daily P&L
     */
    [[nodiscard]] double getDailyPnL() const;

private:
    Config config_;
    std::vector<PnLData> pnlHistory_;
    PnLData currentPnL_;
};

// ─────────────────────────────────────────────────────────────
// Performance Metrics Dashboard
// ─────────────────────────────────────────────────────────────

struct PerformanceMetrics {
    double sharpeRatio{0.0};
    double sortinoRatio{0.0};
    double maxDrawdown{0.0};
    double winRate{0.0};
    double profitFactor{0.0};
    double expectancy{0.0};
    double totalReturn{0.0};
    double annualizedVolatility{0.0};
    double calmarRatio{0.0};
    size_t totalTrades{0};
    size_t winningTrades{0};
    size_t losingTrades{0};
};

class PerformanceDashboard {
public:
    struct Config {
        bool showRiskMetrics{true};
        bool showTradeStats{true};
        bool showReturnMetrics{true};
        std::string title{"Performance Metrics"};
    };

    explicit PerformanceDashboard(const Config& config = Config{});
    ~PerformanceDashboard() = default;

    /**
     * @brief Update performance metrics
     */
    void updateMetrics(const PerformanceMetrics& metrics);

    /**
     * @brief Render the performance dashboard
     */
    void render();

    /**
     * @brief Get current metrics
     */
    [[nodiscard]] const PerformanceMetrics& getMetrics() const { return metrics_; }

private:
    Config config_;
    PerformanceMetrics metrics_;
};

// ─────────────────────────────────────────────────────────────
// Position Monitor
// ─────────────────────────────────────────────────────────────

struct Position {
    std::string symbol;
    double quantity;
    double avgPrice;
    double currentPrice;
    double unrealizedPnL;
    double realizedPnL;
    long timestamp;
};

class PositionMonitor {
public:
    struct Config {
        bool showUnrealizedPnL{true};
        bool showRealizedPnL{true};
        bool sortByPnL{true};
        std::string title{"Positions"};
    };

    explicit PositionMonitor(const Config& config = Config{});
    ~PositionMonitor() = default;

    /**
     * @brief Update position
     */
    void updatePosition(const Position& position);

    /**
     * @brief Remove position
     */
    void removePosition(const std::string& symbol);

    /**
     * @brief Clear all positions
     */
    void clear();

    /**
     * @brief Render position monitor
     */
    void render();

    /**
     * @brief Get all positions
     */
    [[nodiscard]] const std::map<std::string, Position>& getPositions() const {
        return positions_;
    }

    /**
     * @brief Get total unrealized P&L
     */
    [[nodiscard]] double getTotalUnrealizedPnL() const;

    /**
     * @brief Get total realized P&L
     */
    [[nodiscard]] double getTotalRealizedPnL() const;

private:
    Config config_;
    std::map<std::string, Position> positions_;
};

// ─────────────────────────────────────────────────────────────
// Risk Monitor
// ─────────────────────────────────────────────────────────────

struct RiskMetrics {
    double portfolioValue{0.0};
    double grossExposure{0.0};
    double netExposure{0.0};
    double var95{0.0};
    double var99{0.0};
    double cvar95{0.0};
    double beta{0.0};
    double leverage{0.0};
    double utilization{0.0}; // Capital utilization
};

class RiskMonitor {
public:
    struct Config {
        bool showVaR{true};
        bool showExposure{true};
        bool showLeverage{true};
        std::string title{"Risk Monitor"};
        double varThreshold{0.02}; // 2% VaR threshold
        double leverageLimit{3.0}; // 3x leverage limit
    };

    explicit RiskMonitor(const Config& config = Config{});
    ~RiskMonitor() = default;

    /**
     * @brief Update risk metrics
     */
    void updateRiskMetrics(const RiskMetrics& metrics);

    /**
     * @brief Render risk monitor
     */
    void render();

    /**
     * @brief Check if risk limits are breached
     */
    [[nodiscard]] bool checkRiskLimits() const;

    /**
     * @brief Get current risk metrics
     */
    [[nodiscard]] const RiskMetrics& getMetrics() const { return metrics_; }

private:
    Config config_;
    RiskMetrics metrics_;
    bool riskBreached_{false};
};

// ─────────────────────────────────────────────────────────────
// Trade History
// ─────────────────────────────────────────────────────────────

struct Trade {
    std::string symbol;
    std::string side; // "BUY" or "SELL"
    double quantity;
    double price;
    double commission;
    double pnl;
    long timestamp;
    std::string strategy;
};

class TradeHistory {
public:
    struct Config {
        size_t maxVisibleTrades{100};
        bool showPnL{true};
        bool showStrategy{true};
        std::string title{"Trade History"};
    };

    explicit TradeHistory(const Config& config = Config{});
    ~TradeHistory() = default;

    /**
     * @brief Add trade
     */
    void addTrade(const Trade& trade);

    /**
     * @brief Clear history
     */
    void clear();

    /**
     * @brief Render trade history
     */
    void render();

    /**
     * @brief Get all trades
     */
    [[nodiscard]] const std::vector<Trade>& getTrades() const { return trades_; }

private:
    Config config_;
    std::vector<Trade> trades_;
};

// ─────────────────────────────────────────────────────────────
// Main Trading Dashboard
// ─────────────────────────────────────────────────────────────

class TradingDashboard {
public:
    struct Config {
        bool showPnL{true};
        bool showPerformance{true};
        bool showPositions{true};
        bool showRisk{true};
        bool showTradeHistory{true};
        bool showOrderBook{true};
        bool showPriceChart{true};
        std::string title{"QuantTradingLab Dashboard"};
    };

    explicit TradingDashboard(const Config& config = Config{});
    ~TradingDashboard() = default;

    /**
     * @brief Initialize dashboard components
     */
    void initialize();

    /**
     * @brief Render the entire dashboard
     */
    void render();

    /**
     * @brief Update P&L data
     */
    void updatePnL(const PnLData& data);

    /**
     * @brief Update performance metrics
     */
    void updatePerformance(const PerformanceMetrics& metrics);

    /**
     * @brief Update position
     */
    void updatePosition(const Position& position);

    /**
     * @brief Update risk metrics
     */
    void updateRisk(const RiskMetrics& metrics);

    /**
     * @brief Add trade
     */
    void addTrade(const Trade& trade);

    /**
     * @brief Get P&L dashboard
     */
    [[nodiscard]] PnLDashboard& getPnLDashboard() { return *pnlDashboard_; }

    /**
     * @brief Get performance dashboard
     */
    [[nodiscard]] PerformanceDashboard& getPerformanceDashboard() { return *performanceDashboard_; }

    /**
     * @brief Get position monitor
     */
    [[nodiscard]] PositionMonitor& getPositionMonitor() { return *positionMonitor_; }

    /**
     * @brief Get risk monitor
     */
    [[nodiscard]] RiskMonitor& getRiskMonitor() { return *riskMonitor_; }

    /**
     * @brief Get trade history
     */
    [[nodiscard]] TradeHistory& getTradeHistory() { return *tradeHistory_; }

private:
    Config config_;
    std::unique_ptr<PnLDashboard> pnlDashboard_;
    std::unique_ptr<PerformanceDashboard> performanceDashboard_;
    std::unique_ptr<PositionMonitor> positionMonitor_;
    std::unique_ptr<RiskMonitor> riskMonitor_;
    std::unique_ptr<TradeHistory> tradeHistory_;
};

} // namespace qtl
