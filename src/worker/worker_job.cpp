#include "worker/worker.h"

#include "core/market_data.h"
#include "experiment/experiment.h"

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* kParametersPath = "$.job.parameters";

}  // namespace

WorkerJobOutcome execute_backtest_job(const BacktestJob& job) {
    WorkerJobOutcome outcome;

    const auto start = std::chrono::steady_clock::now();

    try {
        // Worker-side semantic validation: the protocol layer accepts any
        // finite parameter map, so the registry decides whether this strategy
        // type exists and whether the parameter set is complete and in range.
        validate_strategy_parameters(
            job.strategy_type,
            job.parameters,
            kParametersPath
        );

        const auto strategy =
            create_strategy(job.strategy_type, job.parameters);

        const std::vector<MarketBar> dataset =
            load_market_data(job.data.path);

        if (dataset.empty()) {
            throw std::runtime_error(
                "Market data file '" + job.data.path + "' is empty"
            );
        }

        const BacktestConfig config{
            job.job_id,
            job.data.asset,
            job.data.start_date,
            job.data.end_date,
            job.execution.initial_cash,
            job.parameters,
            job.execution.transaction_cost_rate,
            job.execution.slippage_rate
        };

        outcome.result = run_backtest(dataset, *strategy, config);
        outcome.status = "completed";
    } catch (const ExperimentValidationError& error) {
        outcome.status = "invalid";
        outcome.error_type = "validation_error";
        outcome.error_path = error.path();
        outcome.error_message = error.what();
    } catch (const std::exception& error) {
        // Missing data, an unusable date range, or an engine failure. The job
        // was well formed, so this is a runtime failure rather than
        // "invalid".
        outcome.status = "failed";
        outcome.error_type = "runtime_error";
        outcome.error_message = error.what();
    }

    const auto end = std::chrono::steady_clock::now();
    outcome.duration_seconds =
        std::chrono::duration<double>(end - start).count();

    return outcome;
}

BacktestResultMessage make_result_message(
    const BacktestJobMessage& request,
    const std::string& worker_id,
    const WorkerJobOutcome& outcome
) {
    BacktestResultMessage message;
    message.protocol_version = request.protocol_version;
    message.message_id = request.message_id;
    message.worker_id = worker_id;

    BacktestResult& result = message.result;
    result.experiment_id = request.job.experiment_id;
    result.job_id = request.job.job_id;
    result.strategy_id = request.job.strategy_id;
    result.strategy_type = request.job.strategy_type;
    result.parameters = request.job.parameters;
    result.status = outcome.status;
    result.duration_seconds = outcome.duration_seconds;

    if (outcome.status == "completed") {
        result.metrics = outcome.result.metrics;

        // Only return trades when the job asked for them; large sweeps would
        // otherwise produce very large messages.
        if (request.job.output.include_trades) {
            result.trades = outcome.result.trades;
        }

        return message;
    }

    BacktestResultError error;
    error.type = outcome.error_type;
    error.path = outcome.error_path;
    error.message = outcome.error_message.empty()
        ? "Backtest " + outcome.status
        : outcome.error_message;

    result.error = std::move(error);
    return message;
}
