/**
 * @file visualization/charts/PriceChart.cpp
 * @brief Implementation of price chart visualization.
 */

#include "visualization/charts/PriceChart.hpp"

namespace qtl {

PriceChart::PriceChart(const Config& config)
    : config_(config) {}

void PriceChart::addDataPoint(const PricePoint& point) {
    data_.push_back(point);
    if (config_.autoScale) {
        needsAutoScale_ = true;
    }
}

void PriceChart::addDataPoints(const std::vector<PricePoint>& points) {
    data_.insert(data_.end(), points.begin(), points.end());
    if (config_.autoScale) {
        needsAutoScale_ = true;
    }
}

void PriceChart::clear() {
    data_.clear();
    indicators_.clear();
}

void PriceChart::setChartType(ChartType type) {
    config_.type = type;
}

void PriceChart::addIndicator(const std::string& name, const std::vector<IndicatorPoint>& points) {
    indicators_[name] = points;
}

void PriceChart::removeIndicator(const std::string& name) {
    indicators_.erase(name);
}

void PriceChart::clearIndicators() {
    indicators_.clear();
}

void PriceChart::setVisibleRange(size_t startIndex, size_t endIndex) {
    visibleStart_ = startIndex;
    visibleEnd_ = endIndex;
}

void PriceChart::render() {
    // In production, this would use ImGui to render the chart
    // For now, this is a stub implementation
    
    // Example ImGui code structure (commented out):
    // if (ImGui::BeginChild(config_.title.c_str(), ImVec2(0, 400), true)) {
    //     ImDrawList* drawList = ImGui::GetWindowDrawList();
    //     ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    //     ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    //     
    //     // Draw grid
    //     // Draw price data based on chart type
    //     // Draw indicators
    //     // Draw crosshair if enabled
    // }
    // ImGui::EndChild();
}

void PriceChart::updateConfig(const Config& config) {
    config_ = config;
    if (config_.autoScale) {
        needsAutoScale_ = true;
    }
}

PriceChart::Statistics PriceChart::calculateStatistics() const {
    Statistics stats;
    
    if (data_.empty()) return stats;
    
    size_t start = visibleStart_;
    size_t end = visibleEnd_ == 0 ? data_.size() : std::min(visibleEnd_, data_.size());
    
    if (start >= end) return stats;
    
    double min = std::numeric_limits<double>::infinity();
    double max = -std::numeric_limits<double>::infinity();
    double sum = 0.0;
    double totalVol = 0.0;
    
    for (size_t i = start; i < end; ++i) {
        min = std::min(min, data_[i].low);
        max = std::max(max, data_[i].high);
        sum += data_[i].close;
        totalVol += data_[i].volume;
    }
    
    stats.min = min;
    stats.max = max;
    stats.average = sum / (end - start);
    stats.totalVolume = totalVol;
    stats.pointCount = end - start;
    
    return stats;
}

void PriceChart::autoScale() {
    if (data_.empty()) return;
    
    double min = std::numeric_limits<double>::infinity();
    double max = -std::numeric_limits<double>::infinity();
    
    size_t start = visibleStart_;
    size_t end = visibleEnd_ == 0 ? data_.size() : std::min(visibleEnd_, data_.size());
    
    for (size_t i = start; i < end; ++i) {
        min = std::min(min, data_[i].low);
        max = std::max(max, data_[i].high);
    }
    
    config_.minPrice = min;
    config_.maxPrice = max;
    needsAutoScale_ = false;
}

// ─────────────────────────────────────────────────────────────
// Order Book Visualizer
// ─────────────────────────────────────────────────────────────

OrderBookVisualizer::OrderBookVisualizer(const Config& config)
    : config_(config) {}

void OrderBookVisualizer::setBids(const std::vector<OrderBookLevel>& bids) {
    bids_ = bids;
}

void OrderBookVisualizer::setAsks(const std::vector<OrderBookLevel>& asks) {
    asks_ = asks;
}

void OrderBookVisualizer::clear() {
    bids_.clear();
    asks_.clear();
}

void OrderBookVisualizer::render() {
    // In production, this would use ImGui to render the order book
    // Example structure:
    // if (ImGui::BeginTable("OrderBook", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
    //     ImGui::TableSetupColumn("Bid Price");
    //     ImGui::TableSetupColumn("Size");
    //     ImGui::TableSetupColumn("Ask Price");
    //     ImGui::TableHeadersRow();
    //     
    //     // Render bid levels (reversed, highest first)
    //     for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
    //         ImGui::TableNextRow();
    //         ImGui::TableSetColumnIndex(0);
    //         ImGui::Text("%.2f", it->price);
    //         ImGui::TableSetColumnIndex(1);
    //         ImGui::Text("%.2f", it->quantity);
    //     }
    //     
    //     ImGui::EndTable();
    // }
}

double OrderBookVisualizer::getMidPrice() const {
    if (bids_.empty() || asks_.empty()) return 0.0;
    return (bids_[0].price + asks_[0].price) / 2.0;
}

double OrderBookVisualizer::getSpread() const {
    if (bids_.empty() || asks_.empty()) return 0.0;
    return asks_[0].price - bids_[0].price;
}

double OrderBookVisualizer::getTotalBidVolume() const {
    double total = 0.0;
    for (const auto& bid : bids_) {
        total += bid.quantity;
    }
    return total;
}

double OrderBookVisualizer::getTotalAskVolume() const {
    double total = 0.0;
    for (const auto& ask : asks_) {
        total += ask.quantity;
    }
    return total;
}

// ─────────────────────────────────────────────────────────────
// Depth Chart
// ─────────────────────────────────────────────────────────────

DepthChart::DepthChart(const Config& config)
    : config_(config) {}

void DepthChart::updateDepth(const std::vector<OrderBookLevel>& bids,
                             const std::vector<OrderBookLevel>& asks) {
    bids_ = bids;
    asks_ = asks;
}

void DepthChart::render() {
    // In production, this would use ImGui to render the depth chart
    // This would show cumulative volume vs price
}

void DepthChart::clear() {
    bids_.clear();
    asks_.clear();
}

} // namespace qtl
