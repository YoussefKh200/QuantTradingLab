#pragma once
/**
 * @file visualization/charts/PriceChart.hpp
 * @brief Real-time price chart visualization for quantitative trading.
 *
 * Features:
 *  - Candlestick charts
 *  - Line charts
 *  - Volume bars
 *  - Technical indicators overlay
 *  - Zoom and pan
 *  - Multiple timeframes
 */

#include <vector>
#include <string>
#include <memory>
#include <functional>

namespace qtl {

struct PricePoint {
    double timestamp;
    double open;
    double high;
    double low;
    double close;
    double volume;
};

struct IndicatorPoint {
    double timestamp;
    double value;
    std::string name;
};

class PriceChart {
public:
    enum class ChartType {
        Candlestick,
        Line,
        Area,
        Bar
    };

    struct Config {
        ChartType type{ChartType::Candlestick};
        bool showVolume{true};
        bool showGrid{true};
        bool showCrosshair{true};
        bool autoScale{true};
        double minPrice{0.0};
        double maxPrice{0.0};
        std::string title;
        std::string yAxisLabel{"Price"};
        std::string xAxisLabel{"Time"};
    };

    explicit PriceChart(const Config& config = Config{});
    ~PriceChart() = default;

    /**
     * @brief Add price data point
     */
    void addDataPoint(const PricePoint& point);

    /**
     * @brief Add multiple price data points
     */
    void addDataPoints(const std::vector<PricePoint>& points);

    /**
     * @brief Clear all data
     */
    void clear();

    /**
     * @brief Set chart type
     */
    void setChartType(ChartType type);

    /**
     * @brief Add technical indicator overlay
     */
    void addIndicator(const std::string& name, const std::vector<IndicatorPoint>& points);

    /**
     * @brief Remove indicator
     */
    void removeIndicator(const std::string& name);

    /**
     * @brief Clear all indicators
     */
    void clearIndicators();

    /**
     * @brief Set visible data range
     */
    void setVisibleRange(size_t startIndex, size_t endIndex);

    /**
     * @brief Get current data
     */
    [[nodiscard]] const std::vector<PricePoint>& getData() const { return data_; }

    /**
     * @brief Get indicators
     */
    [[nodiscard]] const std::map<std::string, std::vector<IndicatorPoint>>& getIndicators() const {
        return indicators_;
    }

    /**
     * @brief Render the chart (called from GUI framework)
     */
    void render();

    /**
     * @brief Update configuration
     */
    void updateConfig(const Config& config);

    /**
     * @brief Get current configuration
     */
    [[nodiscard]] const Config& getConfig() const { return config_; }

    /**
     * @brief Calculate statistics for visible range
     */
    struct Statistics {
        double min{0.0};
        double max{0.0};
        double average{0.0};
        double totalVolume{0.0};
        size_t pointCount{0};
    };

    [[nodiscard]] Statistics calculateStatistics() const;

private:
    Config config_;
    std::vector<PricePoint> data_;
    std::map<std::string, std::vector<IndicatorPoint>> indicators_;
    size_t visibleStart_{0};
    size_t visibleEnd_{0};
    bool needsAutoScale_{true};

    void autoScale();
};

// ─────────────────────────────────────────────────────────────
// Order Book Visualization
// ─────────────────────────────────────────────────────────────

struct OrderBookLevel {
    double price;
    double quantity;
    size_t orderCount;
};

class OrderBookVisualizer {
public:
    struct Config {
        size_t maxLevels{20};
        bool showCumulative{true};
        bool colorBidsGreen{true};
        bool colorAsksRed{true};
        std::string title{"Order Book"};
    };

    explicit OrderBookVisualizer(const Config& config = Config{});
    ~OrderBookVisualizer() = default;

    /**
     * @brief Update bid levels
     */
    void setBids(const std::vector<OrderBookLevel>& bids);

    /**
     * @brief Update ask levels
     */
    void setAsks(const std::vector<OrderBookLevel>& asks);

    /**
     * @brief Clear order book
     */
    void clear();

    /**
     * @brief Render the order book
     */
    void render();

    /**
     * @brief Get current mid price
     */
    [[nodiscard]] double getMidPrice() const;

    /**
     * @brief Get spread
     */
    [[nodiscard]] double getSpread() const;

    /**
     * @brief Get total bid volume
     */
    [[nodiscard]] double getTotalBidVolume() const;

    /**
     * @brief Get total ask volume
     */
    [[nodiscard]] double getTotalAskVolume() const;

private:
    Config config_;
    std::vector<OrderBookLevel> bids_;
    std::vector<OrderBookLevel> asks_;
};

// ─────────────────────────────────────────────────────────────
// Depth Chart (Market Depth)
// ─────────────────────────────────────────────────────────────

class DepthChart {
public:
    struct Config {
        bool showBids{true};
        bool showAsks{true};
        bool showCumulative{true};
        std::string title{"Market Depth"};
    };

    explicit DepthChart(const Config& config = Config{});
    ~DepthChart() = default;

    /**
     * @brief Update depth data
     */
    void updateDepth(const std::vector<OrderBookLevel>& bids,
                     const std::vector<OrderBookLevel>& asks);

    /**
     * @brief Render depth chart
     */
    void render();

    /**
     * @brief Clear data
     */
    void clear();

private:
    Config config_;
    std::vector<OrderBookLevel> bids_;
    std::vector<OrderBookLevel> asks_;
};

} // namespace qtl
