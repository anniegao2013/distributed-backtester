#include "experiment.h"
#include "performance_stats.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* kUsage =
    "Usage: backtester [options]\n"
    "\n"
    "Runs the built-in three-strategy demo when no experiment is supplied.\n"
    "\n"
    "Options:\n"
    "  -e, --experiment PATH  Run the experiment described by a JSON file.\n"
    "                         Use '-' to read the JSON request from stdin.\n"
    "      --json             Emit the response as JSON instead of text.\n"
    "  -h, --help             Show this help.\n"
    "\n"
    "Examples:\n"
    "  backtester --experiment experiments/crossover_sweep.json\n"
    "  backtester --experiment experiments/demo.json --json\n"
    "  cat request.json | backtester --experiment - --json\n";

// Builds the demo experiment programmatically. The demo is expressed with
// the same request model that JSON documents produce, so it exercises the
// exact code path used by file- and service-driven runs.
ExperimentRequest build_demo_experiment() {
    ExperimentRequest request;
    request.experiment_id = "demo-three-strategy-sweep";
    request.description =
        "Moving-average crossover, RSI mean reversion, and Donchian "
        "channel breakout swept over SP500 2000-2010.";
    request.data = {
        "data/market/sp500_1950_2018.csv",
        "SP500",
        "2000-01-01",
        "2010-12-31"
    };
    request.execution = {
        10000.0,  // initial cash
        0.001,    // 10 bps commission on traded notional
        0.0005,   // 5 bps adverse slippage on each fill
        0         // 0 = use hardware concurrency
    };

    ExperimentStrategy crossover;
    crossover.id = "crossover";
    crossover.type = "moving_average_crossover";
    crossover.grid = {
        {"short_window",
         {10.0, 15.0, 20.0, 25.0, 30.0, 35.0, 40.0, 45.0, 50.0,
          55.0, 60.0, 65.0, 70.0, 75.0, 80.0, 85.0, 90.0, 95.0}},
        {"long_window",
         {100.0, 110.0, 120.0, 130.0, 140.0, 150.0, 160.0, 170.0,
          180.0, 190.0, 200.0, 210.0, 220.0, 230.0, 240.0, 250.0,
          260.0, 270.0, 280.0, 290.0, 300.0}}
    };

    ExperimentStrategy rsi;
    rsi.id = "rsi";
    rsi.type = "rsi_mean_reversion";
    rsi.grid = {
        {"rsi_period", {5.0, 10.0, 14.0, 20.0}},
        {"oversold_threshold", {20.0, 30.0}},
        {"exit_threshold", {60.0, 70.0}}
    };

    ExperimentStrategy donchian;
    donchian.id = "donchian";
    donchian.type = "donchian_channel_breakout";
    donchian.grid = {
        {"entry_period", {20.0, 40.0, 55.0}},
        {"exit_period", {5.0, 10.0, 15.0}}
    };

    request.strategies.push_back(std::move(crossover));
    request.strategies.push_back(std::move(rsi));
    request.strategies.push_back(std::move(donchian));

    finalize_experiment(request);
    return request;
}

void print_result(const JobResult& result) {
    std::cout << std::fixed << std::setprecision(4);

    std::cout << "Job ID: " << result.job_id << '\n';
    std::cout << "Asset: " << result.asset << '\n';

    std::cout << "Parameters:\n";
    for (const auto& [name, value] : result.parameters) {
        std::cout << "  " << name << ": " << value << '\n';
    }

    std::cout << "Metrics:\n";
    std::cout << "  Total return: "
              << result.metrics.total_return * 100.0 << "%\n";
    std::cout << "  Sharpe ratio: " << result.metrics.sharpe_ratio << '\n';
    std::cout << "  Max drawdown: "
              << result.metrics.max_drawdown * 100.0 << "%\n";
    std::cout << "  Number of trades: "
              << result.metrics.number_of_trades << " round trips\n";
    std::cout << "  Executed fills: " << result.trades.size() << '\n';
    std::cout << "  Total costs: $"
              << result.metrics.total_costs << '\n';
}

