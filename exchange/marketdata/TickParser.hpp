#pragma once
/**
 * @file exchange/marketdata/TickParser.hpp
 * @brief CSV tick parser — converts text lines into typed tick structs.
 *
 * Supported CSV layouts
 * ─────────────────────
 * TradeTick CSV:
 *   timestamp_ns,symbol,price,quantity,side,trade_id,exchange
 *   1700000000000000000,AAPL,182.50,100,B,9001,NASDAQ
 *
 * QuoteTick CSV:
 *   timestamp_ns,symbol,bid_px,bid_sz,ask_px,ask_sz,exchange
 *   1700000000000000000,AAPL,182.49,200,182.51,150,NASDAQ
 *
 * OrderBookUpdate CSV:
 *   timestamp_ns,symbol,action,side,price,quantity,order_id,level
 *   1700000000000000000,AAPL,A,B,182.49,100,42001,2
 *
 * Notes
 * ─────
 * • Header row is auto-detected and skipped.
 * • Lines beginning with '#' are comments, skipped.
 * • Empty lines skipped.
 * • Malformed lines increment parseErrors_ counter and are skipped.
 * • Side: B/BUY/Buy → Buy,  S/SELL/Sell → Sell.
 * • Action: A/ADD → Add,  C/CANCEL → Cancel,  M/MODIFY → Modify,
 *           T/TRADE → Trade,  X/CLEAR → Clear.
 */

#include "exchange/marketdata/Tick.hpp"
#include <string>
#include <vector>
#include <sstream>
#include <charconv>
#include <stdexcept>
#include <cstdint>

namespace qtl {

class TickParser {
public:
    // ── Parse entry points ────────────────────────────────────

    /**
     * @brief Parse one CSV line as a TradeTick.
     * @throws std::runtime_error on malformed input.
     */
    [[nodiscard]] static TradeTick parseTrade(const std::string& line) {
        auto cols = split(line, ',');
        if (cols.size() < 5)
            throw std::runtime_error("TradeTick: too few columns: " + line);

        TradeTick t;
        t.header.timestamp = parseInt64(cols[0]);
        t.header.recvTime  = t.header.timestamp; // set by feed on receipt
        t.header.symbol    = trim(cols[1]);
        t.price            = parseDouble(cols[2]);
        t.quantity         = parseInt64(cols[3]);
        t.aggressorSide    = parseSide(cols[4]);
        if (cols.size() > 5) t.tradeId   = static_cast<uint64_t>(parseInt64(cols[5]));
        if (cols.size() > 6) t.header.exchange = trim(cols[6]);
        return t;
    }

    /**
     * @brief Parse one CSV line as a QuoteTick.
     * @throws std::runtime_error on malformed input.
     */
    [[nodiscard]] static QuoteTick parseQuote(const std::string& line) {
        auto cols = split(line, ',');
        if (cols.size() < 6)
            throw std::runtime_error("QuoteTick: too few columns: " + line);

        QuoteTick q;
        q.header.timestamp = parseInt64(cols[0]);
        q.header.recvTime  = q.header.timestamp;
        q.header.symbol    = trim(cols[1]);
        q.bidPrice         = parseDouble(cols[2]);
        q.bidSize          = parseInt64(cols[3]);
        q.askPrice         = parseDouble(cols[4]);
        q.askSize          = parseInt64(cols[5]);
        if (cols.size() > 6) q.header.exchange = trim(cols[6]);
        return q;
    }

    /**
     * @brief Parse one CSV line as an OrderBookUpdate.
     * @throws std::runtime_error on malformed input.
     */
    [[nodiscard]] static OrderBookUpdate parseBookUpdate(const std::string& line) {
        auto cols = split(line, ',');
        if (cols.size() < 6)
            throw std::runtime_error("BookUpdate: too few columns: " + line);

        OrderBookUpdate u;
        u.header.timestamp = parseInt64(cols[0]);
        u.header.recvTime  = u.header.timestamp;
        u.header.symbol    = trim(cols[1]);
        u.action           = parseAction(cols[2]);
        u.side             = parseSide(cols[3]);
        u.price            = parseDouble(cols[4]);
        u.quantity         = parseInt64(cols[5]);
        if (cols.size() > 6) u.orderId = static_cast<uint64_t>(parseInt64(cols[6]));
        if (cols.size() > 7) u.level   = static_cast<int>(parseInt64(cols[7]));
        return u;
    }

