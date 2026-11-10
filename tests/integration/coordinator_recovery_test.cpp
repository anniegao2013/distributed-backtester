#include "comm/tcp_channel.h"
#include "coordinator/coordinator.h"
#include "coordinator/persistent_state.h"
#include "experiment/experiment.h"
#include "net/tcp.h"
#include "protocol/protocol.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
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

fs::path unique_path() {
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );

    return fs::temp_directory_path() /
        ("backtester_recovery_" + unique + ".json");
}

ExperimentJobOutcome completed_outcome(
    const ExperimentJob& job,
    double total_return,
    double sharpe
) {
    ExperimentJobOutcome outcome;
    outcome.job = job;
    outcome.ok = true;
    outcome.duration_seconds = 0.01;
    outcome.result.job_id = job.job_id;
    outcome.result.asset = "SP500";
    outcome.result.parameters = job.parameters;
    outcome.result.metrics.total_return = total_return;
    outcome.result.metrics.sharpe_ratio = sharpe;
    outcome.result.metrics.max_drawdown = 0.1;
    outcome.result.metrics.number_of_trades = 2;
    outcome.result.metrics.total_costs = 1.0;
    return outcome;
}

CoordinatorStateSnapshot fresh_snapshot(std::size_t jobs) {
    CoordinatorStateSnapshot snapshot;
    snapshot.states.assign(jobs, JobState::pending);
    snapshot.attempts.assign(jobs, 0);
    snapshot.outcomes.assign(jobs, std::nullopt);
    return snapshot;
}

void write_state(
    const fs::path& path,
    const ExperimentRequest& request,
    const CoordinatorStateSnapshot& snapshot
) {
    CoordinatorStateStore store(path.string(), request);
    store.save(snapshot);
}

struct CoordinatorRun {
    ExperimentResult result;
    CoordinatorStats stats;
    std::exception_ptr error;
    std::vector<std::vector<JobStateTransition>> transitions;

    std::mutex mutex;
    std::mutex transition_mutex;
    std::condition_variable condition;
    bool listening = false;
    bool done = false;
    std::uint16_t port = 0;

    void configure(
        CoordinatorConfig& config,
        std::size_t jobs,
        const std::string& state_path
    ) {
        transitions.resize(jobs);

        config.listen_host = "127.0.0.1";
        config.listen_port = 0;
        config.expected_workers = 1;
        config.state_path = state_path;

        config.on_listening = [this](const net::TcpEndpoint& endpoint) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                port = endpoint.port;
                listening = true;
            }
            condition.notify_all();
        };

        config.on_job_state_changed =
            [this](const JobStateTransition& change) {
                std::lock_guard<std::mutex> lock(transition_mutex);
                transitions.at(change.index).push_back(change);
            };
    }

    std::uint16_t wait_for_port() {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [this] { return listening || done; });

        if (error) {
            const std::exception_ptr captured = error;
            lock.unlock();
            std::rethrow_exception(captured);
        }

        expect(listening, "coordinator must report its listening endpoint");
        return port;
    }
};

BacktestResultMessage make_result(
    const BacktestJobMessage& job,
    const std::string& worker_id
) {
    BacktestResultMessage message;
    message.message_id = job.message_id;
    message.worker_id = worker_id;
    message.result.experiment_id = job.job.experiment_id;
    message.result.job_id = job.job.job_id;
    message.result.strategy_id = job.job.strategy_id;
    message.result.strategy_type = job.job.strategy_type;
    message.result.parameters = job.job.parameters;
    message.result.status = "completed";
    message.result.duration_seconds = 0.01;
    message.result.metrics = BacktestMetrics{0.05, 0.75, 0.15, 4, 2.0};
    return message;
}

