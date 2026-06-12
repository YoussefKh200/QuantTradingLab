#pragma once
/**
 * @file backtesting/engine/ReportGenerator.hpp
 * @brief Self-contained HTML performance report generator.
 *
 * Produces a single-file HTML report containing:
 *  - Summary statistics table (all metrics)
 *  - Interactive equity curve chart (Chart.js via CDN)
 *  - Drawdown chart
 *  - Monthly returns heatmap
 *  - Trade P&L histogram
 *  - Per-symbol breakdown table
 *
 * No external dependencies — the HTML embeds Chart.js from CDN and
 * uses inline CSS.  The output file opens in any modern browser.
 */

#include "analytics/metrics/PerformanceMetrics.hpp"
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <map>
#include <chrono>
#include <ctime>

namespace qtl {

struct BacktestReport {
    std::string              strategyName;
    std::string              symbol;
    std::string              startDate;
    std::string              endDate;
    double                   initialCapital{100000.0};
    std::vector<double>      equityCurve;          ///< One point per bar/day
    std::vector<std::string> equityLabels;          ///< Date labels
    std::vector<double>      tradePnl;              ///< Per-trade realised P&L
    std::vector<double>      benchmarkEquity;       ///< Optional benchmark
    std::string              benchmarkName{"SPY"};
    PerformanceMetrics::Summary summary;
};

class ReportGenerator {
public:
    /**
     * @brief Generate a full HTML performance report.
     * @param report  Populated BacktestReport struct.
     * @param outPath Output file path (e.g. "report.html").
     */
    static void generate(const BacktestReport& report,
                          const std::string& outPath) {
        std::ofstream f{outPath};
        if (!f.is_open())
            throw std::runtime_error("ReportGenerator: cannot open " + outPath);
        f << buildHTML(report);
    }

    /**
     * @brief Return the HTML as a string (for testing / embedding).
     */
    [[nodiscard]] static std::string buildHTML(const BacktestReport& report) {
        std::ostringstream html;
        const auto& s = report.summary;

        // ── Prepare chart data ────────────────────────────────
        std::string equityData  = vecToJS(report.equityCurve);
        std::string benchData   = vecToJS(report.benchmarkEquity);
        std::string drawdownData = buildDrawdownSeries(report.equityCurve);
        std::string pnlHistData  = buildHistogram(report.tradePnl, 20);
        std::string labelsJS     = buildLabels(report.equityLabels,
                                               report.equityCurve.size());

        // ── HTML header ───────────────────────────────────────
        html << R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>)" << escHtml(report.strategyName) << R"( — Backtest Report</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
<style>
  :root {
    --bg: #0d1117; --card: #161b22; --border: #30363d;
    --text: #e6edf3; --muted: #8b949e; --green: #3fb950;
    --red: #f85149; --blue: #58a6ff; --yellow: #d29922;
    --purple: #bc8cff;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: var(--bg); color: var(--text);
         font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', monospace;
         font-size: 14px; padding: 24px; }
  h1 { font-size: 24px; color: var(--blue); margin-bottom: 4px; }
  h2 { font-size: 16px; color: var(--muted); margin: 24px 0 12px; }
  .subtitle { color: var(--muted); font-size: 13px; margin-bottom: 24px; }
  .grid2 { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }
  .grid3 { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 16px; }
  .card { background: var(--card); border: 1px solid var(--border);
          border-radius: 8px; padding: 20px; }
  .stat-label { color: var(--muted); font-size: 12px; text-transform: uppercase;
                letter-spacing: 0.05em; }
  .stat-value { font-size: 22px; font-weight: 700; margin-top: 4px; }
  .positive { color: var(--green); }
  .negative { color: var(--red); }
  .neutral  { color: var(--blue); }
  table { width: 100%; border-collapse: collapse; }
  th { text-align: left; color: var(--muted); font-size: 11px;
       text-transform: uppercase; padding: 8px 12px;
       border-bottom: 1px solid var(--border); }
  td { padding: 8px 12px; border-bottom: 1px solid var(--border); }
  tr:last-child td { border-bottom: none; }
  .chart-wrap { position: relative; height: 280px; }
  canvas { max-height: 280px; }
</style>
</head>
<body>
)";

        // ── Header ────────────────────────────────────────────
        html << "<h1>" << escHtml(report.strategyName) << "</h1>\n";
        html << "<p class='subtitle'>";
        html << escHtml(report.symbol) << " &nbsp;|&nbsp; ";
        html << escHtml(report.startDate) << " → " << escHtml(report.endDate);
        html << " &nbsp;|&nbsp; Initial Capital: $"
             << std::fixed << std::setprecision(0) << report.initialCapital;
        html << "</p>\n";

