#include "comm/tcp_channel.h"
#include "coordinator/coordinator.h"
#include "experiment/experiment.h"
#include "net/tcp.h"
#include "protocol/protocol.h"

#include <chrono>
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

struct CoordinatorRun {
    ExperimentResult result;
    CoordinatorStats stats;
    std::exception_ptr error;
    std::vector<WorkerDisconnectEvent> disconnects;

    std::mutex mutex;
    std::mutex disconnect_mutex;
    std::condition_variable condition;
    bool listening = false;
    bool done = false;
    std::uint16_t port = 0;

    void configure(CoordinatorConfig& config) {
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
    std::chrono::milliseconds job_timeout,
    const WorkerBody& worker_body
) {
    CoordinatorConfig config;
    run.configure(config);
    config.expected_workers = worker_count;
    config.max_job_attempts = max_attempts;
    config.job_timeout = job_timeout;

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

BacktestResultMessage completed_result(
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
    message.result.metrics = BacktestMetrics{0.1, 0.5, 0.2, 3, 12.0};
    return message;
}

ExperimentRequest single_job_request() {
    ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");
    request.jobs.resize(1);
    return request;
}

// A connected worker that receives a job and never answers must not stall the
// run forever. With no attempts left the timeout is terminal.
void test_timeout_fails_job_with_no_attempts_left() {
    const ExperimentRequest request = single_job_request();

    CoordinatorRun run;
    run_with_workers(
        run,
        request,
        /*worker_count=*/1,
        /*max_attempts=*/1,
        std::chrono::milliseconds(150),
        [](std::uint16_t port, std::size_t) {
            comm::TcpClientChannel channel("127.0.0.1", port);
            channel.connect();
            announce_worker(channel, "hung-worker");

            const std::optional<std::string> text = channel.receive();
            expect(text.has_value(), "the hung worker must get the job");

            // Stay connected but never reply. The coordinator must close the
            // connection after the timeout, which ends this read.
            while (channel.receive().has_value()) {
            }
        }
    );

    expect(
        run.result.status == "failed",
        "a timed-out job with no attempts left must fail the run"
    );
    expect(
        run.result.summary.jobs_failed == 1,
        "the timed-out job must be recorded as failed"
    );
    expect(
        run.stats.job_timeouts == 1,
        "the timeout must be counted"
    );
    expect(
        run.stats.workers_disconnected == 1,
        "a timed-out worker must be removed from scheduling"
    );
    expect(
        run.stats.transport_failures == 1,
        "a timeout must also count as a transport failure"
    );
    expect(
        run.stats.job_attempts.size() == 1 && run.stats.job_attempts[0] == 1,
        "the timed-out job must consume exactly one attempt"
    );
    expect(
        run.stats.job_states.size() == 1 &&
            run.stats.job_states[0] == JobState::failed,
        "the timed-out job must finish failed"
    );
    expect(
        run.result.results.size() == 1 &&
            run.result.results[0].error.find("did not return a result") !=
                std::string::npos,
        "the failure must explain that the result timed out"
    );
    expect(
        run.disconnects.size() == 1,
        "exactly one worker loss must be reported"
    );
    expect(
        run.disconnects[0].worker_id == "hung-worker",
        "the timed-out worker must be identified"
    );
    expect(
        run.disconnects[0].job_failed,
        "the job must be reported as failed when no attempts remain"
    );
    expect(
        !run.disconnects[0].job_retry_scheduled,
        "an exhausted job must not be scheduled for another attempt"
    );
}

// Both workers follow the same policy: whichever receives the first delivery
// hangs, and the other one stays idle until the retry arrives. This makes the
// reassignment deterministic without relying on thread scheduling order.
void test_timeout_is_retried_on_another_worker() {
    const ExperimentRequest request = single_job_request();

    std::mutex mutex;
    std::condition_variable condition;
    bool first_delivery_seen = false;

    CoordinatorRun run;
    run_with_workers(
        run,
        request,
        /*worker_count=*/2,
        /*max_attempts=*/2,
        std::chrono::milliseconds(200),
        [&](std::uint16_t port, std::size_t worker_index) {
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

                if (job.message_id.find(":retry:") != std::string::npos) {
                    channel.send(
                        backtest_result_to_json(
                            completed_result(job, worker_id),
                            /*pretty=*/false
                        )
                    );
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lock(mutex);
                    first_delivery_seen = true;
                }
                condition.notify_all();

                // Hang until the coordinator times this attempt out and the
                // connection is closed.
                while (channel.receive().has_value()) {
                }
                return;
            }
        }
    );

    expect(
        first_delivery_seen,
        "one worker must have received the first delivery"
    );
    expect(
        run.result.status == "completed",
        "the healthy worker must complete the retried job"
    );
    expect(
        run.result.summary.jobs_completed == 1,
        "the retried job must complete exactly once"
    );
    expect(
        run.result.summary.jobs_failed == 0,
        "a retryable timeout must not fail the job"
    );
    expect(
        run.stats.job_timeouts == 1,
        "exactly one attempt must time out"
    );
    expect(
        run.stats.jobs_dispatched == 2,
        "the job must be delivered once to each worker"
    );
    expect(
        run.stats.job_retries == 1,
        "the timeout must schedule exactly one retry"
    );
    expect(
        run.stats.results_received == 1,
        "only the healthy retry must produce a result"
    );
    expect(
        run.stats.workers_disconnected == 1,
        "only the hung worker must be removed"
    );
    expect(
        run.stats.job_attempts.size() == 1 && run.stats.job_attempts[0] == 2,
        "the timeout and the retry must both count as attempts"
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
        "the timed-out attempt must be requeued"
    );
    expect(
        !run.disconnects[0].job_failed,
        "the timed-out job must not be marked failed while retries remain"
    );
}

// A timeout that is generous relative to the reply must not fire at all.
void test_prompt_worker_does_not_time_out() {
    ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");
    request.jobs.resize(2);

    CoordinatorRun run;
    run_with_workers(
        run,
        request,
        /*worker_count=*/1,
        /*max_attempts=*/1,
        std::chrono::milliseconds(2000),
        [](std::uint16_t port, std::size_t) {
            comm::TcpClientChannel channel("127.0.0.1", port);
            channel.connect();
            announce_worker(channel, "prompt-worker");

            while (const std::optional<std::string> text =
                       channel.receive()) {
                const ProtocolEnvelope envelope =
                    parse_protocol_envelope(*text);

                if (envelope.type == "shutdown") {
                    return;
                }

                const BacktestJobMessage job =
                    parse_backtest_job_json(*text);
                channel.send(
                    backtest_result_to_json(
                        completed_result(job, "prompt-worker"),
                        /*pretty=*/false
                    )
                );
            }
        }
    );

    expect(
        run.result.status == "completed",
        "a prompt worker must complete the run, got " + run.result.status +
            " with " + std::to_string(run.stats.job_timeouts) +
            " timeout(s), " + std::to_string(run.stats.workers_disconnected) +
            " disconnect(s)"
    );
    expect(
        run.result.summary.jobs_completed == 2,
        "both jobs must complete"
    );
    expect(
        run.stats.job_timeouts == 0,
        "no timeout may fire for a prompt reply"
    );
    expect(
        run.stats.workers_disconnected == 0,
        "a prompt worker must stay connected"
    );
}

void test_negative_timeout_is_rejected() {
    const ExperimentRequest request = single_job_request();

    CoordinatorConfig config;
    config.listen_port = 0;
    config.job_timeout = std::chrono::milliseconds(-1);

    try {
        (void)run_distributed_experiment(request, config);
    } catch (const std::invalid_argument&) {
        return;
    }

    throw std::runtime_error(
        "a negative job timeout must be rejected before dispatch"
    );
}

}  // namespace

int main() {
    try {
        test_timeout_fails_job_with_no_attempts_left();
        test_timeout_is_retried_on_another_worker();
        test_prompt_worker_does_not_time_out();
        test_negative_timeout_is_rejected();

        std::cout << "coordinator_timeout_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "coordinator_timeout_test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