void run_with_worker(
    CoordinatorRun& run,
    const ExperimentRequest& request,
    const fs::path& state_path,
    std::size_t max_attempts,
    const std::function<void(std::uint16_t)>& worker_body
) {
    CoordinatorConfig config;
    run.configure(config, request.jobs.size(), state_path.string());
    config.max_job_attempts = max_attempts;

    std::thread coordinator([&] {
        try {
            run.result = run_distributed_experiment(
                request,
                config,
                &run.stats
            );
        } catch (...) {
            run.error = std::current_exception();
        }

        {
            std::lock_guard<std::mutex> lock(run.mutex);
            run.done = true;
        }
        run.condition.notify_all();
    });

    const std::uint16_t port = run.wait_for_port();

    std::exception_ptr worker_error;
    std::thread worker([&] {
        try {
            worker_body(port);
        } catch (...) {
            worker_error = std::current_exception();
        }
    });

    coordinator.join();
    worker.join();

    if (run.error) {
        std::rethrow_exception(run.error);
    }

    if (worker_error) {
        std::rethrow_exception(worker_error);
    }
}

// A completed state file means the experiment is already finished. A relaunch
// must return the aggregated result without accepting a worker or binding a
// socket.
void test_all_terminal_state_completes_without_workers() {
    const fs::path path = unique_path();
    const ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");

    CoordinatorStateSnapshot snapshot = fresh_snapshot(request.jobs.size());

    for (std::size_t i = 0; i < request.jobs.size(); ++i) {
        snapshot.states[i] = JobState::completed;
        snapshot.attempts[i] = i + 1;
        snapshot.outcomes[i] = completed_outcome(
            request.jobs[i],
            0.1 * static_cast<double>(i + 1),
            1.0 - 0.1 * static_cast<double>(i)
        );
    }

    write_state(path, request, snapshot);

    CoordinatorConfig config;
    config.expected_workers = 1;
    config.state_path = path.string();

    bool listened = false;
    config.on_listening = [&listened](const net::TcpEndpoint&) {
        listened = true;
    };

    CoordinatorStats stats;
    const ExperimentResult result =
        run_distributed_experiment(request, config, &stats);

    expect(!listened, "a fully recovered run must not bind a socket");
    expect(
        result.status == "completed",
        "a fully completed state file produces a completed result"
    );
    expect(
        result.summary.jobs_completed == request.jobs.size(),
        "every recovered job counts as completed"
    );
    expect(
        result.summary.jobs_failed == 0,
        "no recovered job is failed"
    );
    expect(
        stats.state_recovered,
        "the run reports that it recovered state"
    );
    expect(
        stats.jobs_recovered == request.jobs.size(),
        "every terminal job is reported as recovered"
    );
    expect(
        stats.jobs_requeued_after_restart == 0,
        "nothing was running when the state was written"
    );
    expect(
        stats.workers_registered == 0,
        "no worker is registered on the fast path"
    );
    expect(
        stats.job_states.size() == request.jobs.size() &&
            stats.job_states[0] == JobState::completed,
        "recovered states are exposed in the stats"
    );

    // The best job must come from the restored metrics, not a fresh run.
    expect(
        result.summary.best_job_id == request.jobs[0].job_id,
        "aggregation uses the persisted metrics"
    );

    CoordinatorStateSnapshot reloaded;
    CoordinatorStateStore store(path.string(), request);
    const CoordinatorRecovery recovery = store.load(reloaded);

    expect(recovery.recovered, "the state file still exists");
    expect(
        recovery.jobs_completed == request.jobs.size(),
        "the fast path leaves the completed state intact"
    );

    fs::remove(path);
}

