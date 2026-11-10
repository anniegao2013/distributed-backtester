#include "coordinator/persistent_state.h"

#include "core/json_value.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

namespace fs = std::filesystem;

using JsonObject = JsonValue::Object;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

const JsonValue& require_object(
    const JsonValue& value,
    const std::string& context
) {
    if (!value.is_object()) {
        fail(context + " must be an object");
    }

    return value;
}

const JsonValue& require_member(
    const JsonValue& object,
    const std::string& key,
    const std::string& context
) {
    require_object(object, context);

    const JsonValue::Object& members = object.as_object();
    const auto found = members.find(key);

    if (found == members.end()) {
        fail(context + " is missing '" + key + "'");
    }

    return found->second;
}

std::string require_string(
    const JsonValue& value,
    const std::string& context
) {
    if (!value.is_string()) {
        fail(context + " must be a string");
    }

    return value.as_string();
}

double require_number(
    const JsonValue& value,
    const std::string& context
) {
    if (!value.is_number()) {
        fail(context + " must be a number");
    }

    return value.as_number();
}

bool require_boolean(
    const JsonValue& value,
    const std::string& context
) {
    if (!value.is_boolean()) {
        fail(context + " must be a boolean");
    }

    return value.as_boolean();
}

std::size_t require_count(
    const JsonValue& value,
    const std::string& context
) {
    const double number = require_number(value, context);

    if (number < 0.0 ||
        std::floor(number) != number ||
        number > static_cast<double>(
            std::numeric_limits<std::size_t>::max()
        )) {
        fail(context + " must be a non-negative integer");
    }

    return static_cast<std::size_t>(number);
}

const JsonValue::Array& require_array(
    const JsonValue& value,
    const std::string& context
) {
    if (!value.is_array()) {
        fail(context + " must be an array");
    }

    return value.as_array();
}

JsonValue parameters_to_json(const ParameterMap& parameters) {
    JsonObject object;

    for (const auto& [name, value] : parameters) {
        object.emplace(name, value);
    }

    return JsonValue(std::move(object));
}

ParameterMap parameters_from_json(
    const JsonValue& value,
    const std::string& context
) {
    require_object(value, context);

    ParameterMap parameters;

    for (const auto& [name, member] : value.as_object()) {
        parameters.emplace(
            name,
            require_number(member, context + "." + name)
        );
    }

    return parameters;
}

bool same_number(double left, double right) {
    if (left == right) {
        return true;
    }

    const double scale = std::max(
        {1.0, std::abs(left), std::abs(right)}
    );

    return std::abs(left - right) <= 1e-12 * scale;
}

bool same_parameters(
    const ParameterMap& left,
    const ParameterMap& right
) {
    if (left.size() != right.size()) {
        return false;
    }

    auto left_it = left.begin();
    auto right_it = right.begin();

    while (left_it != left.end()) {
        if (left_it->first != right_it->first ||
            !same_number(left_it->second, right_it->second)) {
            return false;
        }

        ++left_it;
        ++right_it;
    }

    return true;
}

