#include "coordinator/persistent_state.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expect_failure(
    const std::function<void()>& action,
    const std::string& message
) {
    try {
        action();
    } catch (const std::exception&) {
        return;
    }

    throw std::runtime_error(message);
}

bool same_number(double left, double right) {
    const double scale = std::max({1.0, std::abs(left), std::abs(right)});
    return std::abs(left - right) <= 1e-12 * scale;
}

ExperimentRequest make_request() {
    ExperimentRequest request;
    request.experiment_id = "recovery-unit";
    request.data = {
        "data/market.csv",
        "TEST",
        "2020-01-01",
        "2020-12-31"
    };
    request.jobs = {
        {
            "ma-1",
            "ma",
            "moving_average",
            {{"short_window", 5.0}, {"long_window", 20.0}}
        },
        {
            "ma-2",
            "ma",
            "moving_average",
            {{"short_window", 10.0}, {"long_window", 30.0}}
        }
    };

    return request;
}

ExperimentJobOutcome completed_outcome(
    const ExperimentJob& job,
    double total_return
) {
    ExperimentJobOutcome outcome;
    outcome.job = job;
    outcome.ok = true;
    outcome.duration_seconds = 0.25;
    outcome.result.job_id = job.job_id;
    outcome.result.asset = "TEST";
    outcome.result.parameters = job.parameters;
    outcome.result.metrics.total_return = total_return;
    outcome.result.metrics.sharpe_ratio = 1.5;
    outcome.result.metrics.max_drawdown = 0.2;
    outcome.result.metrics.number_of_trades = 3;
    outcome.result.metrics.total_costs = 4.5;

    Trade trade;
    trade.symbol = "TEST";
    trade.side = OrderSide::Sell;
    trade.quantity = 2.5;
    trade.fill_price = 101.25;
    trade.commission = 0.5;
    trade.slippage_cost = 0.25;
    outcome.result.trades.push_back(trade);

    return outcome;
}

ExperimentJobOutcome failed_outcome(const ExperimentJob& job) {
    ExperimentJobOutcome outcome;
    outcome.job = job;
    outcome.ok = false;
    outcome.error = "simulated failure";
    outcome.duration_seconds = 0.1;
    return outcome;
}

fs::path unique_path(const std::string& suffix) {
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );

    return fs::temp_directory_path() /
        ("backtester_state_" + unique + suffix);
}

CoordinatorStateSnapshot fresh_snapshot(std::size_t jobs) {
    CoordinatorStateSnapshot snapshot;
    snapshot.states.assign(jobs, JobState::pending);
    snapshot.attempts.assign(jobs, 0);
    snapshot.outcomes.assign(jobs, std::nullopt);
    return snapshot;
}

void test_missing_file_starts_fresh() {
    const fs::path path = unique_path(".json");

    CoordinatorStateStore store(path.string(), make_request());

    CoordinatorStateSnapshot snapshot;
    const CoordinatorRecovery recovery = store.load(snapshot);

    expect(!recovery.recovered, "a missing file is not a recovery");
    expect(snapshot.states.size() == 2, "the snapshot has one entry per job");
    expect(
        snapshot.states[0] == JobState::pending &&
            snapshot.states[1] == JobState::pending,
        "a fresh run starts every job pending"
    );
    expect(
        snapshot.attempts[0] == 0 && snapshot.attempts[1] == 0,
        "a fresh run has no attempts"
    );
    expect(
        !snapshot.outcomes[0] && !snapshot.outcomes[1],
        "a fresh run has no outcomes"
    );
    expect(!fs::exists(path), "load must not create a state file");
}

