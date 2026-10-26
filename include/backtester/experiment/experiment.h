#pragma once

#include "trading/backtest_engine.h"
#include "core/parameter_grid.h"
#include "core/performance_stats.h"
#include "trading/strategy.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Raised when an experiment document is structurally valid JSON but does not
// satisfy the experiment schema. `path()` points at the offending field using
// a JSONPath-like expression such as
// "$.strategies[0].grid.short_window".
class ExperimentValidationError : public std::runtime_error {
public:
    ExperimentValidationError(std::string path, std::string message);

    const std::string& path() const noexcept;

private:
    std::string path_;
};

// Where the market data comes from and which slice to backtest.
struct ExperimentData {
    std::string path;
    std::string asset;
    std::string start_date;
    std::string end_date;
};

// Execution and scheduling settings shared by every job in an experiment.
struct ExperimentExecution {
    double initial_cash = 10000.0;
    double transaction_cost_rate = 0.0;
    double slippage_rate = 0.0;

    // 0 means "use the hardware concurrency", matching the demo runner.
    std::size_t max_concurrent_jobs = 0;
};

// A strategy declaration. Exactly one of `grid` or `parameter_sets` must be
// populated. A grid is expanded into the Cartesian product of its values;
// `parameter_sets` supplies explicit combinations.
struct ExperimentStrategy {
    std::string id;
    std::string type;
    ParameterGrid grid;
    std::vector<ParameterMap> parameter_sets;
};

struct ExperimentOutput {
    bool include_trades = false;
};

// One fully-expanded backtest: a strategy type, its parameters, and a stable
// job ID of the form "<strategy id>-<n>" (1-based within the strategy).
struct ExperimentJob {
    std::string job_id;
    std::string strategy_id;
    std::string strategy_type;
    ParameterMap parameters;
};

// The validated, expanded form of an experiment document.
struct ExperimentRequest {
    int schema_version = 1;
    std::string experiment_id;
    std::string description;
    ExperimentData data;
    ExperimentExecution execution;
    ExperimentOutput output;
    std::vector<ExperimentStrategy> strategies;

    // Filled by finalize_experiment().
    std::vector<ExperimentJob> jobs;
};

struct ExperimentDataSummary {
    std::size_t trading_days = 0;
    std::string first_date;
    std::string last_date;
};

// The outcome of one job. `result` is populated only when `ok` is true.
struct ExperimentJobOutcome {
    ExperimentJob job;
    bool ok = false;
    std::string error;
    double duration_seconds = 0.0;
    JobResult result;
};

struct ExperimentSummary {
    std::size_t jobs_total = 0;
    std::size_t jobs_completed = 0;
    std::size_t jobs_failed = 0;
    double wall_seconds = 0.0;
    std::string best_job_id;
    double best_sharpe_ratio = 0.0;
    double best_total_return = 0.0;
    PerformanceStats performance;
};

// Result document returned to a caller (and serialized by
// experiment_result_to_json). `status` is "completed", "partial", or
// "failed".
struct ExperimentResult {
    int schema_version = 1;
    std::string experiment_id;
    std::string status;
    ExperimentData data;
    ExperimentDataSummary data_summary;
    ExperimentExecution execution;
    ExperimentOutput output;
    std::size_t max_concurrent_jobs = 0;
    ExperimentSummary summary;
    std::vector<ExperimentJobOutcome> results;
};

// Parses and validates an experiment document. Does not read market data.
// Throws JsonParseError for malformed JSON and ExperimentValidationError for
// schema violations.
ExperimentRequest parse_experiment_json(const std::string& text);

// Reads a file and parses it as an experiment document.
ExperimentRequest load_experiment_file(const std::string& path);

// Validates the semantic rules of a request (dates, costs, strategy
// parameters, unique IDs) and expands strategies into `jobs`, interleaving
// them round-robin so the execution window contains a mix of strategies.
// Throws ExperimentValidationError.
void finalize_experiment(ExperimentRequest& request);

// Constructs a strategy from its schema type name and validated parameters.
// Throws std::invalid_argument for an unknown type.
std::unique_ptr<Strategy> create_strategy(
    const std::string& type,
    const ParameterMap& parameters
);

// Canonical strategy type names accepted by the schema, in display order.
std::vector<std::string> supported_strategy_types();

// Loads the market data, runs every job with bounded concurrency, and returns
// metrics plus timing. Throws if the data cannot be loaded or the request was
// not finalized. The optional callback fires once after the dataset is loaded
// and before the first job starts, which lets a CLI print progress without
// coupling the runner to stdout.
using ExperimentProgressCallback = std::function<void(
    const ExperimentDataSummary&,
    std::size_t jobs_total,
    std::size_t max_concurrent
)>;

ExperimentResult run_experiment(
    const ExperimentRequest& request,
    const ExperimentProgressCallback& on_started = {}
);

// Serializes a completed (or partially completed) experiment run.
std::string experiment_result_to_json(
    const ExperimentResult& result,
    bool pretty = true
);

// Service entry point: accepts an experiment request document and always
// returns a schema-shaped JSON response. Parse and validation failures produce
// status "invalid"; data or processing failures produce status "failed".
std::string run_experiment_json(
    const std::string& request_json,
    bool pretty = true
);