    /**
     * @brief Auto-detect tick type from a header row and parse body lines.
     *
     * Detection logic:
     *   header contains "bid" or "ask"                → QuoteTick
     *   header contains "action" or "order_id"        → BookUpdate
     *   header contains "trade_id" or "side" + "price"→ TradeTick
     *   default                                        → QuoteTick
     */
    [[nodiscard]] static TickType detectType(const std::string& headerLine) {
        std::string h = toLower(headerLine);
        if (h.find("action") != std::string::npos ||
            h.find("order_id") != std::string::npos) return TickType::BookUpdate;
        if (h.find("bid") != std::string::npos ||
            h.find("ask") != std::string::npos)    return TickType::Quote;
        if (h.find("trade_id") != std::string::npos) return TickType::Trade;
        return TickType::Quote;
    }

    /**
     * @brief Parse a complete line given a known TickType.
     * Returns an AnyTick, or throws on malformed input.
     */
    [[nodiscard]] static AnyTick parseLine(const std::string& line, TickType type) {
        switch (type) {
            case TickType::Trade:
                return AnyTick::makeTrade(parseTrade(line));
            case TickType::Quote:
                return AnyTick::makeQuote(parseQuote(line));
            case TickType::BookUpdate:
                return AnyTick::makeBook(parseBookUpdate(line));
        }
        return AnyTick::makeQuote(parseQuote(line));
    }

    [[nodiscard]] static bool isHeaderOrComment(const std::string& line) {
        if (line.empty()) return true;
        if (line[0] == '#') return true;
        // If first token cannot be parsed as a number it's a header
        std::string first = split(line, ',')[0];
        first = trim(first);
        if (first.empty()) return true;
        // Try to parse as int64 — if it fails it's a header
        int64_t v = 0;
        auto [ptr, ec] = std::from_chars(first.data(),
                                          first.data() + first.size(), v);
        return ec != std::errc{};
    }

    // ── Helpers ───────────────────────────────────────────────

    [[nodiscard]] static Side parseSide(const std::string& s) {
        std::string t = toUpper(trim(s));
        if (t == "B" || t == "BUY"  || t == "1") return Side::Buy;
        if (t == "S" || t == "SELL" || t == "0") return Side::Sell;
        throw std::runtime_error("Unknown side: " + s);
    }

    [[nodiscard]] static BookAction parseAction(const std::string& s) {
        std::string t = toUpper(trim(s));
        if (t == "A" || t == "ADD")    return BookAction::Add;
        if (t == "C" || t == "CANCEL") return BookAction::Cancel;
        if (t == "M" || t == "MODIFY") return BookAction::Modify;
        if (t == "T" || t == "TRADE")  return BookAction::Trade;
        if (t == "X" || t == "CLEAR")  return BookAction::Clear;
        throw std::runtime_error("Unknown book action: " + s);
    }

    [[nodiscard]] static double parseDouble(const std::string& s) {
        try { return std::stod(s); }
        catch (...) { throw std::runtime_error("Bad double: " + s); }
    }

    [[nodiscard]] static int64_t parseInt64(const std::string& s) {
        std::string t = trim(s);
        int64_t v = 0;
        auto [ptr, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
        if (ec != std::errc{})
            throw std::runtime_error("Bad int64: " + s);
        return v;
    }

    [[nodiscard]] static std::vector<std::string> split(
            const std::string& s, char delim) {
        std::vector<std::string> out;
        std::stringstream ss{s};
        std::string tok;
        while (std::getline(ss, tok, delim)) out.push_back(tok);
        return out;
    }

    [[nodiscard]] static std::string trim(std::string s) {
        size_t l = s.find_first_not_of(" \t\r\n\"");
        size_t r = s.find_last_not_of(" \t\r\n\"");
        if (l == std::string::npos) return "";
        return s.substr(l, r - l + 1);
    }

    [[nodiscard]] static std::string toUpper(std::string s) {
        for (auto& c : s) c = static_cast<char>(std::toupper(
                static_cast<unsigned char>(c)));
        return s;
    }

    [[nodiscard]] static std::string toLower(std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(
                static_cast<unsigned char>(c)));
        return s;
    }
};

} // namespace qtl