void test_round_trip_restores_outcomes_and_requeues_running() {
    const fs::path path = unique_path(".json");
    const ExperimentRequest request = make_request();

    CoordinatorStateStore store(path.string(), request);

    CoordinatorStateSnapshot snapshot = fresh_snapshot(2);
    snapshot.states[0] = JobState::completed;
    snapshot.attempts[0] = 1;
    snapshot.outcomes[0] = completed_outcome(request.jobs[0], 0.12);

    // The second job was running when the previous coordinator stopped.
    snapshot.states[1] = JobState::running;
    snapshot.attempts[1] = 2;

    store.save(snapshot);

    expect(fs::exists(path), "save must create the state file");

    fs::path temporary = path;
    temporary += ".tmp";
    expect(
        !fs::exists(temporary),
        "an atomic save must leave no temporary file"
    );

    CoordinatorStateSnapshot loaded;
    const CoordinatorRecovery recovery = store.load(loaded);

    expect(recovery.recovered, "an existing file is recovered");
    expect(recovery.jobs_completed == 1, "one completed job is restored");
    expect(recovery.jobs_failed == 0, "no failed job is restored");
    expect(recovery.jobs_requeued == 1, "a running job is requeued");
    expect(
        loaded.states[0] == JobState::completed &&
            loaded.states[1] == JobState::pending,
        "completed stays terminal and running returns to pending"
    );
    expect(
        loaded.attempts[0] == 1 && loaded.attempts[1] == 2,
        "attempt counts survive a round trip"
    );
    expect(
        loaded.outcomes[0] && !loaded.outcomes[1],
        "only the terminal job has an outcome"
    );

    const ExperimentJobOutcome& outcome = *loaded.outcomes[0];

    expect(outcome.ok, "the restored outcome is successful");
    expect(outcome.job.job_id == "ma-1", "the outcome is bound to its job");
    expect(
        same_number(outcome.result.metrics.total_return, 0.12),
        "metrics survive serialization"
    );
    expect(
        outcome.result.metrics.number_of_trades == 3,
        "trade counts survive serialization"
    );
    expect(
        outcome.result.trades.size() == 1 &&
            outcome.result.trades[0].side == OrderSide::Sell,
        "trades survive serialization"
    );
    expect(
        same_number(outcome.result.trades[0].fill_price, 101.25),
        "trade values survive serialization"
    );

    fs::remove(path);
}

void test_failed_outcome_round_trip() {
    const fs::path path = unique_path(".json");
    const ExperimentRequest request = make_request();

    CoordinatorStateStore store(path.string(), request);

    CoordinatorStateSnapshot snapshot = fresh_snapshot(2);
    snapshot.states[0] = JobState::failed;
    snapshot.attempts[0] = 3;
    snapshot.outcomes[0] = failed_outcome(request.jobs[0]);
    snapshot.states[1] = JobState::completed;
    snapshot.attempts[1] = 1;
    snapshot.outcomes[1] = completed_outcome(request.jobs[1], -0.05);

    store.save(snapshot);

    CoordinatorStateSnapshot loaded;
    const CoordinatorRecovery recovery = store.load(loaded);

    expect(recovery.jobs_completed == 1, "one completed job is counted");
    expect(recovery.jobs_failed == 1, "one failed job is counted");
    expect(
        loaded.states[0] == JobState::failed &&
            loaded.states[1] == JobState::completed,
        "terminal states are restored exactly"
    );
    expect(
        loaded.outcomes[0] && !loaded.outcomes[0]->ok &&
            loaded.outcomes[0]->error == "simulated failure",
        "the failure message survives serialization"
    );

    fs::remove(path);
}

void test_outcome_wins_over_lagging_state() {
    const fs::path path = unique_path(".json");
    const ExperimentRequest request = make_request();

    CoordinatorStateStore store(path.string(), request);

    // This is the shape a snapshot can take if a worker commits an outcome
    // while another thread is persisting a still-running state.
    CoordinatorStateSnapshot snapshot = fresh_snapshot(2);
    snapshot.states[0] = JobState::running;
    snapshot.attempts[0] = 1;
    snapshot.outcomes[0] = completed_outcome(request.jobs[0], 0.3);

    store.save(snapshot);

    CoordinatorStateSnapshot loaded;
    const CoordinatorRecovery recovery = store.load(loaded);

    expect(
        loaded.states[0] == JobState::completed,
        "a terminal outcome overrides a lagging running state"
    );
    expect(
        recovery.jobs_completed == 1 && recovery.jobs_requeued == 0,
        "the job is terminal, not requeued"
    );

    fs::remove(path);
}

