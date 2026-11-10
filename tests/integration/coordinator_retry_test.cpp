#include "comm/tcp_channel.h"
#include "coordinator/coordinator.h"
#include "experiment/experiment.h"
#include "net/tcp.h"
#include "protocol/protocol.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
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

// Joins a thread during stack unwinding so a failed assertion reports the
// failure instead of terminating the process on a joinable std::thread.
class ThreadJoiner {
public:
    explicit ThreadJoiner(std::thread& thread) : thread_(thread) {}

    ~ThreadJoiner() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    ThreadJoiner(const ThreadJoiner&) = delete;
    ThreadJoiner& operator=(const ThreadJoiner&) = delete;

private:
    std::thread& thread_;
};

// Signals a shared flag on scope exit so a waiting worker cannot hang if the
// signalling worker throws.
class FlagGuard {
public:
    FlagGuard(
        std::mutex& mutex,
        std::condition_variable& condition,
        bool& flag
    ) : mutex_(mutex), condition_(condition), flag_(flag) {}

    ~FlagGuard() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            flag_ = true;
        }
        condition_.notify_all();
    }

    FlagGuard(const FlagGuard&) = delete;
    FlagGuard& operator=(const FlagGuard&) = delete;

private:
    std::mutex& mutex_;
    std::condition_variable& condition_;
    bool& flag_;
};

struct CoordinatorRun {
    ExperimentResult result;
    CoordinatorStats stats;
    std::exception_ptr error;
    std::vector<std::vector<JobStateTransition>> transitions;
    std::vector<WorkerDisconnectEvent> disconnects;

    std::mutex mutex;
    std::mutex transition_mutex;
    std::mutex disconnect_mutex;
    std::condition_variable condition;
    bool listening = false;
    bool done = false;
    std::uint16_t port = 0;

    void configure(CoordinatorConfig& config, std::size_t jobs) {
        transitions.resize(jobs);

        config.listen_host = "127.0.0.1";
        config.listen_port = 0;

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

        config.on_worker_disconnected =
            [this](const WorkerDisconnectEvent& event) {
                std::lock_guard<std::mutex> lock(disconnect_mutex);
                disconnects.push_back(event);
            };
    }

