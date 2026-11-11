#include "comm/tcp_channel.h"
#include "coordinator/coordinator.h"
#include "experiment/experiment.h"
#include "net/tcp.h"
#include "protocol/protocol.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
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

    void configure(CoordinatorConfig& config, std::size_t jobs) {
        transitions.resize(jobs);

        config.listen_host = "127.0.0.1";
        config.listen_port = 0;
        config.expected_workers = 1;

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

using WorkerBody = std::function<void(std::uint16_t)>;

// Runs one coordinator and one scripted worker. The worker owns the client
// socket so each test can decide exactly which replies to submit and when.
void run_with_worker(
    CoordinatorRun& run,
    const ExperimentRequest& request,
    std::size_t max_attempts,
    const WorkerBody& worker_body
) {
    CoordinatorConfig config;
    run.configure(config, request.jobs.size());
    config.max_job_attempts = max_attempts;

    std::thread coordinator([&] {
        try {
            run.result = run_distributed_experiment(request, config, &run.stats);
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

void announce_worker(
    comm::TcpClientChannel& channel,
    const std::string& worker_id
) {
    WorkerReadyMessage ready;
    ready.message_id = worker_id + ":ready:0001";
    ready.worker_id = worker_id;
    channel.send(worker_ready_to_json(ready, /*pretty=*/false));
}

BacktestResultMessage make_result(
    const BacktestJobMessage& job,
    const std::string& worker_id,
    const std::string& status,
    const std::string& error_message = {}
) {
    BacktestResultMessage message;
    message.message_id = job.message_id;
    message.worker_id = worker_id;
    message.result.experiment_id = job.job.experiment_id;
    message.result.job_id = job.job.job_id;
    message.result.strategy_id = job.job.strategy_id;
    message.result.strategy_type = job.job.strategy_type;
    message.result.parameters = job.job.parameters;
    message.result.status = status;
    message.result.duration_seconds = 0.01;

    if (status == "completed") {
        message.result.metrics = BacktestMetrics{0.1, 0.5, 0.2, 3, 12.0};
        return message;
    }

    BacktestResultError error;
    error.type =
        status == "invalid" ? "validation_error" : "runtime_error";
    error.message = error_message.empty()
        ? "simulated " + status + " result"
        : error_message;
    message.result.error = error;

    return message;
}

void expect_all_completed(
    const CoordinatorRun& run,
    const ExperimentRequest& request,
    std::size_t expected_transitions_per_job
) {
    expect(
        run.result.status == "completed",
        "the run must complete despite duplicate submissions"
    );
    expect(
        run.result.summary.jobs_completed == request.jobs.size(),
        "every job must complete exactly once"
    );
    expect(
        run.result.summary.jobs_failed == 0,
        "duplicates must not turn a result into a failure"
    );
    expect(
        run.stats.workers_disconnected == 0,
        "a duplicate must not be treated as a connection loss"
    );

    for (std::size_t i = 0; i < request.jobs.size(); ++i) {
        expect(
            run.stats.job_states[i] == JobState::completed,
            "the job must finish completed"
        );
        expect(
            run.transitions[i].size() == expected_transitions_per_job,
            "a duplicate must not publish extra state transitions"
        );
    }
}

// A worker that submits the same completed result twice must not have the
// second copy consumed as the reply to its next job.
void test_duplicate_completed_results_are_ignored() {
    const ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");
    expect(request.jobs.size() == 4, "the sample experiment has four jobs");

    CoordinatorRun run;
    run_with_worker(
        run,
        request,
        /*max_attempts=*/3,
        [](std::uint16_t port) {
            comm::TcpClientChannel channel("127.0.0.1", port);
            channel.connect();
            announce_worker(channel, "duplicate-worker");

            while (const std::optional<std::string> text =
                       channel.receive()) {
                const ProtocolEnvelope envelope =
                    parse_protocol_envelope(*text);

                if (envelope.type == "shutdown") {
                    return;
                }

                const BacktestJobMessage job =
                    parse_backtest_job_json(*text);

                const std::string result = backtest_result_to_json(
                    make_result(job, "duplicate-worker", "completed"),
                    /*pretty=*/false
                );

                // Submit the same correlated result twice before asking for
                // the next job.
                channel.send(result);
                channel.send(result);
            }
        }
    );

    expect_all_completed(
        run,
        request,
        /*expected_transitions_per_job=*/2
    );

    expect(
        run.stats.jobs_dispatched == request.jobs.size(),
        "each job must be dispatched once"
    );
    expect(
        run.stats.results_received == request.jobs.size(),
        "only the first submission counts as a received result"
    );
    expect(
        run.stats.job_retries == 0,
        "duplicates must not schedule retries"
    );

    // The last duplicate is still buffered when the run completes, so only the
    // earlier extras are read and ignored.
    expect(
        run.stats.duplicate_results == request.jobs.size() - 1,
        "every duplicate read before completion must be counted"
    );
}

// A retryable failure submitted twice must schedule one retry, not two.
void test_duplicate_failed_result_schedules_one_retry() {
    ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");
    request.jobs.resize(1);

    CoordinatorRun run;
    run_with_worker(
        run,
        request,
        /*max_attempts=*/2,
        [](std::uint16_t port) {
            comm::TcpClientChannel channel("127.0.0.1", port);
            channel.connect();
            announce_worker(channel, "retrying-worker");

            while (const std::optional<std::string> text =
                       channel.receive()) {
                const ProtocolEnvelope envelope =
                    parse_protocol_envelope(*text);

                if (envelope.type == "shutdown") {
                    return;
                }

                const BacktestJobMessage job =
                    parse_backtest_job_json(*text);

                if (job.message_id.find(":retry:") != std::string::npos) {
                    channel.send(
                        backtest_result_to_json(
                            make_result(job, "retrying-worker", "completed"),
                            /*pretty=*/false
                        )
                    );
                    continue;
                }

                const std::string failure = backtest_result_to_json(
                    make_result(
                        job,
                        "retrying-worker",
                        "failed",
                        "first attempt failed"
                    ),
                    /*pretty=*/false
                );

                channel.send(failure);
                channel.send(failure);
            }
        }
    );

    expect_all_completed(
        run,
        request,
        /*expected_transitions_per_job=*/4
    );

    expect(
        run.stats.jobs_dispatched == 2,
        "a duplicate failure must not cause a second retry dispatch"
    );
    expect(
        run.stats.results_received == 2,
        "the failure and the successful retry must each count once"
    );
    expect(
        run.stats.duplicate_results == 1,
        "the duplicate failure must be ignored"
    );
    expect(
        run.stats.job_retries == 1,
        "the job must be retried exactly once"
    );
    expect(
        run.stats.job_attempts.size() == 1 &&
            run.stats.job_attempts[0] == 2,
        "the duplicate must not consume an extra attempt"
    );
}

// The same rule applies to a worker_error reply: a repeated error is ignored
// and does not trigger an extra attempt.
void test_duplicate_worker_error_schedules_one_retry() {
    ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");
    request.jobs.resize(1);

    CoordinatorRun run;
    run_with_worker(
        run,
        request,
        /*max_attempts=*/2,
        [](std::uint16_t port) {
            comm::TcpClientChannel channel("127.0.0.1", port);
            channel.connect();
            announce_worker(channel, "erroring-worker");

            while (const std::optional<std::string> text =
                       channel.receive()) {
                const ProtocolEnvelope envelope =
                    parse_protocol_envelope(*text);

                if (envelope.type == "shutdown") {
                    return;
                }

                const BacktestJobMessage job =
                    parse_backtest_job_json(*text);

                if (job.message_id.find(":retry:") != std::string::npos) {
                    channel.send(
                        backtest_result_to_json(
                            make_result(job, "erroring-worker", "completed"),
                            /*pretty=*/false
                        )
                    );
                    continue;
                }

                const std::string error = worker_error_to_json(
                    make_worker_error(
                        "execution_error",
                        "first attempt failed",
                        "",
                        job.message_id,
                        "erroring-worker"
                    ),
                    /*pretty=*/false
                );

                channel.send(error);
                channel.send(error);
            }
        }
    );

    expect_all_completed(
        run,
        request,
        /*expected_transitions_per_job=*/4
    );

    expect(
        run.stats.jobs_dispatched == 2,
        "a duplicate worker_error must not cause a second retry dispatch"
    );
    expect(
        run.stats.worker_errors == 1,
        "only the first worker_error counts"
    );
    expect(
        run.stats.duplicate_results == 1,
        "the repeated worker_error must be ignored"
    );
    expect(
        run.stats.job_retries == 1,
        "the job must be retried exactly once"
    );
    expect(
        run.stats.job_attempts.size() == 1 &&
            run.stats.job_attempts[0] == 2,
        "the duplicate must not consume an extra attempt"
    );
}

}  // namespace

int main() {
    try {
        test_duplicate_completed_results_are_ignored();
        test_duplicate_failed_result_schedules_one_retry();
        test_duplicate_worker_error_schedules_one_retry();

        std::cout << "coordinator_duplicate_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "coordinator_duplicate_test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
