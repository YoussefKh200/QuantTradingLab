#pragma once
/**
 * @file visualization/heatmaps/Heatmap.hpp
 * @brief Heatmap visualization for correlation matrices and other 2D data.
 *
 * Features:
 *  - Correlation matrix visualization
 *  - Order book depth heatmaps
 *  - Performance attribution heatmaps
 *  - Customizable color schemes
 *  - Interactive tooltips
 */

#include <vector>
#include <string>
#include <functional>
#include <memory>
#include <optional>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// Color Scheme
// ─────────────────────────────────────────────────────────────

struct Color {
    float r{0.0f};
    float g{0.0f};
    float b{0.0f};
    float a{1.0f};
    
    Color() = default;
    Color(float red, float green, float blue, float alpha = 1.0f)
        : r(red), g(green), b(blue), a(alpha) {}
};

class ColorScheme {
public:
    virtual ~ColorScheme() = default;
    
    [[nodiscard]] virtual Color getColor(double value, double min, double max) const = 0;
    [[nodiscard]] virtual std::string name() const = 0;
};

// Sequential: Blue (low) to Red (high)
class SequentialScheme : public ColorScheme {
public:
    [[nodiscard]] Color getColor(double value, double min, double max) const override;
    [[nodiscard]] std::string name() const override { return "Sequential"; }
};

// Diverging: Blue (negative) to White (zero) to Red (positive)
class DivergingScheme : public ColorScheme {
public:
    [[nodiscard]] Color getColor(double value, double min, double max) const override;
    [[nodiscard]] std::string name() const override { return "Diverging"; }
};

// ─────────────────────────────────────────────────────────────
// Heatmap Data
// ─────────────────────────────────────────────────────────────

struct HeatmapData {
    std::vector<std::vector<double>> values;
    std::vector<std::string> rowLabels;
    std::vector<std::string> colLabels;
    std::string title;
    
    [[nodiscard]] size_t rows() const noexcept { return values.size(); }
    [[nodiscard]] size_t cols() const noexcept { return values.empty() ? 0 : values[0].size(); }
    
    [[nodiscard]] std::pair<double, double> getRange() const {
        if (values.empty()) return {0.0, 0.0};
        
        double minVal = values[0][0];
        double maxVal = values[0][0];
        
        for (const auto& row : values) {
            for (double val : row) {
                minVal = std::min(minVal, val);
                maxVal = std::max(maxVal, val);
            }
        }
        
        return {minVal, maxVal};
    }
};

// ─────────────────────────────────────────────────────────────
// Heatmap Config
// ─────────────────────────────────────────────────────────────

struct HeatmapConfig {
    std::shared_ptr<ColorScheme> colorScheme;
    bool showValues{true};
    bool showGrid{true};
    bool showColorbar{true};
    double cellPadding{0.1};
    int fontSize{12};
    
    HeatmapConfig() : colorScheme(std::make_shared<DivergingScheme>()) {}
};

// ─────────────────────────────────────────────────────────────
// Heatmap
// ─────────────────────────────────────────────────────────────

class Heatmap {
public:
    explicit Heatmap(const HeatmapConfig& config = HeatmapConfig());
    ~Heatmap() = default;
    
    // Set data
    void setData(const HeatmapData& data);
    void setData(std::vector<std::vector<double>> values,
                 std::vector<std::string> rowLabels = {},
                 std::vector<std::string> colLabels = {});
    
    // Configuration
    void setColorScheme(std::shared_ptr<ColorScheme> scheme);
    void setShowValues(bool show) { showValues_ = show; }
    void setShowGrid(bool show) { showGrid_ = show; }
    void setShowColorbar(bool show) { showColorbar_ = show; }
    
    // Rendering
    void render() const;
    [[nodiscard]] std::string toString() const;
    
    // Convenience builders
    static Heatmap fromCorrelationMatrix(
        const std::vector<std::vector<double>>& correlation,
        const std::vector<std::string>& labels);
    
    static Heatmap fromOrderBookDepth(
        const std::vector<std::pair<double, double>>& bids,
        const std::vector<std::pair<double, double>>& asks);
    
private:
    HeatmapData data_;
    std::shared_ptr<ColorScheme> colorScheme_;
    bool showValues_;
    bool showGrid_;
    bool showColorbar_;
    double cellPadding_;
    int fontSize_;
    
    [[nodiscard]] Color getColor(double value) const;
    void renderCell(size_t row, size_t col, double value) const;
    void renderColorbar() const;
};

// ─────────────────────────────────────────────────────────────
// Correlation Heatmap
// ─────────────────────────────────────────────────────────────

class CorrelationHeatmap : public Heatmap {
public:
    explicit CorrelationHeatmap(const std::vector<std::vector<double>>& correlation,
                                const std::vector<std::string>& labels,
                                const HeatmapConfig& config = HeatmapConfig());
};

// ─────────────────────────────────────────────────────────────
// Order Book Depth Heatmap
// ─────────────────────────────────────────────────────────────

class OrderBookDepthHeatmap : public Heatmap {
public:
    explicit OrderBookDepthHeatmap(
        const std::vector<std::pair<double, double>>& bids,
        const std::vector<std::pair<double, double>>& asks,
        const HeatmapConfig& config = HeatmapConfig());
};

} // namespace qtl