JobState parse_job_state(
    const std::string& text,
    const std::string& context
) {
    if (text == "pending") {
        return JobState::pending;
    }
    if (text == "running") {
        return JobState::running;
    }
    if (text == "completed") {
        return JobState::completed;
    }
    if (text == "failed") {
        return JobState::failed;
    }

    fail(
        context + " has unsupported value '" + text +
        "' (expected pending, running, completed, or failed)"
    );
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

BacktestMetrics metrics_from_json(
    const JsonValue& value,
    const std::string& context
) {
    require_object(value, context);

    BacktestMetrics metrics;
    metrics.total_return = require_number(
        require_member(value, "total_return", context),
        context + ".total_return"
    );
    metrics.sharpe_ratio = require_number(
        require_member(value, "sharpe_ratio", context),
        context + ".sharpe_ratio"
    );
    metrics.max_drawdown = require_number(
        require_member(value, "max_drawdown", context),
        context + ".max_drawdown"
    );
    metrics.number_of_trades = require_count(
        require_member(value, "number_of_trades", context),
        context + ".number_of_trades"
    );
    metrics.total_costs = require_number(
        require_member(value, "total_costs", context),
        context + ".total_costs"
    );

    return metrics;
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

Trade trade_from_json(
    const JsonValue& value,
    const std::string& context
) {
    require_object(value, context);

    Trade trade;
    trade.symbol = require_string(
        require_member(value, "symbol", context),
        context + ".symbol"
    );

    const std::string side = require_string(
        require_member(value, "side", context),
        context + ".side"
    );

    if (side == "buy") {
        trade.side = OrderSide::Buy;
    } else if (side == "sell") {
        trade.side = OrderSide::Sell;
    } else {
        fail(context + ".side has unsupported value '" + side + "'");
    }

    trade.quantity = require_number(
        require_member(value, "quantity", context),
        context + ".quantity"
    );
    trade.fill_price = require_number(
        require_member(value, "fill_price", context),
        context + ".fill_price"
    );
    trade.commission = require_number(
        require_member(value, "commission", context),
        context + ".commission"
    );
    trade.slippage_cost = require_number(
        require_member(value, "slippage_cost", context),
        context + ".slippage_cost"
    );

    return trade;
}

JsonValue outcome_to_json(const ExperimentJobOutcome& outcome) {
    JsonObject object;
    object.emplace("ok", outcome.ok);
    object.emplace("duration_seconds", outcome.duration_seconds);
    object.emplace("asset", outcome.result.asset);

    if (!outcome.ok) {
        object.emplace(
            "error",
            outcome.error.empty()
                ? std::string("job did not reach a terminal state")
                : outcome.error
        );
        return JsonValue(std::move(object));
    }

    object.emplace(
        "metrics",
        metrics_to_json(outcome.result.metrics)
    );

    JsonValue::Array trades;
    trades.reserve(outcome.result.trades.size());

    for (const Trade& trade : outcome.result.trades) {
        trades.push_back(trade_to_json(trade));
    }

    object.emplace("trades", JsonValue(std::move(trades)));

    return JsonValue(std::move(object));
}

ExperimentJobOutcome outcome_from_json(
    const JsonValue& value,
    const ExperimentJob& job,
    const std::string& context
) {
    require_object(value, context);

    ExperimentJobOutcome outcome;
    outcome.job = job;
    outcome.ok = require_boolean(
        require_member(value, "ok", context),
        context + ".ok"
    );
    outcome.duration_seconds = require_number(
        require_member(value, "duration_seconds", context),
        context + ".duration_seconds"
    );

    if (value.contains("asset")) {
        outcome.result.asset = require_string(
            value.at("asset"),
            context + ".asset"
        );
    }

    if (!outcome.ok) {
        outcome.error = require_string(
            require_member(value, "error", context),
            context + ".error"
        );

        if (outcome.error.empty()) {
            outcome.error = "job did not reach a terminal state";
        }

        return outcome;
    }

    outcome.result.job_id = job.job_id;
    outcome.result.parameters = job.parameters;
    outcome.result.metrics = metrics_from_json(
        require_member(value, "metrics", context),
        context + ".metrics"
    );

    if (value.contains("trades")) {
        const JsonValue::Array& trades = require_array(
            value.at("trades"),
            context + ".trades"
        );

        outcome.result.trades.reserve(trades.size());

        for (std::size_t i = 0; i < trades.size(); ++i) {
            outcome.result.trades.push_back(
                trade_from_json(
                    trades[i],
                    context + ".trades[" + std::to_string(i) + "]"
                )
            );
        }
    }

    return outcome;
}

std::string read_text_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);

    if (!stream) {
        fail("Could not open coordinator state file '" + path.string() + "'");
    }

    std::ostringstream contents;
    contents << stream.rdbuf();

    if (!stream.good() && !stream.eof()) {
        fail("Could not read coordinator state file '" + path.string() + "'");
    }

    return contents.str();
}

