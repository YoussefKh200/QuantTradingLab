#pragma once
/**
 * @file core/config/Config.hpp
 * @brief Hierarchical key-value configuration store.
 *
 * Keys use dot-notation: "risk.maxDailyLoss", "strategy.mm.spread".
 * Values are stored as strings and converted on access.
 * Can be loaded from a simple INI-style file or populated programmatically.
 *
 * Thread-safety: all public methods are guarded by a shared_mutex
 * (multiple readers, exclusive writer).
 */

#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <optional>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <charconv>
#include <mutex>
#include <shared_mutex>

namespace qtl {

class Config {
public:
    static Config& instance() {
        static Config inst;
        return inst;
    }

    // ─── Write ─────────────────────────────────────────────────

    void set(const std::string& key, const std::string& value) {
        std::unique_lock lock{mutex_};
        store_[key] = value;
    }

    void set(const std::string& key, double value) {
        set(key, std::to_string(value));
    }

    void set(const std::string& key, int64_t value) {
        set(key, std::to_string(value));
    }

    void set(const std::string& key, bool value) {
        set(key, std::string{value ? "true" : "false"});
    }

    // ─── Read ──────────────────────────────────────────────────

    [[nodiscard]] std::optional<std::string> get(const std::string& key) const {
        std::shared_lock lock{mutex_};
        auto it = store_.find(key);
        if (it == store_.end()) return std::nullopt;
        return it->second;
    }

    [[nodiscard]] std::string getOrDefault(const std::string& key,
                                            const std::string& def) const {
        auto v = get(key);
        return v.value_or(def);
    }

    [[nodiscard]] double getDouble(const std::string& key, double def = 0.0) const {
        auto v = get(key);
        if (!v) return def;
        try { return std::stod(*v); }
        catch (...) { return def; }
    }

    [[nodiscard]] int64_t getInt(const std::string& key, int64_t def = 0) const {
        auto v = get(key);
        if (!v) return def;
        int64_t result = def;
        std::from_chars(v->data(), v->data() + v->size(), result);
        return result;
    }

    [[nodiscard]] bool getBool(const std::string& key, bool def = false) const {
        auto v = get(key);
        if (!v) return def;
        return (*v == "true" || *v == "1" || *v == "yes");
    }

    // ─── File I/O ──────────────────────────────────────────────

    /**
     * @brief Load a simple INI-style config file.
     *
     * Format:
     * @code
     *   # comment
     *   [section]
     *   key = value
     * @endcode
     * Keys become "section.key".
     */
    void loadFile(const std::string& path) {
        std::ifstream file{path};
        if (!file.is_open())
            throw std::runtime_error("Config: cannot open " + path);

        std::string line, section;
        while (std::getline(file, line)) {
            // Strip leading whitespace
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos) continue;
            line = line.substr(start);

            if (line.empty() || line[0] == '#' || line[0] == ';') continue;

            if (line[0] == '[') {
                auto end = line.find(']');
                section  = (end != std::string::npos)
                               ? line.substr(1, end - 1)
                               : "";
                continue;
            }

            auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));

            std::string fullKey = section.empty() ? key : section + "." + key;
            set(fullKey, val);
        }
    }

    /// Dump all keys to a human-readable string.
    [[nodiscard]] std::string dump() const {
        std::shared_lock lock{mutex_};
        std::ostringstream oss;
        for (auto& [k, v] : store_) {
            oss << k << " = " << v << '\n';
        }
        return oss.str();
    }

private:
    Config() = default;

    static std::string trim(std::string s) {
        size_t l = s.find_first_not_of(" \t\r\n");
        size_t r = s.find_last_not_of(" \t\r\n");
        if (l == std::string::npos) return "";
        return s.substr(l, r - l + 1);
    }

    mutable std::shared_mutex              mutex_;
    std::unordered_map<std::string,std::string> store_;
};

} // namespace qtl
