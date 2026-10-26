#include "experiment_internal.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

using JsonObject = JsonValue::Object;

namespace {

JsonValue parameters_to_json(const ParameterMap& parameters) {
    JsonObject object;

    for (const auto& [name, value] : parameters) {
        object.emplace(name, value);
    }

    return JsonValue(std::move(object));
}

JsonValue metrics_to_json(const BacktestMetrics& metrics) {
    JsonObject object;
    object.emplace("total_return", metrics.total_return);
    object.emplace("sharpe_ratio", metrics.sharpe_ratio);
    object.emplace("max_drawdown", metrics.max_drawdown);
    object.emplace(
        "number_of_trades",
        static_cast<double>(metrics.number_of_trades)
    );
    object.emplace("total_costs", metrics.total_costs);

    return JsonValue(std::move(object));
}

JsonValue trade_to_json(const Trade& trade) {
    JsonObject object;
    object.emplace("symbol", trade.symbol);
    object.emplace(
        "side",
        trade.side == OrderSide::Buy ? "buy" : "sell"
    );
    object.emplace("quantity", trade.quantity);
    object.emplace("fill_price", trade.fill_price);
    object.emplace("commission", trade.commission);
    object.emplace("slippage_cost", trade.slippage_cost);

    return JsonValue(std::move(object));
}

JsonValue performance_to_json(const PerformanceStats& stats) {
    JsonObject object;
    object.emplace(
        "max_concurrent",
        static_cast<double>(stats.max_concurrent)
    );
    object.emplace("throughput", stats.throughput);
    object.emplace("mean_latency", stats.mean_latency);
    object.emplace("median_latency", stats.median_latency);
    object.emplace("min_latency", stats.min_latency);
    object.emplace("max_latency", stats.max_latency);
    object.emplace("p95_latency", stats.p95_latency);
    object.emplace("total_work_seconds", stats.total_work_seconds);
    object.emplace("speedup", stats.speedup);
    object.emplace("cpu_seconds", stats.cpu_seconds);
    object.emplace("cpu_utilization", stats.cpu_utilization);
    object.emplace("cpu_efficiency", stats.cpu_efficiency);

    return JsonValue(std::move(object));
}

}  // namespace

namespace experiment_detail {

JsonValue error_document(
    const std::string& status,
    const std::string& type,
    const std::string& path,
    const std::string& message
) {
    JsonObject root;
    root.emplace("schema_version", 1);
    root.emplace("status", status);

    JsonObject error;
    error.emplace("type", type);
    if (!path.empty()) {
        error.emplace("path", path);
    }
    error.emplace("message", message);
    root.emplace("error", JsonValue(std::move(error)));

    return JsonValue(std::move(root));
}

}  // namespace experiment_detail

std::string experiment_result_to_json(
    const ExperimentResult& result,
    bool pretty
) {
    JsonObject root;
    root.emplace("schema_version", result.schema_version);
    root.emplace("status", result.status);
    root.emplace("experiment_id", result.experiment_id);

    JsonObject data;
    data.emplace("path", result.data.path);
    data.emplace("asset", result.data.asset);
    data.emplace("start_date", result.data.start_date);
    data.emplace("end_date", result.data.end_date);
    data.emplace(
        "trading_days",
        static_cast<double>(result.data_summary.trading_days)
    );
    data.emplace("first_date", result.data_summary.first_date);
    data.emplace("last_date", result.data_summary.last_date);
    root.emplace("data", JsonValue(std::move(data)));

    JsonObject execution;
    execution.emplace("initial_cash", result.execution.initial_cash);
    execution.emplace(
        "transaction_cost_rate",
        result.execution.transaction_cost_rate
    );
    execution.emplace("slippage_rate", result.execution.slippage_rate);
    execution.emplace(
        "max_concurrent_jobs",
        static_cast<double>(result.max_concurrent_jobs)
    );
    root.emplace("execution", JsonValue(std::move(execution)));

    JsonObject summary;
    summary.emplace(
        "jobs_total",
        static_cast<double>(result.summary.jobs_total)
    );
    summary.emplace(
        "jobs_completed",
        static_cast<double>(result.summary.jobs_completed)
    );
    summary.emplace(
        "jobs_failed",
        static_cast<double>(result.summary.jobs_failed)
    );
    summary.emplace("wall_seconds", result.summary.wall_seconds);
    summary.emplace("best_job_id", result.summary.best_job_id);
    summary.emplace(
        "best_sharpe_ratio",
        result.summary.best_sharpe_ratio
    );
    summary.emplace(
        "best_total_return",
        result.summary.best_total_return
    );
    summary.emplace(
        "performance",
        performance_to_json(result.summary.performance)
    );
    root.emplace("summary", JsonValue(std::move(summary)));

    JsonValue::Array results;
    results.reserve(result.results.size());

    for (const auto& outcome : result.results) {
        JsonObject entry;
        entry.emplace("job_id", outcome.job.job_id);
        entry.emplace("strategy_id", outcome.job.strategy_id);
        entry.emplace("strategy_type", outcome.job.strategy_type);
        entry.emplace("status", outcome.ok ? "completed" : "failed");
        entry.emplace("duration_seconds", outcome.duration_seconds);
        entry.emplace(
            "parameters",
            parameters_to_json(outcome.job.parameters)
        );

        if (outcome.ok) {
            entry.emplace(
                "metrics",
                metrics_to_json(outcome.result.metrics)
            );

            // Attach the full fill history only when the request opts in;
            // a large sweep can otherwise produce a very large response.
            if (result.output.include_trades) {
                JsonValue::Array trades;
                trades.reserve(outcome.result.trades.size());

                for (const auto& trade : outcome.result.trades) {
                    trades.push_back(trade_to_json(trade));
                }

                entry.emplace("trades", JsonValue(std::move(trades)));
            }
        } else {
            entry.emplace("error", outcome.error);
        }

        results.push_back(JsonValue(std::move(entry)));
    }

    root.emplace("results", JsonValue(std::move(results)));

    return serialize_json(JsonValue(std::move(root)), pretty);
}
