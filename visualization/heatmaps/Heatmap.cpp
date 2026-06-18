/**
 * @file visualization/heatmaps/Heatmap.cpp
 * @brief Heatmap visualization implementation.
 */

#include "visualization/heatmaps/Heatmap.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <algorithm>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// Color Scheme Implementations
// ─────────────────────────────────────────────────────────────

Color SequentialScheme::getColor(double value, double min, double max) const {
    // Normalize to [0, 1]
    double normalized = (max - min) > 0 ? (value - min) / (max - min) : 0.5;
    normalized = std::clamp(normalized, 0.0, 1.0);
    
    // Blue (low) to Red (high)
    return Color{
        static_cast<float>(normalized),           // R increases
        0.0f,                                    // G stays 0
        static_cast<float>(1.0 - normalized),     // B decreases
        1.0f
    };
}

Color DivergingScheme::getColor(double value, double min, double max) const {
    // Normalize to [-1, 1] with 0 at the midpoint
    double range = max - min;
    if (range == 0) return Color{1.0f, 1.0f, 1.0f, 1.0f};
    
    double normalized = 2.0 * (value - min) / range - 1.0; // [-1, 1]
    normalized = std::clamp(normalized, -1.0, 1.0);
    
    // Blue (negative) to White (zero) to Red (positive)
    if (normalized < 0) {
        // Blue to White
        return Color{
            static_cast<float>(1.0 + normalized),  // R increases to 1
            static_cast<float>(1.0 + normalized),  // G increases to 1
            1.0f,                                   // B stays 1
            1.0f
        };
    } else {
        // White to Red
        return Color{
            1.0f,                                   // R stays 1
            static_cast<float>(1.0 - normalized),  // G decreases
            static_cast<float>(1.0 - normalized),  // B decreases
            1.0f
        };
    }
}

// ─────────────────────────────────────────────────────────────
// Heatmap Implementation
// ─────────────────────────────────────────────────────────────

Heatmap::Heatmap(const HeatmapConfig& config)
    : colorScheme_(config.colorScheme)
    , showValues_(config.showValues)
    , showGrid_(config.showGrid)
    , showColorbar_(config.showColorbar)
    , cellPadding_(config.cellPadding)
    , fontSize_(config.fontSize)
{
}

void Heatmap::setData(const HeatmapData& data) {
    data_ = data;
}

void Heatmap::setData(std::vector<std::vector<double>> values,
                      std::vector<std::string> rowLabels,
                      std::vector<std::string> colLabels) {
    data_.values = std::move(values);
    data_.rowLabels = std::move(rowLabels);
    data_.colLabels = std::move(colLabels);
}

void Heatmap::setColorScheme(std::shared_ptr<ColorScheme> scheme) {
    colorScheme_ = std::move(scheme);
}

Color Heatmap::getColor(double value) const {
    auto [min, max] = data_.getRange();
    return colorScheme_->getColor(value, min, max);
}

void Heatmap::renderCell(size_t row, size_t col, double value) const {
    Color color = getColor(value);
    
    std::cout << "\033[48;2;"
              << static_cast<int>(color.r * 255) << ";"
              << static_cast<int>(color.g * 255) << ";"
              << static_cast<int>(color.b * 255) << "m";
    
    if (showValues_) {
        std::cout << std::fixed << std::setprecision(2) << std::setw(8) << value;
    } else {
        std::cout << "        ";
    }
    
    std::cout << "\033[0m"; // Reset color
}

void Heatmap::renderColorbar() const {
    if (!showColorbar_) return;
    
    std::cout << "\nColorbar: ";
    auto [min, max] = data_.getRange();
    
    for (int i = 0; i < 20; ++i) {
        double value = min + (max - min) * i / 19.0;
        Color color = getColor(value);
        
        std::cout << "\033[48;2;"
                  << static_cast<int>(color.r * 255) << ";"
                  << static_cast<int>(color.g * 255) << ";"
                  << static_cast<int>(color.b * 255) << "m  \033[0m";
    }
    
    std::cout << " [" << min << " to " << max << "]\n";
}