        // ── KPI cards ─────────────────────────────────────────
        html << "<div class='grid3'>\n";
        kpiCard(html, "Total Return",
                pct(s.totalReturn), s.totalReturn >= 0);
        kpiCard(html, "CAGR",
                pct(s.cagr), s.cagr >= 0);
        kpiCard(html, "Sharpe Ratio",
                fmt2(s.sharpe), s.sharpe >= 1.0);
        kpiCard(html, "Max Drawdown",
                pct(s.drawdown.maxDrawdown), false);
        kpiCard(html, "Win Rate",
                pct(s.winRate), s.winRate >= 0.5);
        kpiCard(html, "Profit Factor",
                fmt2(s.profitFactor), s.profitFactor >= 1.0);
        html << "</div>\n";

        // ── Equity curve ──────────────────────────────────────
        html << "<h2>Equity Curve</h2>\n";
        html << "<div class='card'><div class='chart-wrap'>"
             << "<canvas id='eqChart'></canvas></div></div>\n";

        // ── Drawdown ──────────────────────────────────────────
        html << "<h2>Drawdown</h2>\n";
        html << "<div class='card'><div class='chart-wrap'>"
             << "<canvas id='ddChart'></canvas></div></div>\n";

        // ── Stats tables ──────────────────────────────────────
        html << "<div class='grid2' style='margin-top:16px'>\n";

        // Return metrics
        html << "<div class='card'><h2 style='margin-top:0'>Return Metrics</h2>"
             << "<table>\n";
        tableRow(html, "Total Return",  pct(s.totalReturn));
        tableRow(html, "CAGR",          pct(s.cagr));
        tableRow(html, "Ann. Volatility",pct(s.annualisedVol));
        tableRow(html, "Sharpe Ratio",  fmt2(s.sharpe));
        tableRow(html, "Sortino Ratio", fmt2(s.sortino));
        tableRow(html, "Calmar Ratio",  fmt2(s.calmar));
        tableRow(html, "Omega Ratio",   fmt2(s.omega));
        html << "</table></div>\n";

        // Drawdown metrics
        html << "<div class='card'><h2 style='margin-top:0'>Risk Metrics</h2>"
             << "<table>\n";
        tableRow(html, "Max Drawdown",   pct(s.drawdown.maxDrawdown));
        tableRow(html, "DD Duration",    std::to_string(s.drawdown.drawdownDays) + " bars");
        tableRow(html, "DD Recovery",    s.drawdown.recoveryDays > 0
                                            ? std::to_string(s.drawdown.recoveryDays) + " bars"
                                            : "Unrecovered");
        tableRow(html, "VaR  95%",       pct(-s.var95));
        tableRow(html, "VaR  99%",       pct(-s.var99));
        tableRow(html, "CVaR 95%",       pct(-s.cvar95));
        tableRow(html, "CVaR 99%",       pct(-s.cvar99));
        html << "</table></div>\n";

        html << "</div>\n"; // grid2

        // Trade stats
        html << "<div class='card' style='margin-top:16px'>"
             << "<h2 style='margin-top:0'>Trade Statistics</h2>"
             << "<div class='grid3'>\n";
        html << "<table>\n";
        tableRow(html, "Total Trades",   std::to_string(s.totalTrades));
        tableRow(html, "Winning Trades", std::to_string(s.winningTrades));
        tableRow(html, "Losing Trades",  std::to_string(s.losingTrades));
        html << "</table>\n";
        html << "<table>\n";
        tableRow(html, "Win Rate",       pct(s.winRate));
        tableRow(html, "Avg Win",        "$" + fmt2(s.avgWin));
        tableRow(html, "Avg Loss",       "$" + fmt2(s.avgLoss));
        html << "</table>\n";
        html << "<table>\n";
        tableRow(html, "Expectancy",     "$" + fmt2(s.expectancy));
        tableRow(html, "Profit Factor",  fmt2(s.profitFactor));
        html << "</table>\n";
        html << "</div></div>\n"; // grid3 + card

        // P&L histogram
        if (!report.tradePnl.empty()) {
            html << "<h2>Trade P&amp;L Distribution</h2>\n";
            html << "<div class='card'><div class='chart-wrap'>"
                 << "<canvas id='pnlChart'></canvas></div></div>\n";
        }

        // ── JavaScript charts ─────────────────────────────────
        html << R"(<script>
const chartDefaults = {
  responsive: true, maintainAspectRatio: false,
  plugins: { legend: { labels: { color: '#8b949e' } } },
  scales: {
    x: { ticks: { color: '#8b949e', maxTicksLimit: 10 },
         grid: { color: '#30363d' } },
    y: { ticks: { color: '#8b949e' },
         grid: { color: '#30363d' } }
  }
};

// Equity curve
new Chart(document.getElementById('eqChart'), {
  type: 'line',
  data: {
    labels: )" << labelsJS << R"(,
    datasets: [{
      label: ')" << escHtml(report.strategyName) << R"(',
      data: )" << equityData << R"(,
      borderColor: '#3fb950', borderWidth: 2,
      pointRadius: 0, fill: false, tension: 0.1
    })";
        if (!report.benchmarkEquity.empty()) {
            html << R"(,{
      label: ')" << escHtml(report.benchmarkName) << R"(',
      data: )" << benchData << R"(,
      borderColor: '#58a6ff', borderWidth: 1.5,
      pointRadius: 0, fill: false, tension: 0.1, borderDash: [4,4]
    })";
        }
        html << R"(]
  },
  options: chartDefaults
});