void write_text_atomically(const fs::path& path, const std::string& text) {
    std::error_code error;

    const fs::path parent = path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, error);

        if (error) {
            fail(
                "Could not create state directory '" + parent.string() +
                "': " + error.message()
            );
        }
    }

    fs::path temporary = path;
    temporary += ".tmp";

    {
        std::ofstream stream(
            temporary,
            std::ios::binary | std::ios::trunc
        );

        if (!stream) {
            fail(
                "Could not open coordinator state file '" +
                temporary.string() + "' for writing"
            );
        }

        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();

        if (!stream) {
            fail(
                "Could not write coordinator state file '" +
                temporary.string() + "'"
            );
        }
    }

    fs::rename(temporary, path, error);

    if (error) {
        // Some platforms refuse to replace an existing target. Removing it
        // keeps the update path working, at the cost of a tiny window where
        // neither name exists.
        std::error_code ignored;
        fs::remove(path, ignored);
        error.clear();
        fs::rename(temporary, path, error);

        if (error) {
            fs::remove(temporary, ignored);
            fail(
                "Could not replace coordinator state file '" +
                path.string() + "': " + error.message()
            );
        }
    }
}

}  // namespace

CoordinatorStateStore::CoordinatorStateStore(
    std::string path,
    const ExperimentRequest& request
) : path_(std::move(path)),
    experiment_id_(request.experiment_id),
    jobs_(request.jobs) {
    if (path_.empty()) {
        throw std::invalid_argument(
            "Coordinator state path must not be empty"
        );
    }
}

CoordinatorRecovery CoordinatorStateStore::load(
    CoordinatorStateSnapshot& snapshot
) const {
    snapshot.states.assign(jobs_.size(), JobState::pending);
    snapshot.attempts.assign(jobs_.size(), 0);
    snapshot.outcomes.assign(jobs_.size(), std::nullopt);

    CoordinatorRecovery recovery;

    std::error_code error;
    const bool exists = fs::exists(path_, error);

    if (error) {
        throw std::runtime_error(
            "Could not inspect coordinator state file '" + path_ +
            "': " + error.message()
        );
    }

    if (!exists) {
        return recovery;
    }

    const std::string text = read_text_file(path_);

    try {
        const JsonValue root = parse_json(text);
        require_object(root, "$");

        const std::size_t version = require_count(
            require_member(root, "state_version", "$"),
            "$.state_version"
        );

        if (version != 1) {
            fail(
                "unsupported state_version " + std::to_string(version) +
                "; this build understands version 1"
            );
        }

        const std::string experiment_id = require_string(
            require_member(root, "experiment_id", "$"),
            "$.experiment_id"
        );

        if (experiment_id != experiment_id_) {
            fail(
                "state belongs to experiment '" + experiment_id +
                "', not '" + experiment_id_ + "'"
            );
        }

        const JsonValue::Array& jobs = require_array(
            require_member(root, "jobs", "$"),
            "$.jobs"
        );

        if (jobs.size() != jobs_.size()) {
            fail(
                "state has " + std::to_string(jobs.size()) +
                " job(s), but the experiment has " +
                std::to_string(jobs_.size())
            );
        }

        recovery.recovered = true;

        for (std::size_t i = 0; i < jobs_.size(); ++i) {
            const ExperimentJob& expected = jobs_[i];
            const std::string context =
                "$.jobs[" + std::to_string(i) + "]";

            const JsonValue& entry = require_object(jobs[i], context);

            const std::string job_id = require_string(
                require_member(entry, "job_id", context),
                context + ".job_id"
            );

            if (job_id != expected.job_id) {
                fail(
                    context + ".job_id is '" + job_id +
                    "', expected '" + expected.job_id + "'"
                );
            }

            const std::string strategy_id = require_string(
                require_member(entry, "strategy_id", context),
                context + ".strategy_id"
            );

            if (strategy_id != expected.strategy_id) {
                fail(
                    context + ".strategy_id is '" + strategy_id +
                    "', expected '" + expected.strategy_id + "'"
                );
            }

            const std::string strategy_type = require_string(
                require_member(entry, "strategy_type", context),
                context + ".strategy_type"
            );

            if (strategy_type != expected.strategy_type) {
                fail(
                    context + ".strategy_type is '" + strategy_type +
                    "', expected '" + expected.strategy_type + "'"
                );
            }

            const ParameterMap parameters = parameters_from_json(
                require_member(entry, "parameters", context),
                context + ".parameters"
            );

            if (!same_parameters(parameters, expected.parameters)) {
                fail(
                    context + ".parameters do not match the experiment's" +
                    " strategy parameters"
                );
            }

            const JobState state = parse_job_state(
                require_string(
                    require_member(entry, "state", context),
                    context + ".state"
                ),
                context + ".state"
            );

            snapshot.attempts[i] = require_count(
                require_member(entry, "attempts", context),
                context + ".attempts"
            );

            if (entry.contains("outcome")) {
                ExperimentJobOutcome outcome = outcome_from_json(
                    entry.at("outcome"),
                    expected,
                    context + ".outcome"
                );

                const JobState derived = outcome.ok
                    ? JobState::completed
                    : JobState::failed;

                if (state != JobState::pending &&
                    state != JobState::running &&
                    state != derived) {
                    fail(
                        context + " has state '" +
                        std::string(job_state_name(state)) +
                        "' but its outcome is '" +
                        std::string(job_state_name(derived)) + "'"
                    );
                }

                // A terminal outcome is authoritative. The persisted state
                // may lag it by one transition when a snapshot overlaps a
                // worker's commit.
                snapshot.states[i] = derived;
                snapshot.outcomes[i] = std::move(outcome);

                if (derived == JobState::completed) {
                    ++recovery.jobs_completed;
                } else {
                    ++recovery.jobs_failed;
                }

                continue;
            }

            if (state == JobState::completed ||
                state == JobState::failed) {
                fail(
                    context + " is '" +
                    std::string(job_state_name(state)) +
                    "' but has no persisted outcome"
                );
            }

            snapshot.states[i] = JobState::pending;

            if (state == JobState::running) {
                // The previous coordinator may have died after handing the job
                // to a worker. The result is unknown, so make it available
                // again; the persisted attempt count still bounds retries.
                ++recovery.jobs_requeued;
            }
        }
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "Invalid coordinator state file '" + path_ + "': " +
            error.what()
        );
    }

    return recovery;
}