void Heatmap::render() const {
    if (data_.values.empty()) {
        std::cout << "No data to render\n";
        return;
    }
    
    std::cout << "\n" << data_.title << "\n";
    
    // Print column labels
    if (!data_.colLabels.empty()) {
        std::cout << "        ";
        for (const auto& label : data_.colLabels) {
            std::cout << std::setw(8) << label.substr(0, 7) << " ";
        }
        std::cout << "\n";
    }
    
    // Print rows
    for (size_t row = 0; row < data_.rows(); ++row) {
        // Print row label
        if (row < data_.rowLabels.size()) {
            std::cout << std::setw(8) << data_.rowLabels[row].substr(0, 7) << " ";
        } else {
            std::cout << "        ";
        }
        
        // Print cells
        for (size_t col = 0; col < data_.cols(); ++col) {
            renderCell(row, col, data_.values[row][col]);
            if (showGrid_ && col < data_.cols() - 1) {
                std::cout << " ";
            }
        }
        std::cout << "\n";
    }
    
    renderColorbar();
}

std::string Heatmap::toString() const {
    std::ostringstream oss;
    
    if (data_.values.empty()) {
        oss << "No data";
        return oss.str();
    }
    
    oss << data_.title << " (" << data_.rows() << "x" << data_.cols() << ")\n";
    auto [min, max] = data_.getRange();
    oss << "Range: [" << min << ", " << max << "]\n";
    oss << "Color scheme: " << colorScheme_->name();
    
    return oss.str();
}

Heatmap Heatmap::fromCorrelationMatrix(
    const std::vector<std::vector<double>>& correlation,
    const std::vector<std::string>& labels) {
    
    Heatmap heatmap;
    HeatmapData data;
    data.values = correlation;
    data.rowLabels = labels;
    data.colLabels = labels;
    data.title = "Correlation Matrix";
    
    heatmap.setData(data);
    heatmap.setColorScheme(std::make_shared<DivergingScheme>());
    
    return heatmap;
}

Heatmap Heatmap::fromOrderBookDepth(
    const std::vector<std::pair<double, double>>& bids,
    const std::vector<std::pair<double, double>>& asks) {
    
    Heatmap heatmap;
    HeatmapData data;
    
    // Create a 2D representation: price levels vs time buckets
    // For simplicity, we'll organize by price levels
    size_t levels = std::max(bids.size(), asks.size());
    
    data.values.resize(levels, std::vector<double>(2));
    data.rowLabels.resize(levels);
    data.colLabels = {"Bids", "Asks"};
    data.title = "Order Book Depth";
    
    for (size_t i = 0; i < levels; ++i) {
        if (i < bids.size()) {
            data.values[i][0] = bids[i].second; // Quantity
            data.rowLabels[i] = std::to_string(bids[i].first);
        } else {
            data.values[i][0] = 0.0;
            data.rowLabels[i] = "-";
        }
        
        if (i < asks.size()) {
            data.values[i][1] = asks[i].second; // Quantity
        } else {
            data.values[i][1] = 0.0;
        }
    }
    
    heatmap.setData(data);
    heatmap.setColorScheme(std::make_shared<SequentialScheme>());
    
    return heatmap;
}

// ─────────────────────────────────────────────────────────────
// Correlation Heatmap
// ─────────────────────────────────────────────────────────────

CorrelationHeatmap::CorrelationHeatmap(
    const std::vector<std::vector<double>>& correlation,
    const std::vector<std::string>& labels,
    const HeatmapConfig& config)
    : Heatmap(config)
{
    HeatmapData data;
    data.values = correlation;
    data.rowLabels = labels;
    data.colLabels = labels;
    data.title = "Correlation Matrix";
    
    setData(data);
    setColorScheme(std::make_shared<DivergingScheme>());
}

// ─────────────────────────────────────────────────────────────
// Order Book Depth Heatmap
// ─────────────────────────────────────────────────────────────

OrderBookDepthHeatmap::OrderBookDepthHeatmap(
    const std::vector<std::pair<double, double>>& bids,
    const std::vector<std::pair<double, double>>& asks,
    const HeatmapConfig& config)
    : Heatmap(config)
{
    setData(fromOrderBookDepth(bids, asks).data_);
    setColorScheme(std::make_shared<SequentialScheme>());
}

} // namespace qtl