void test_identity_mismatch_is_rejected() {
    const fs::path path = unique_path(".json");
    ExperimentRequest request = make_request();

    CoordinatorStateStore store(path.string(), request);
    CoordinatorStateSnapshot snapshot = fresh_snapshot(2);
    snapshot.states[0] = JobState::completed;
    snapshot.attempts[0] = 1;
    snapshot.outcomes[0] = completed_outcome(request.jobs[0], 0.1);
    store.save(snapshot);

    ExperimentRequest other_experiment = request;
    other_experiment.experiment_id = "another-experiment";

    expect_failure(
        [&] {
            CoordinatorStateStore other(path.string(), other_experiment);
            CoordinatorStateSnapshot ignored;
            other.load(ignored);
        },
        "a state file from a different experiment must be rejected"
    );

    ExperimentRequest other_parameters = request;
    other_parameters.jobs[1].parameters["short_window"] = 15.0;

    expect_failure(
        [&] {
            CoordinatorStateStore other(path.string(), other_parameters);
            CoordinatorStateSnapshot ignored;
            other.load(ignored);
        },
        "a job identity mismatch must be rejected"
    );

    fs::remove(path);
}

void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);

    if (!stream) {
        throw std::runtime_error("could not create test state file");
    }

    stream << contents;
}

void test_malformed_and_inconsistent_files_are_rejected() {
    const fs::path malformed = unique_path(".json");
    write_file(malformed, "{not json");

    expect_failure(
        [&] {
            CoordinatorStateStore store(malformed.string(), make_request());
            CoordinatorStateSnapshot ignored;
            store.load(ignored);
        },
        "malformed JSON must be rejected"
    );

    fs::remove(malformed);

    const fs::path wrong_version = unique_path(".json");
    write_file(
        wrong_version,
        R"({"state_version":2,"experiment_id":"recovery-unit","jobs":[]})"
    );

    expect_failure(
        [&] {
            CoordinatorStateStore store(
                wrong_version.string(),
                make_request()
            );
            CoordinatorStateSnapshot ignored;
            store.load(ignored);
        },
        "an unsupported state version must be rejected"
    );

    fs::remove(wrong_version);

    const fs::path missing_outcome = unique_path(".json");
    write_file(
        missing_outcome,
        R"({"state_version":1,"experiment_id":"recovery-unit","jobs":[)"
        R"({"job_id":"ma-1","strategy_id":"ma","strategy_type":"moving_average",)"
        R"("parameters":{"short_window":5,"long_window":20},"state":"completed","attempts":1},)"
        R"({"job_id":"ma-2","strategy_id":"ma","strategy_type":"moving_average",)"
        R"("parameters":{"short_window":10,"long_window":30},"state":"pending","attempts":0}]})"
    );

    expect_failure(
        [&] {
            CoordinatorStateStore store(
                missing_outcome.string(),
                make_request()
            );
            CoordinatorStateSnapshot ignored;
            store.load(ignored);
        },
        "a terminal state without an outcome must be rejected"
    );

    fs::remove(missing_outcome);
}

void test_constructor_requires_a_path() {
    expect_failure(
        [] {
            CoordinatorStateStore store("", make_request());
        },
        "an empty state path must be rejected"
    );
}

}  // namespace

int main() {
    try {
        test_missing_file_starts_fresh();
        test_round_trip_restores_outcomes_and_requeues_running();
        test_failed_outcome_round_trip();
        test_outcome_wins_over_lagging_state();
        test_identity_mismatch_is_rejected();
        test_malformed_and_inconsistent_files_are_rejected();
        test_constructor_requires_a_path();

        std::cout << "persistent_state_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "persistent_state_test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