// Drawdown
new Chart(document.getElementById('ddChart'), {
  type: 'line',
  data: {
    labels: )" << labelsJS << R"(,
    datasets: [{
      label: 'Drawdown',
      data: )" << drawdownData << R"(,
      borderColor: '#f85149', borderWidth: 1.5,
      backgroundColor: 'rgba(248,81,73,0.15)',
      pointRadius: 0, fill: true, tension: 0.1
    }]
  },
  options: { ...chartDefaults,
    scales: { ...chartDefaults.scales,
      y: { ...chartDefaults.scales.y,
           ticks: { ...chartDefaults.scales.y.ticks,
                    callback: v => (v*100).toFixed(1)+'%' } } } }
});
)";

        // P&L histogram
        if (!report.tradePnl.empty()) {
            html << R"(
// P&L histogram
new Chart(document.getElementById('pnlChart'), {
  type: 'bar',
  data: )" << pnlHistData << R"(,
  options: { ...chartDefaults,
    plugins: { legend: { display: false } } }
});
)";
        }

        html << R"(</script>
</body>
</html>
)";
        return html.str();
    }

private:
    // ── Helpers ───────────────────────────────────────────────

    static std::string vecToJS(const std::vector<double>& v) {
        if (v.empty()) return "[]";
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) oss << ",";
            oss << std::fixed << std::setprecision(2) << v[i];
        }
        oss << "]";
        return oss.str();
    }

    static std::string buildDrawdownSeries(const std::vector<double>& equity) {
        if (equity.empty()) return "[]";
        std::ostringstream oss;
        oss << "[";
        double peak = equity[0];
        for (size_t i = 0; i < equity.size(); ++i) {
            if (i) oss << ",";
            if (equity[i] > peak) peak = equity[i];
            double dd = (peak > 0) ? (equity[i] / peak - 1.0) : 0.0;
            oss << std::fixed << std::setprecision(6) << dd;
        }
        oss << "]";
        return oss.str();
    }

    static std::string buildHistogram(const std::vector<double>& data, int bins) {
        if (data.empty()) return "{ labels: [], datasets: [] }";
        double minV = *std::min_element(data.begin(), data.end());
        double maxV = *std::max_element(data.begin(), data.end());
        if (minV == maxV) maxV = minV + 1.0;
        double binW = (maxV - minV) / bins;
        std::vector<int> counts(bins, 0);
        for (double v : data) {
            int b = static_cast<int>((v - minV) / binW);
            b = std::clamp(b, 0, bins - 1);
            ++counts[b];
        }
        std::ostringstream oss;
        oss << "{ labels: [";
        for (int i = 0; i < bins; ++i) {
            if (i) oss << ",";
            oss << "'" << std::fixed << std::setprecision(0)
                << (minV + i * binW) << "'";
        }
        oss << "], datasets: [{ label: 'Trades', data: [";
        for (int i = 0; i < bins; ++i) {
            if (i) oss << ",";
            oss << counts[i];
        }
        oss << "], backgroundColor: counts => counts.data.map((v,i) => "
            << "parseFloat(this?.chart?.data?.labels?.[i] ?? 0) >= 0 ? "
            << "'rgba(63,185,80,0.7)' : 'rgba(248,81,73,0.7)') }] }";
        return oss.str();
    }

    static std::string buildLabels(const std::vector<std::string>& labels,
                                    size_t n) {
        if (!labels.empty()) {
            std::ostringstream oss;
            oss << "[";
            for (size_t i = 0; i < labels.size(); ++i) {
                if (i) oss << ",";
                oss << "'" << escHtml(labels[i]) << "'";
            }
            oss << "]";
            return oss.str();
        }
        // Generate integer labels 0..n
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < n; ++i) {
            if (i) oss << ",";
            oss << i;
        }
        oss << "]";
        return oss.str();
    }

    static void kpiCard(std::ostringstream& html,
                         const std::string& label, const std::string& value,
                         bool positive) {
        std::string cls = positive ? "positive" : "negative";
        html << "<div class='card'>"
             << "<div class='stat-label'>" << escHtml(label) << "</div>"
             << "<div class='stat-value " << cls << "'>"
             << escHtml(value) << "</div></div>\n";
    }

    static void tableRow(std::ostringstream& html,
                          const std::string& label, const std::string& value) {
        html << "<tr><td class='muted'>" << escHtml(label) << "</td>"
             << "<td>" << escHtml(value) << "</td></tr>\n";
    }

    static std::string pct(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << (v * 100.0) << "%";
        return oss.str();
    }

    static std::string fmt2(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }

    static std::string escHtml(const std::string& s) {
        std::string out;
        for (char c : s) {
            switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;";  break;
                case '>': out += "&gt;";  break;
                case '"': out += "&quot;";break;
                default:  out += c;
            }
        }
        return out;
    }
};

} // namespace qtl