// Ranks successful jobs by Sharpe ratio (highest first) and prints a summary
// table. Ties receive the same rank; the next rank skips tied positions.
void print_ranked_results(
    const std::vector<ExperimentJobOutcome>& outcomes
) {
    std::vector<const ExperimentJobOutcome*> ranked;
    ranked.reserve(outcomes.size());

    for (const auto& outcome : outcomes) {
        if (outcome.ok) {
            ranked.push_back(&outcome);
        }
    }

    if (ranked.empty()) {
        return;
    }

    std::sort(
        ranked.begin(),
        ranked.end(),
        [](const ExperimentJobOutcome* a, const ExperimentJobOutcome* b) {
            const BacktestMetrics& left = a->result.metrics;
            const BacktestMetrics& right = b->result.metrics;

            if (left.sharpe_ratio != right.sharpe_ratio) {
                return left.sharpe_ratio > right.sharpe_ratio;
            }
            return left.total_return > right.total_return;
        }
    );

    std::cout << "\n=== Ranked Results (by Sharpe ratio) ===\n";
    std::cout << std::left << std::setw(6) << "Rank"
              << std::setw(14) << "Job ID"
              << "Sharpe\tTotal Return\tMax Drawdown\tCosts\tRound Trips\tParameters\n";

    std::size_t rank = 0;
    for (std::size_t i = 0; i < ranked.size(); ++i) {
        const BacktestMetrics& metrics = ranked[i]->result.metrics;

        // Competition ranking ("1224"): ties share a rank.
        if (i == 0) {
            rank = 1;
        } else {
            const BacktestMetrics& previous =
                ranked[i - 1]->result.metrics;

            if (metrics.sharpe_ratio != previous.sharpe_ratio ||
                metrics.total_return != previous.total_return) {
                rank = i + 1;
            }
        }

        std::cout << std::left << std::setw(6) << rank
                  << std::setw(14) << ranked[i]->result.job_id
                  << std::fixed << std::setprecision(4)
                  << metrics.sharpe_ratio
                  << "\t"
                  << std::fixed << std::setprecision(4)
                  << metrics.total_return * 100.0 << "%"
                  << "\t"
                  << std::fixed << std::setprecision(4)
                  << metrics.max_drawdown * 100.0 << "%"
                  << "\t"
                  << std::fixed << std::setprecision(2)
                  << metrics.total_costs
                  << "\t"
                  << std::fixed << std::setprecision(0)
                  << metrics.number_of_trades
                  << "\t";

        bool first = true;
        for (const auto& [name, value] :
             ranked[i]->result.parameters) {
            if (!first) {
                std::cout << ", ";
            }
            std::cout << name << "=" << std::fixed << std::setprecision(0)
                      << value;
            first = false;
        }
        std::cout << '\n';
    }

    std::cout << "\nBest configuration: "
              << ranked.front()->result.job_id
              << " (Sharpe " << std::fixed << std::setprecision(4)
              << ranked.front()->result.metrics.sharpe_ratio
              << ", total return "
              << std::fixed << std::setprecision(4)
              << ranked.front()->result.metrics.total_return * 100.0
              << "%)\n";
}

void print_performance_stats(const PerformanceStats& stats) {
    if (stats.jobs_completed == 0) {
        std::cout << "\nNo jobs completed; skipping performance stats.\n";
        return;
    }

    std::cout << "\n=== Performance Stats ===\n";
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "Jobs completed:    " << stats.jobs_completed << " of "
              << stats.jobs_total << '\n';
    std::cout << "Max concurrent:    " << stats.max_concurrent << '\n';
    std::cout << "Wall time:         " << stats.wall_seconds << " s\n";
    std::cout << "Throughput:        " << stats.throughput << " jobs/s\n";
    std::cout << "Latency (mean):    " << stats.mean_latency << " s\n";
    std::cout << "Latency (median):  " << stats.median_latency << " s\n";
    std::cout << "Latency (min):     " << stats.min_latency << " s\n";
    std::cout << "Latency (max):     " << stats.max_latency << " s\n";
    std::cout << "Latency (p95):     " << stats.p95_latency << " s\n";
    std::cout << "Total work:        " << stats.total_work_seconds << " CPU-s\n";
    std::cout << "Speedup:           " << stats.speedup << "x\n";
    std::cout << "CPU time:          " << stats.cpu_seconds << " s\n";
    std::cout << "CPU utilization:   " << stats.cpu_utilization << "% of one core\n";
    std::cout << "CPU efficiency:    " << stats.cpu_efficiency << "%\n";
}