// Terminal jobs are reused, a running job is requeued, and a pending job is
// dispatched for the first time.
void test_partial_state_dispatches_only_unfinished_jobs() {
    const fs::path path = unique_path();
    const ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");
    expect(request.jobs.size() == 4, "the sample experiment has four jobs");

    CoordinatorStateSnapshot snapshot = fresh_snapshot(request.jobs.size());
    snapshot.states = {
        JobState::completed,
        JobState::running,
        JobState::pending,
        JobState::completed
    };
    snapshot.attempts = {1, 1, 0, 2};
    snapshot.outcomes[0] = completed_outcome(request.jobs[0], 0.11, 1.5);
    snapshot.outcomes[3] = completed_outcome(request.jobs[3], 0.44, 0.5);

    write_state(path, request, snapshot);

    std::vector<std::string> received_job_ids;
    std::vector<std::string> received_message_ids;

    CoordinatorRun run;
    run_with_worker(
        run,
        request,
        path,
        /*max_attempts=*/3,
        [&received_job_ids, &received_message_ids](std::uint16_t port) {
            comm::TcpClientChannel channel("127.0.0.1", port);
            channel.connect();

            WorkerReadyMessage ready;
            ready.message_id = "recovery-worker:ready:0001";
            ready.worker_id = "recovery-worker";
            channel.send(worker_ready_to_json(ready, /*pretty=*/false));

            while (const std::optional<std::string> text =
                       channel.receive()) {
                const ProtocolEnvelope envelope =
                    parse_protocol_envelope(*text);

                if (envelope.type == "shutdown") {
                    return;
                }

                const BacktestJobMessage job =
                    parse_backtest_job_json(*text);

                received_job_ids.push_back(job.job.job_id);
                received_message_ids.push_back(job.message_id);

                channel.send(
                    backtest_result_to_json(
                        make_result(job, "recovery-worker"),
                        /*pretty=*/false
                    )
                );
            }
        }
    );

    expect(
        received_job_ids.size() == 2,
        "only the running and pending jobs are dispatched"
    );
    expect(
        received_job_ids[0] == request.jobs[1].job_id &&
            received_job_ids[1] == request.jobs[2].job_id,
        "unfinished jobs keep their request order"
    );
    expect(
        received_message_ids[0] ==
            request.experiment_id + ":" + request.jobs[1].job_id +
                ":2:retry:2",
        "a resumed job continues at the persisted attempt number"
    );
    expect(
        received_message_ids[1] ==
            request.experiment_id + ":" + request.jobs[2].job_id + ":3",
        "a pending job starts at attempt 1"
    );

    expect(
        run.result.status == "completed",
        "recovered and newly executed jobs form a completed experiment"
    );
    expect(
        run.result.summary.jobs_completed == 4 &&
            run.result.summary.jobs_failed == 0,
        "every job is completed exactly once"
    );
    expect(
        run.result.results[0].result.metrics.total_return == 0.11 &&
            run.result.results[3].result.metrics.total_return == 0.44,
        "recovered outcomes are included unchanged"
    );

    expect(run.stats.state_recovered, "state recovery is reported");
    expect(
        run.stats.jobs_recovered == 2,
        "two terminal outcomes are recovered"
    );
    expect(
        run.stats.jobs_requeued_after_restart == 1,
        "the running job is requeued"
    );
    expect(
        run.stats.jobs_exhausted_after_restart == 0,
        "attempts remain for the requeued job"
    );
    expect(
        run.stats.jobs_dispatched == 2,
        "only unfinished jobs are dispatched"
    );
    expect(
        run.stats.results_received == 2,
        "recovered results are not received again"
    );
    expect(
        run.stats.job_retries == 1,
        "the requeued job's delivery counts as a retry"
    );
    expect(
        run.stats.job_attempts ==
            std::vector<std::size_t>({1, 2, 1, 2}),
        "attempt counts merge persisted and new deliveries"
    );
    expect(
        run.stats.job_states ==
            std::vector<JobState>(4, JobState::completed),
        "recovered jobs stay completed and unfinished jobs finish"
    );

    CoordinatorStateSnapshot reloaded;
    CoordinatorStateStore store(path.string(), request);
    const CoordinatorRecovery recovery = store.load(reloaded);

    expect(recovery.recovered, "the final state file is readable");
    expect(
        recovery.jobs_completed == 4,
        "the final checkpoint contains every completed job"
    );
    expect(
        reloaded.attempts == std::vector<std::size_t>({1, 2, 1, 2}),
        "the final checkpoint contains the merged attempt counts"
    );

    fs::remove(path);
}

