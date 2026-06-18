/**
 * @file visualization/dashboards/TradingDashboard.cpp
 * @brief Implementation of trading dashboard components.
 */

#include "visualization/dashboards/TradingDashboard.hpp"

namespace qtl {

// ─────────────────────────────────────────────────────────────
// P&L Dashboard
// ─────────────────────────────────────────────────────────────

PnLDashboard::PnLDashboard(const Config& config)
    : config_(config) {}

void PnLDashboard::updatePnL(const PnLData& data) {
    currentPnL_ = data;
    pnlHistory_.push_back(data);
}

void PnLDashboard::updatePnLHistory(const std::vector<PnLData>& history) {
    pnlHistory_ = history;
    if (!history.empty()) {
        currentPnL_ = history.back();
    }
}

void PnLDashboard::clear() {
    pnlHistory_.clear();
    currentPnL_ = PnLData{};
}

void PnLDashboard::render() {
    // In production, this would use ImGui to render the P&L dashboard
    // Example structure:
    // if (ImGui::Begin(config_.title.c_str())) {
    //     ImGui::Text("Total P&L: $%.2f", currentPnL_.totalPnL);
    //     ImGui::Text("Realized: $%.2f", currentPnL_.realizedPnL);
    //     ImGui::Text("Unrealized: $%.2f", currentPnL_.unrealizedPnL);
    //     ImGui::Text("Daily P&L: $%.2f", currentPnL_.dailyPnL);
    //     
    //     // P&L chart would go here
    // }
    // ImGui::End();
}

double PnLDashboard::getTotalPnL() const {
    return currentPnL_.totalPnL;
}

double PnLDashboard::getDailyPnL() const {
    return currentPnL_.dailyPnL;
}

// ─────────────────────────────────────────────────────────────
// Performance Dashboard
// ─────────────────────────────────────────────────────────────

PerformanceDashboard::PerformanceDashboard(const Config& config)
    : config_(config) {}

void PerformanceDashboard::updateMetrics(const PerformanceMetrics& metrics) {
    metrics_ = metrics;
}

void PerformanceDashboard::render() {
    // In production, this would use ImGui to render performance metrics
    // Example structure:
    // if (ImGui::Begin(config_.title.c_str())) {
    //     if (config_.showReturnMetrics) {
    //         ImGui::SeparatorText("Return Metrics");
    //         ImGui::Text("Total Return: %.2f%%", metrics_.totalReturn * 100);
    //         ImGui::Text("Sharpe Ratio: %.2f", metrics_.sharpeRatio);
    //         ImGui::Text("Sortino Ratio: %.2f", metrics_.sortinoRatio);
    //         ImGui::Text("Calmar Ratio: %.2f", metrics_.calmarRatio);
    //         ImGui::Text("Ann. Volatility: %.2f%%", metrics_.annualizedVolatility * 100);
    //     }
    //     
    //     if (config_.showRiskMetrics) {
    //         ImGui::SeparatorText("Risk Metrics");
    //         ImGui::Text("Max Drawdown: %.2f%%", metrics_.maxDrawdown * 100);
    //     }
    //     
    //     if (config_.showTradeStats) {
    //         ImGui::SeparatorText("Trade Statistics");
    //         ImGui::Text("Total Trades: %zu", metrics_.totalTrades);
    //         ImGui::Text("Win Rate: %.2f%%", metrics_.winRate * 100);
    //         ImGui::Text("Profit Factor: %.2f", metrics_.profitFactor);
    //         ImGui::Text("Expectancy: $%.2f", metrics_.expectancy);
    //     }
    // }
    // ImGui::End();
}

// ─────────────────────────────────────────────────────────────
// Position Monitor
// ─────────────────────────────────────────────────────────────

PositionMonitor::PositionMonitor(const Config& config)
    : config_(config) {}

void PositionMonitor::updatePosition(const Position& position) {
    positions_[position.symbol] = position;
}

void PositionMonitor::removePosition(const std::string& symbol) {
    positions_.erase(symbol);
}

void PositionMonitor::clear() {
    positions_.clear();
}

void PositionMonitor::render() {
    // In production, this would use ImGui to render positions table
    // Example structure:
    // if (ImGui::Begin(config_.title.c_str())) {
    //     if (ImGui::BeginTable("Positions", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
    //         ImGui::TableSetupColumn("Symbol");
    //         ImGui::TableSetupColumn("Quantity");
    //         ImGui::TableSetupColumn("Avg Price");
    //         ImGui::TableSetupColumn("Current Price");
    //         ImGui::TableSetupColumn("Unrealized P&L");
    //         ImGui::TableHeadersRow();
    //         
    //         for (const auto& [symbol, pos] : positions_) {
    //             ImGui::TableNextRow();
    //             ImGui::TableSetColumnIndex(0);
    //             ImGui::Text("%s", symbol.c_str());
    //             ImGui::TableSetColumnIndex(1);
    //             ImGui::Text("%.2f", pos.quantity);
    //             ImGui::TableSetColumnIndex(2);
    //             ImGui::Text("%.2f", pos.avgPrice);
    //             ImGui::TableSetColumnIndex(3);
    //             ImGui::Text("%.2f", pos.currentPrice);
    //             ImGui::TableSetColumnIndex(4);
    //             ImGui::Text("$%.2f", pos.unrealizedPnL);
    //         }
    //         
    //         ImGui::EndTable();
    //     }
    // }
    // ImGui::End();
}

double PositionMonitor::getTotalUnrealizedPnL() const {
    double total = 0.0;
    for (const auto& [symbol, pos] : positions_) {
        total += pos.unrealizedPnL;
    }
    return total;
}

double PositionMonitor::getTotalRealizedPnL() const {
    double total = 0.0;
    for (const auto& [symbol, pos] : positions_) {
        total += pos.realizedPnL;
    }
    return total;
}

// ─────────────────────────────────────────────────────────────
// Risk Monitor
// ─────────────────────────────────────────────────────────────

RiskMonitor::RiskMonitor(const Config& config)
    : config_(config) {}

void RiskMonitor::updateRiskMetrics(const RiskMetrics& metrics) {
    metrics_ = metrics;
    riskBreached_ = checkRiskLimits();
}

void RiskMonitor::render() {
    // In production, this would use ImGui to render risk metrics
    // Example structure:
    // if (ImGui::Begin(config_.title.c_str())) {
    //     if (riskBreached_) {
    //         ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
    //         ImGui::Text("WARNING: Risk limits breached!");
    //         ImGui::PopStyleColor();
    //     }
    //     
    //     if (config_.showVaR) {
    //         ImGui::SeparatorText("Value at Risk");
    //         ImGui::Text("VaR 95%%: $%.2f", metrics_.var95);
    //         ImGui::Text("VaR 99%%: $%.2f", metrics_.var99);
    //         ImGui::Text("CVaR 95%%: $%.2f", metrics_.cvar95);
    //     }
    //     
    //     if (config_.showExposure) {
    //         ImGui::SeparatorText("Exposure");
    //         ImGui::Text("Gross Exposure: $%.2f", metrics_.grossExposure);
    //         ImGui::Text("Net Exposure: $%.2f", metrics_.netExposure);
    //         ImGui::Text("Portfolio Value: $%.2f", metrics_.portfolioValue);
    //     }
    //     
    //     if (config_.showLeverage) {
    //         ImGui::SeparatorText("Leverage");
    //         ImGui::Text("Leverage: %.2fx", metrics_.leverage);
    //         ImGui::Text("Capital Utilization: %.2f%%", metrics_.utilization * 100);
    //     }
    // }
    // ImGui::End();
}

bool RiskMonitor::checkRiskLimits() const {
    // Check if VaR exceeds threshold
    if (metrics_.var95 > config_.varThreshold * metrics_.portfolioValue) {
        return true;
    }
    
    // Check if leverage exceeds limit
    if (metrics_.leverage > config_.leverageLimit) {
        return true;
    }
    
    return false;
}

// ─────────────────────────────────────────────────────────────
// Trade History
// ─────────────────────────────────────────────────────────────

TradeHistory::TradeHistory(const Config& config)
    : config_(config) {}

void TradeHistory::addTrade(const Trade& trade) {
    trades_.push_back(trade);
}

void TradeHistory::clear() {
    trades_.clear();
}

void TradeHistory::render() {
    // In production, this would use ImGui to render trade history table
    // Example structure:
    // if (ImGui::Begin(config_.title.c_str())) {
    //     if (ImGui::BeginTable("TradeHistory", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
    //         ImGui::TableSetupColumn("Time");
    //         ImGui::TableSetupColumn("Symbol");
    //         ImGui::TableSetupColumn("Side");
    //         ImGui::TableSetupColumn("Qty");
    //         ImGui::TableSetupColumn("Price");
    //         ImGui::TableSetupColumn("P&L");
    //         ImGui::TableSetupColumn("Strategy");
    //         ImGui::TableHeadersRow();
    //         
    //         size_t startIdx = trades_.size() > config_.maxVisibleTrades 
    //                         ? trades_.size() - config_.maxVisibleTrades : 0;
    //         
    //         for (size_t i = startIdx; i < trades_.size(); ++i) {
    //             const auto& trade = trades_[i];
    //             ImGui::TableNextRow();
    //             ImGui::TableSetColumnIndex(0);
    //             ImGui::Text("%ld", trade.timestamp);
    //             ImGui::TableSetColumnIndex(1);
    //             ImGui::Text("%s", trade.symbol.c_str());
    //             ImGui::TableSetColumnIndex(2);
    //             ImGui::Text("%s", trade.side.c_str());
    //             ImGui::TableSetColumnIndex(3);
    //             ImGui::Text("%.2f", trade.quantity);
    //             ImGui::TableSetColumnIndex(4);
    //             ImGui::Text("%.2f", trade.price);
    //             ImGui::TableSetColumnIndex(5);
    //             ImGui::Text("$%.2f", trade.pnl);
    //             ImGui::TableSetColumnIndex(6);
    //             ImGui::Text("%s", trade.strategy.c_str());
    //         }
    //         
    //         ImGui::EndTable();
    //     }
    // }
    // ImGui::End();
}

// ─────────────────────────────────────────────────────────────
// Main Trading Dashboard
// ─────────────────────────────────────────────────────────────

TradingDashboard::TradingDashboard(const Config& config)
    : config_(config) {}

void TradingDashboard::initialize() {
    pnlDashboard_ = std::make_unique<PnLDashboard>(PnLDashboard::Config{});
    performanceDashboard_ = std::make_unique<PerformanceDashboard>(PerformanceDashboard::Config{});
    positionMonitor_ = std::make_unique<PositionMonitor>(PositionMonitor::Config{});
    riskMonitor_ = std::make_unique<RiskMonitor>(RiskMonitor::Config{});
    tradeHistory_ = std::make_unique<TradeHistory>(TradeHistory::Config{});
}

void TradingDashboard::render() {
    // In production, this would use ImGui to render the entire dashboard layout
    // Example structure:
    // ImGui::Begin(config_.title.c_str());
    // 
    // // Main layout with dockable windows
    // if (ImGui::BeginTable("DashboardLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings)) {
    //     ImGui::TableSetupColumn("Left", ImGuiTableColumnFlags_WidthStretch, 0.6f);
    //     ImGui::TableSetupColumn("Right", ImGuiTableColumnFlags_WidthStretch, 0.4f);
    //     
    //     ImGui::TableNextRow();
    //     
    //     // Left column - Price chart and order book
    //     ImGui::TableSetColumnIndex(0);
    //     if (config_.showPriceChart) {
    //         // Price chart rendering
    //     }
    //     if (config_.showOrderBook) {
    //         // Order book rendering
    //     }
    //     
    //     // Right column - P&L, performance, positions, risk
    //     ImGui::TableSetColumnIndex(1);
    //     if (config_.showPnL) {
    //         pnlDashboard_->render();
    //     }
    //     if (config_.showPerformance) {
    //         performanceDashboard_->render();
    //     }
    //     if (config_.showPositions) {
    //         positionMonitor_->render();
    //     }
    //     if (config_.showRisk) {
    //         riskMonitor_->render();
    //     }
    //     
    //     ImGui::EndTable();
    // }
    // 
    // // Bottom - Trade history
    // if (config_.showTradeHistory) {
    //     tradeHistory_->render();
    // }
    // 
    // ImGui::End();
}

void TradingDashboard::updatePnL(const PnLData& data) {
    if (pnlDashboard_) {
        pnlDashboard_->updatePnL(data);
    }
}

void TradingDashboard::updatePerformance(const PerformanceMetrics& metrics) {
    if (performanceDashboard_) {
        performanceDashboard_->updateMetrics(metrics);
    }
}

void TradingDashboard::updatePosition(const Position& position) {
    if (positionMonitor_) {
        positionMonitor_->updatePosition(position);
    }
}

void TradingDashboard::updateRisk(const RiskMetrics& metrics) {
    if (riskMonitor_) {
        riskMonitor_->updateRiskMetrics(metrics);
    }
}

void TradingDashboard::addTrade(const Trade& trade) {
    if (tradeHistory_) {
        tradeHistory_->addTrade(trade);
    }
}

} // namespace qtl