    void run_coordinator(
        const ExperimentRequest& request,
        const CoordinatorConfig& config
    ) {
        try {
            result = run_distributed_experiment(request, config, &stats);
        } catch (...) {
            error = std::current_exception();
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        condition.notify_all();
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

using WorkerBody = std::function<void(std::uint16_t, std::size_t)>;

void run_with_workers(
    CoordinatorRun& run,
    const ExperimentRequest& request,
    std::size_t worker_count,
    std::size_t max_attempts,
    bool retry_failed_results,
    const WorkerBody& worker_body
) {
    CoordinatorConfig config;
    run.configure(config, request.jobs.size());
    config.expected_workers = worker_count;
    config.max_job_attempts = max_attempts;
    config.retry_failed_results = retry_failed_results;

    std::thread coordinator([&] {
        run.run_coordinator(request, config);
    });
    ThreadJoiner coordinator_joiner(coordinator);

    const std::uint16_t port = run.wait_for_port();

    std::vector<std::exception_ptr> worker_errors(worker_count);
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (std::size_t i = 0; i < worker_count; ++i) {
        workers.emplace_back([&, i] {
            try {
                worker_body(port, i);
            } catch (...) {
                worker_errors[i] = std::current_exception();
            }
        });
    }

    coordinator.join();

    for (std::thread& worker : workers) {
        worker.join();
    }

    if (run.error) {
        std::rethrow_exception(run.error);
    }

    for (const std::exception_ptr& error : worker_errors) {
        if (error) {
            std::rethrow_exception(error);
        }
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

// Serves jobs until shutdown, producing one backtest_result per job.
void serve_results(
    std::uint16_t port,
    const std::string& worker_id,
    const std::function<BacktestResultMessage(const BacktestJobMessage&)>& reply
) {
    comm::TcpClientChannel channel("127.0.0.1", port);
    channel.connect();
    announce_worker(channel, worker_id);

    while (const std::optional<std::string> text = channel.receive()) {
        const ProtocolEnvelope envelope = parse_protocol_envelope(*text);

        if (envelope.type == "shutdown") {
            return;
        }

        const BacktestJobMessage job = parse_backtest_job_json(*text);
        channel.send(
            backtest_result_to_json(reply(job), /*pretty=*/false)
        );
    }
}

// Serves worker_error replies until shutdown. The coordinator treats every
// attempt as a retryable failure, so this drives the retry limit.
void serve_worker_errors(
    std::uint16_t port,
    const std::string& worker_id
) {
    comm::TcpClientChannel channel("127.0.0.1", port);
    channel.connect();
    announce_worker(channel, worker_id);

    while (const std::optional<std::string> text = channel.receive()) {
        const ProtocolEnvelope envelope = parse_protocol_envelope(*text);

        if (envelope.type == "shutdown") {
            return;
        }

        const BacktestJobMessage job = parse_backtest_job_json(*text);
        channel.send(
            worker_error_to_json(
                make_worker_error(
                    "execution_error",
                    "simulated attempt failure",
                    "",
                    job.message_id,
                    worker_id
                ),
                /*pretty=*/false
            )
        );
    }
}

std::size_t job_index(
    const ExperimentRequest& request,
    const std::string& job_id
) {
    for (std::size_t i = 0; i < request.jobs.size(); ++i) {
        if (request.jobs[i].job_id == job_id) {
            return i;
        }
    }

    throw std::runtime_error("unknown job_id '" + job_id + "'");
}

void test_zero_attempts_is_rejected() {
    const ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");

    CoordinatorConfig config;
    config.max_job_attempts = 0;

    try {
        (void)run_distributed_experiment(request, config);
    } catch (const std::invalid_argument&) {
        return;
    }

    throw std::runtime_error(
        "a zero-attempt retry policy must be rejected before dispatch"
    );
}

// A retryable runtime failure is retried with a fresh message_id and can
// succeed on a later attempt.
void test_retryable_failure_succeeds_on_second_attempt() {
    const ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");

    std::map<std::string, std::vector<std::string>> seen_message_ids;

    CoordinatorRun run;
    run_with_workers(
        run,
        request,
        /*worker_count=*/1,
        /*max_attempts=*/2,
        /*retry_failed_results=*/true,
        [&](std::uint16_t port, std::size_t) {
            comm::TcpClientChannel channel("127.0.0.1", port);
            channel.connect();
            announce_worker(channel, "retry-worker");

            while (const std::optional<std::string> text =
                       channel.receive()) {
                const ProtocolEnvelope envelope =
                    parse_protocol_envelope(*text);

                if (envelope.type == "shutdown") {
                    return;
                }

                const BacktestJobMessage job =
                    parse_backtest_job_json(*text);

                std::vector<std::string>& ids =
                    seen_message_ids[job.job.job_id];
                ids.push_back(job.message_id);

                if (ids.size() == 1) {
                    channel.send(
                        backtest_result_to_json(
                            make_result(
                                job,
                                "retry-worker",
                                "failed",
                                "first attempt failed"
                            ),
                            /*pretty=*/false
                        )
                    );
                } else {
                    channel.send(
                        backtest_result_to_json(
                            make_result(
                                job,
                                "retry-worker",
                                "completed"
                            ),
                            /*pretty=*/false
                        )
                    );
                }
            }
        }
    );

    expect(
        run.result.status == "completed",
        "retries must let a failed job complete, got " +
            run.result.status
    );
    expect(
        run.result.summary.jobs_completed == request.jobs.size(),
        "every job must complete after its retry"
    );
    expect(
        run.result.summary.jobs_failed == 0,
        "no job may remain failed"
    );
    expect(
        run.stats.jobs_dispatched == request.jobs.size() * 2,
        "each job must be dispatched twice"
    );
    expect(
        run.stats.results_received == request.jobs.size() * 2,
        "each attempt must produce a result"
    );
    expect(
        run.stats.job_retries == request.jobs.size(),
        "every job must record one retry"
    );
    expect(
        run.stats.job_attempts.size() == request.jobs.size(),
        "the coordinator must report attempts per job"
    );
    expect(
        run.stats.workers_disconnected == 0,
        "a returned failure must not lose the worker"
    );

    for (std::size_t i = 0; i < request.jobs.size(); ++i) {
        const std::string& job_id = request.jobs[i].job_id;
        const std::vector<std::string>& ids = seen_message_ids[job_id];

        expect(
            ids.size() == 2,
            "job '" + job_id + "' must be delivered twice"
        );
        expect(
            ids[0] != ids[1],
            "a retry must use a new correlation key"
        );
        expect(
            ids[1].find(":retry:2") != std::string::npos,
            "the retry key must identify the attempt"
        );
        expect(
            run.stats.job_attempts[i] == 2,
            "the per-job attempt count must include the retry"
        );
        expect(
            run.stats.job_states[i] == JobState::completed,
            "the retried job must finish completed"
        );

        const std::vector<JobStateTransition>& changes =
            run.transitions[i];

        expect(
            changes.size() == 4,
            "a retried success must publish four transitions"
        );
        expect(
            changes[0].from == JobState::pending &&
                changes[0].to == JobState::running,
            "the first attempt must enter running"
        );
        expect(
            changes[1].from == JobState::running &&
                changes[1].to == JobState::pending,
            "a retryable failure must return to pending"
        );
        expect(
            changes[2].from == JobState::pending &&
                changes[2].to == JobState::running,
            "the retry must enter running"
        );
        expect(
            changes[3].from == JobState::running &&
                changes[3].to == JobState::completed,
            "the retry must finish completed"
        );
    }
}

// A worker that reports an error and disappears must not take the job with it:
// the retry is offered to the healthy worker.
void test_retry_is_reassigned_to_another_worker() {
    const ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");

    std::string failed_job_id;
    std::vector<std::string> healthy_completed_jobs;

    // Keep the healthy worker from racing through the whole queue before the
    // flaky worker has received and failed its one job.
    std::mutex flaky_mutex;
    std::condition_variable flaky_condition;
    bool flaky_failed = false;

    CoordinatorRun run;
    run_with_workers(
        run,
        request,
        /*worker_count=*/2,
        /*max_attempts=*/2,
        /*retry_failed_results=*/false,
        [&](std::uint16_t port, std::size_t worker_index) {
            if (worker_index == 0) {
                FlagGuard guard(flaky_mutex, flaky_condition, flaky_failed);

                comm::TcpClientChannel channel("127.0.0.1", port);
                channel.connect();
                announce_worker(channel, "flaky-worker");

                const std::optional<std::string> text = channel.receive();
                expect(text.has_value(), "the flaky worker must get a job");

                const BacktestJobMessage job =
                    parse_backtest_job_json(*text);
                failed_job_id = job.job.job_id;

                channel.send(
                    worker_error_to_json(
                        make_worker_error(
                            "execution_error",
                            "transient failure",
                            "",
                            job.message_id,
                            "flaky-worker"
                        ),
                        /*pretty=*/false
                    )
                );

                // Disappear so the retry cannot run on this worker.
                channel.close();
                return;
            }

            comm::TcpClientChannel channel("127.0.0.1", port);
            channel.connect();
            announce_worker(channel, "healthy-worker");

            {
                std::unique_lock<std::mutex> lock(flaky_mutex);
                flaky_condition.wait(
                    lock,
                    [&] { return flaky_failed; }
                );
            }

            while (const std::optional<std::string> text =
                       channel.receive()) {
                const ProtocolEnvelope envelope =
                    parse_protocol_envelope(*text);

                if (envelope.type == "shutdown") {
                    return;
                }

                const BacktestJobMessage job =
                    parse_backtest_job_json(*text);
                healthy_completed_jobs.push_back(job.job.job_id);

                channel.send(
                    backtest_result_to_json(
                        make_result(job, "healthy-worker", "completed"),
                        /*pretty=*/false
                    )
                );
            }
        }
    );

    expect(!failed_job_id.empty(), "the flaky worker must report a job");
    expect(
        run.result.status == "completed",
        "the healthy worker must finish the experiment"
    );
    expect(
        run.result.summary.jobs_completed == request.jobs.size(),
        "reassignment must let every job complete"
    );
    expect(
        run.stats.workers_disconnected == 1,
        "the lost worker must be recorded"
    );
    expect(
        run.stats.worker_errors == 1,
        "the worker_error attempt must be counted"
    );
    expect(
        run.stats.results_received == request.jobs.size(),
        "the healthy worker must produce one result per job"
    );
    expect(
        run.stats.job_retries >= 1,
        "the failed job must be retried"
    );

    const std::size_t index = job_index(request, failed_job_id);

    expect(
        run.stats.job_attempts[index] == 2,
        "the failed job must be attempted twice"
    );
    expect(
        run.stats.job_states[index] == JobState::completed,
        "the retried job must complete on another worker"
    );
    expect(
        std::find(
            healthy_completed_jobs.begin(),
            healthy_completed_jobs.end(),
            failed_job_id
        ) != healthy_completed_jobs.end(),
        "the healthy worker must be the one that completes the retry"
    );

    const std::vector<WorkerDisconnectEvent>& disconnects =
        run.disconnects;

    expect(
        disconnects.size() == 1,
        "exactly one worker loss must be reported"
    );
    expect(
        disconnects[0].worker_id == "flaky-worker",
        "the lost worker must be identified"
    );
    expect(
        !disconnects[0].job_failed,
        "a retried in-flight job must not be marked failed"
    );
}

// An idle worker must not leave when the queue is momentarily empty: if the
// busy worker crashes, that idle worker is the only one left to run the retry.
void test_idle_worker_stays_available_for_retry() {
    ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");
    request.jobs.resize(1);

    CoordinatorRun run;
    run_with_workers(
        run,
        request,
        /*worker_count=*/2,
        /*max_attempts=*/2,
        /*retry_failed_results=*/true,
        [](std::uint16_t port, std::size_t worker_index) {
            const std::string worker_id =
                "worker-" + std::to_string(worker_index + 1);

            comm::TcpClientChannel channel("127.0.0.1", port);
            channel.connect();
            announce_worker(channel, worker_id);

            while (const std::optional<std::string> text =
                       channel.receive()) {
                const ProtocolEnvelope envelope =
                    parse_protocol_envelope(*text);

                if (envelope.type == "shutdown") {
                    return;
                }

                const BacktestJobMessage job =
                    parse_backtest_job_json(*text);

                // Both workers use the same policy: crash on the first
                // delivery, complete the retry. Exactly one of them receives
                // the first delivery, so the other is idle and must survive.
                if (job.message_id.find(":retry:") == std::string::npos) {
                    channel.close();
                    return;
                }

                channel.send(
                    backtest_result_to_json(
                        make_result(job, worker_id, "completed"),
                        /*pretty=*/false
                    )
                );
            }
        }
    );

    expect(
        run.result.status == "completed",
        "the surviving idle worker must complete the retried job"
    );
    expect(
        run.result.summary.jobs_completed == 1,
        "the only job must complete on the retry"
    );
    expect(
        run.result.summary.jobs_failed == 0,
        "the crashed attempt must not fail the job permanently"
    );
    expect(
        run.stats.workers_disconnected == 1,
        "the crashing worker must be detected"
    );
    expect(
        run.stats.jobs_dispatched == 2,
        "the job must be delivered to the crashed worker and then retried"
    );
    expect(
        run.stats.job_retries == 1,
        "the crash must schedule exactly one retry"
    );
    expect(
        run.stats.job_attempts.size() == 1 &&
            run.stats.job_attempts[0] == 2,
        "the crashed attempt and the retry must both count"
    );
    expect(
        run.stats.job_states.size() == 1 &&
            run.stats.job_states[0] == JobState::completed,
        "the retried job must finish completed"
    );

    expect(
        run.disconnects.size() == 1,
        "exactly one worker loss must be reported"
    );
    expect(
        run.disconnects[0].job_retry_scheduled,
        "the crashed in-flight job must be requeued for a retry"
    );
    expect(
        !run.disconnects[0].job_failed,
        "the crashed in-flight job must not be marked failed"
    );
}

// When every attempt is retryable and no worker ever succeeds, the job is
// finally failed after exactly max_job_attempts deliveries.
void test_retry_exhaustion_fails_the_job() {
    const ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");

    constexpr std::size_t kAttempts = 3;

    CoordinatorRun run;
    run_with_workers(
        run,
        request,
        /*worker_count=*/1,
        kAttempts,
        /*retry_failed_results=*/true,
        [](std::uint16_t port, std::size_t) {
            serve_worker_errors(port, "failing-worker");
        }
    );

    expect(
        run.result.status == "failed",
        "a run with no completed jobs must be failed"
    );
    expect(
        run.result.summary.jobs_failed == request.jobs.size(),
        "every job must fail after exhausting its attempts"
    );
    expect(
        run.stats.jobs_dispatched == request.jobs.size() * kAttempts,
        "each job must be dispatched exactly max_job_attempts times"
    );
    expect(
        run.stats.worker_errors == request.jobs.size() * kAttempts,
        "every attempt must produce a worker_error"
    );
    expect(
        run.stats.job_retries == request.jobs.size() * (kAttempts - 1),
        "all attempts after the first must count as retries"
    );
    expect(
        run.stats.workers_disconnected == 0,
        "a worker_error must not lose the worker"
    );

    for (std::size_t i = 0; i < request.jobs.size(); ++i) {
        expect(
            run.stats.job_attempts[i] == kAttempts,
            "the job must stop at the configured attempt limit"
        );
        expect(
            run.stats.job_states[i] == JobState::failed,
            "an exhausted job must finish failed"
        );
        expect(
            run.result.results[i].error == "simulated attempt failure",
            "the final failure must keep the last attempt's error"
        );

        const std::vector<JobStateTransition>& changes =
            run.transitions[i];

        expect(
            changes.size() == kAttempts * 2,
            "each attempt must enter running and then leave it"
        );
        expect(
            changes.back().to == JobState::failed,
            "the final transition must be failed"
        );
    }
}

// A validation failure is deterministic, so it must not be retried even when
// the retry policy allows attempts.
void test_invalid_result_is_not_retried() {
    const ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");

    CoordinatorRun run;
    run_with_workers(
        run,
        request,
        /*worker_count=*/1,
        /*max_attempts=*/3,
        /*retry_failed_results=*/true,
        [](std::uint16_t port, std::size_t) {
            serve_results(
                port,
                "validating-worker",
                [](const BacktestJobMessage& job) {
                    return make_result(
                        job,
                        "validating-worker",
                        "invalid",
                        "bad strategy parameters"
                    );
                }
            );
        }
    );

    expect(
        run.result.summary.jobs_failed == request.jobs.size(),
        "every invalid job must fail"
    );
    expect(
        run.stats.jobs_dispatched == request.jobs.size(),
        "invalid results must not be retried"
    );
    expect(
        run.stats.job_retries == 0,
        "no retry may be scheduled for a validation failure"
    );

    for (std::size_t i = 0; i < request.jobs.size(); ++i) {
        expect(
            run.stats.job_attempts[i] == 1,
            "an invalid job must be attempted once"
        );
        expect(
            run.result.results[i].error == "bad strategy parameters",
            "the validation error must be preserved"
        );
    }
}

// The runtime-failure retry is policy-controlled. With it disabled, a
// backtest_result status "failed" is terminal on the first attempt.
void test_failed_result_retry_can_be_disabled() {
    const ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");

    CoordinatorRun run;
    run_with_workers(
        run,
        request,
        /*worker_count=*/1,
        /*max_attempts=*/3,
        /*retry_failed_results=*/false,
        [](std::uint16_t port, std::size_t) {
            serve_results(
                port,
                "runtime-failure-worker",
                [](const BacktestJobMessage& job) {
                    return make_result(
                        job,
                        "runtime-failure-worker",
                        "failed",
                        "missing data"
                    );
                }
            );
        }
    );

    expect(
        run.result.summary.jobs_failed == request.jobs.size(),
        "every runtime failure must be terminal"
    );
    expect(
        run.stats.jobs_dispatched == request.jobs.size(),
        "failed results must not be retried when the policy is off"
    );
    expect(
        run.stats.job_retries == 0,
        "the retry policy must not schedule runtime retries"
    );

    for (std::size_t i = 0; i < request.jobs.size(); ++i) {
        expect(
            run.stats.job_attempts[i] == 1,
            "a disabled runtime retry must not consume attempts"
        );
    }
}

}  // namespace

int main() {
    try {
        test_zero_attempts_is_rejected();
        test_retryable_failure_succeeds_on_second_attempt();
        test_retry_is_reassigned_to_another_worker();
        test_idle_worker_stays_available_for_retry();
        test_retry_exhaustion_fails_the_job();
        test_invalid_result_is_not_retried();
        test_failed_result_retry_can_be_disabled();

        std::cout << "coordinator_retry_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "coordinator_retry_test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
