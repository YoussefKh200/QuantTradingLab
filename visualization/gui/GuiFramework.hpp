#pragma once
/**
 * @file visualization/gui/GuiFramework.hpp
 * @brief Dear ImGui-based GUI framework for quantitative trading dashboard.
 *
 * Features:
 *  - Window management and layout
 *  - Theme customization
 *  - Real-time rendering loop
 *  - Event handling
 */

#include <memory>
#include <functional>
#include <vector>
#include <string>
#include <map>

namespace qtl {

// Forward declarations to avoid ImGui dependency in header
struct ImGuiContext;
struct GLFWwindow;

class GuiFramework {
public:
    struct Config {
        int width{1920};
        int height{1080};
        std::string title{"QuantTradingLab Dashboard"};
        bool vsync{true};
        bool darkMode{true};
    };

    explicit GuiFramework(const Config& config = Config{});
    ~GuiFramework();

    // Disable copying
    GuiFramework(const GuiFramework&) = delete;
    GuiFramework& operator=(const GuiFramework&) = delete;

    /**
     * @brief Initialize the GUI framework
     */
    [[nodiscard]] bool initialize();

    /**
     * @brief Run the main render loop
     * @param renderCallback Function called each frame to render content
     */
    void run(std::function<void()> renderCallback);

    /**
     * @brief Stop the render loop
     */
    void stop();

    /**
     * @brief Check if the window should close
     */
    [[nodiscard]] bool shouldClose() const;

    /**
     * @brief Set custom render callback
     */
    void setRenderCallback(std::function<void()> callback);

    /**
     * @brief Apply dark theme
     */
    void applyDarkTheme();

    /**
     * @brief Apply light theme
     */
    void applyLightTheme();

    /**
     * @brief Get window width
     */
    [[nodiscard]] int getWidth() const { return config_.width; }

    /**
     * @brief Get window height
     */
    [[nodiscard]] int getHeight() const { return config_.height; }

private:
    Config config_;
    GLFWwindow* window_{nullptr};
    ImGuiContext* imguiContext_{nullptr};
    std::function<void()> renderCallback_;
    bool running_{false};
};

} // namespace qtl