void CoordinatorStateStore::save(
    const CoordinatorStateSnapshot& snapshot
) const {
    if (snapshot.states.size() != jobs_.size() ||
        snapshot.attempts.size() != jobs_.size() ||
        snapshot.outcomes.size() != jobs_.size()) {
        throw std::invalid_argument(
            "Coordinator state snapshot must have one entry per job"
        );
    }

    JsonObject root;
    root.emplace("state_version", 1);
    root.emplace("experiment_id", experiment_id_);

    JsonValue::Array jobs;
    jobs.reserve(jobs_.size());

    for (std::size_t i = 0; i < jobs_.size(); ++i) {
        const ExperimentJob& job = jobs_[i];

        JsonObject entry;
        entry.emplace("job_id", job.job_id);
        entry.emplace("strategy_id", job.strategy_id);
        entry.emplace("strategy_type", job.strategy_type);
        entry.emplace("parameters", parameters_to_json(job.parameters));
        entry.emplace(
            "attempts",
            static_cast<double>(snapshot.attempts[i])
        );

        // A terminal outcome is authoritative. This keeps a snapshot written
        // concurrently with a state transition from pairing an outcome with a
        // stale running/pending state.
        if (snapshot.outcomes[i]) {
            entry.emplace(
                "state",
                job_state_name(
                    snapshot.outcomes[i]->ok
                        ? JobState::completed
                        : JobState::failed
                )
            );
            entry.emplace(
                "outcome",
                outcome_to_json(*snapshot.outcomes[i])
            );
        } else {
            entry.emplace(
                "state",
                job_state_name(snapshot.states[i])
            );
        }

        jobs.push_back(JsonValue(std::move(entry)));
    }

    root.emplace("jobs", JsonValue(std::move(jobs)));

    std::string text = serialize_json(
        JsonValue(std::move(root)),
        /*pretty=*/true
    );
    text.push_back('\n');

    write_text_atomically(path_, text);
}

const std::string& CoordinatorStateStore::path() const noexcept {
    return path_;
}