void print_experiment_header(
    const ExperimentDataSummary& summary,
    std::size_t jobs_total,
    std::size_t max_concurrent
) {
    std::cout << "Successfully loaded " << summary.trading_days
              << " trading days.\n";
    std::cout << "First date: " << summary.first_date << '\n';
    std::cout << "Last date:  " << summary.last_date << '\n';

    std::cout << "Running " << jobs_total
              << " parameter combinations concurrently across all "
              << "strategies (up to " << max_concurrent
              << " jobs at a time).\n";
}

void print_experiment_results(const ExperimentResult& result) {
    std::cout << "Experiment: " << result.experiment_id << '\n';

    std::size_t printed = 0;
    for (const auto& outcome : result.results) {
        if (!outcome.ok) {
            std::cerr << "Job " << outcome.job.job_id
                      << " failed: " << outcome.error << '\n';
            continue;
        }

        ++printed;
        std::cout << "\n--- Result " << printed << " ---\n";
        std::cout << "Strategy: " << outcome.job.strategy_type << '\n';
        print_result(outcome.result);
    }

    std::cout << "\nCompleted " << result.summary.jobs_completed << " of "
              << result.summary.jobs_total << " backtests in "
              << result.summary.wall_seconds << " seconds.\n";

    print_ranked_results(result.results);
    print_performance_stats(result.summary.performance);
}

std::string read_stream(std::istream& stream) {
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

std::string read_file(const std::string& path) {
    if (path == "-") {
        return read_stream(std::cin);
    }

    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error(
            "Could not open experiment file '" + path + "'"
        );
    }

    std::string contents = read_stream(stream);
    if (!stream.good() && !stream.eof()) {
        throw std::runtime_error(
            "Could not read experiment file '" + path + "'"
        );
    }

    return contents;
}

struct Options {
    bool json = false;
    bool help = false;
    bool has_experiment = false;
    std::string experiment_path;
};

Options parse_options(int argc, char* argv[]) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];

        if (argument == "--json") {
            options.json = true;
        } else if (argument == "-h" || argument == "--help") {
            options.help = true;
        } else if (argument == "-e" || argument == "--experiment") {
            if (i + 1 >= argc) {
                throw std::invalid_argument(
                    argument + " requires a file path (or '-')"
                );
            }

            options.has_experiment = true;
            options.experiment_path = argv[++i];
        } else {
            throw std::invalid_argument(
                "Unknown argument '" + argument + "'"
            );
        }
    }

    return options;
}

void run_demo(const Options& options) {
    const ExperimentRequest request = build_demo_experiment();

    if (options.json) {
        const ExperimentResult result = run_experiment(request);
        std::cout << experiment_result_to_json(result, true);
        return;
    }

    const ExperimentResult result = run_experiment(
        request,
        [](const ExperimentDataSummary& summary,
           std::size_t jobs_total,
           std::size_t max_concurrent) {
            print_experiment_header(summary, jobs_total, max_concurrent);
        }
    );
    print_experiment_results(result);
}

void run_experiment_document(
    const std::string& request_json,
    const Options& options
) {
    if (options.json) {
        std::cout << run_experiment_json(request_json, true);
        return;
    }

    const ExperimentRequest request =
        parse_experiment_json(request_json);
    const ExperimentResult result = run_experiment(
        request,
        [](const ExperimentDataSummary& summary,
           std::size_t jobs_total,
           std::size_t max_concurrent) {
            print_experiment_header(summary, jobs_total, max_concurrent);
        }
    );
    print_experiment_results(result);
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const Options options = parse_options(argc, argv);

        if (options.help) {
            std::cout << kUsage;
            return 0;
        }

        if (options.has_experiment) {
            const std::string request_json =
                read_file(options.experiment_path);
            run_experiment_document(request_json, options);
            return 0;
        }

        run_demo(options);
        return 0;
    } catch (const ExperimentValidationError& error) {
        std::cerr << "Invalid experiment";
        if (!error.path().empty()) {
            std::cerr << " at " << error.path();
        }
        std::cerr << ": " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