// A job that used its last attempt before the restart cannot run again. It is
// failed during recovery; the remaining work still executes.
void test_exhausted_job_fails_during_recovery() {
    const fs::path path = unique_path();
    ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");
    request.jobs.resize(2);

    CoordinatorStateSnapshot snapshot = fresh_snapshot(2);
    snapshot.states = {JobState::running, JobState::pending};
    snapshot.attempts = {2, 0};

    write_state(path, request, snapshot);

    std::vector<std::string> received_job_ids;

    CoordinatorRun run;
    run_with_worker(
        run,
        request,
        path,
        /*max_attempts=*/2,
        [&received_job_ids](std::uint16_t port) {
            comm::TcpClientChannel channel("127.0.0.1", port);
            channel.connect();

            WorkerReadyMessage ready;
            ready.message_id = "exhausted-worker:ready:0001";
            ready.worker_id = "exhausted-worker";
            channel.send(worker_ready_to_json(ready, /*pretty=*/false));

            while (const std::optional<std::string> text =
                       channel.receive()) {
                const ProtocolEnvelope envelope =
                    parse_protocol_envelope(*text);

                if (envelope.type == "shutdown") {
                    return;
                }

                const BacktestJobMessage job =
                    parse_backtest_job_json(*text);

                received_job_ids.push_back(job.job.job_id);

                channel.send(
                    backtest_result_to_json(
                        make_result(job, "exhausted-worker"),
                        /*pretty=*/false
                    )
                );
            }
        }
    );

    expect(
        received_job_ids.size() == 1 &&
            received_job_ids[0] == request.jobs[1].job_id,
        "an exhausted job is never dispatched again"
    );

    expect(
        run.result.summary.jobs_completed == 1 &&
            run.result.summary.jobs_failed == 1,
        "the exhausted job is failed and the pending job completes"
    );
    expect(
        run.result.status == "partial",
        "a mix of completed and failed jobs is partial"
    );

    const ExperimentJobOutcome& failed = run.result.results[0];

    expect(!failed.ok, "the exhausted job has a failed outcome");
    expect(
        failed.error ==
            "job was interrupted by a coordinator restart after 2 "
            "attempt(s); no attempts remain",
        "the exhausted job explains why it cannot run"
    );

    expect(run.stats.state_recovered, "state recovery is reported");
    expect(
        run.stats.jobs_recovered == 0,
        "the state file had no terminal outcomes"
    );
    expect(
        run.stats.jobs_requeued_after_restart == 1,
        "the running job was first requeued by load()"
    );
    expect(
        run.stats.jobs_exhausted_after_restart == 1,
        "the requeued job was then failed as exhausted"
    );
    expect(
        run.stats.jobs_dispatched == 1 &&
            run.stats.job_attempts ==
                std::vector<std::size_t>({2, 1}),
        "only the pending job is dispatched"
    );

    CoordinatorStateSnapshot reloaded;
    CoordinatorStateStore store(path.string(), request);
    const CoordinatorRecovery recovery = store.load(reloaded);

    expect(
        recovery.jobs_completed == 1 && recovery.jobs_failed == 1,
        "the exhausted failure is durable"
    );
    expect(
        reloaded.states[0] == JobState::failed &&
            reloaded.states[1] == JobState::completed,
        "final states are written to the checkpoint"
    );

    fs::remove(path);
}

// A mismatched state file fails before the listening socket is created.
void test_invalid_state_rejected_before_binding() {
    const fs::path path = unique_path();
    const ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");

    CoordinatorStateSnapshot snapshot = fresh_snapshot(request.jobs.size());
    snapshot.states[0] = JobState::completed;
    snapshot.attempts[0] = 1;
    snapshot.outcomes[0] = completed_outcome(request.jobs[0], 0.1, 1.0);
    write_state(path, request, snapshot);

    ExperimentRequest other = request;
    other.experiment_id = "different-experiment";

    CoordinatorConfig config;
    config.expected_workers = 1;
    config.state_path = path.string();

    bool listened = false;
    config.on_listening = [&listened](const net::TcpEndpoint&) {
        listened = true;
    };

    expect_failure(
        [&] {
            run_distributed_experiment(other, config);
        },
        "a state file for another experiment must fail the run"
    );

    expect(
        !listened,
        "state validation happens before the coordinator binds"
    );

    fs::remove(path);
}

}  // namespace

int main() {
    try {
        test_all_terminal_state_completes_without_workers();
        test_partial_state_dispatches_only_unfinished_jobs();
        test_exhausted_job_fails_during_recovery();
        test_invalid_state_rejected_before_binding();

        std::cout << "coordinator_recovery_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "coordinator_recovery_test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
