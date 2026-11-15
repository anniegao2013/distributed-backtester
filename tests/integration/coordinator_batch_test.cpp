#include "comm/tcp_channel.h"
#include "coordinator/coordinator.h"
#include "experiment/experiment.h"
#include "net/tcp.h"
#include "protocol/protocol.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
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

    void run(
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

    std::vector<WorkerDisconnectEvent> take_disconnects() {
        std::lock_guard<std::mutex> lock(disconnect_mutex);
        return disconnects;
    }

private:
    std::mutex disconnect_mutex;
};

void announce_worker(
    comm::MessageChannel& channel,
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

// Reads one job and then drains whatever the coordinator has already
// prefetched, up to `batch_limit`. Replies to the whole batch in order. This is
// how a worker observes that dispatch prefetching happened.
void serve_batches(
    std::uint16_t port,
    const std::string& worker_id,
    std::size_t batch_limit,
    std::size_t& max_batch,
    std::size_t& handled
) {
    comm::TcpChannel channel(net::connect_to("127.0.0.1", port));
    announce_worker(channel, worker_id);

    for (;;) {
        const std::optional<std::string> text = channel.receive();

        if (!text) {
            return;
        }

        if (parse_protocol_envelope(*text).type == "shutdown") {
            return;
        }

        std::vector<BacktestJobMessage> jobs;
        jobs.push_back(parse_backtest_job_json(*text));

        bool shutdown_seen = false;

        while (jobs.size() < batch_limit) {
            std::optional<std::string> extra;

            try {
                extra = channel.receive_for(std::chrono::milliseconds(100));
            } catch (const net::TimeoutError&) {
                break;
            }

            if (!extra) {
                break;
            }

            if (parse_protocol_envelope(*extra).type == "shutdown") {
                shutdown_seen = true;
                break;
            }

            jobs.push_back(parse_backtest_job_json(*extra));
        }

        max_batch = std::max(max_batch, jobs.size());
        handled += jobs.size();

        for (const BacktestJobMessage& job : jobs) {
            channel.send(
                backtest_result_to_json(
                    completed_result(job, worker_id),
                    /*pretty=*/false
                )
            );
        }

        if (shutdown_seen) {
            return;
        }
    }
}

// Serves one job at a time until shutdown, optionally pausing before each
// reply so a sibling worker has time to claim its own batch.
void serve_results(
    std::uint16_t port,
    const std::string& worker_id,
    std::chrono::milliseconds reply_delay
) {
    comm::TcpChannel channel(net::connect_to("127.0.0.1", port));
    announce_worker(channel, worker_id);

    while (const std::optional<std::string> text = channel.receive()) {
        if (parse_protocol_envelope(*text).type == "shutdown") {
            return;
        }

        const BacktestJobMessage job = parse_backtest_job_json(*text);

        if (reply_delay.count() > 0) {
            std::this_thread::sleep_for(reply_delay);
        }

        channel.send(
            backtest_result_to_json(
                completed_result(job, worker_id),
                /*pretty=*/false
            )
        );
    }
}

void expect_request_order(
    const ExperimentRequest& request,
    const ExperimentResult& result
) {
    expect(
        result.results.size() == request.jobs.size(),
        "every job must produce one outcome"
    );

    for (std::size_t i = 0; i < request.jobs.size(); ++i) {
        expect(
            result.results[i].job.job_id == request.jobs[i].job_id,
            "batched outcomes must stay in request order"
        );
    }
}

// With enough queued work the coordinator keeps several jobs assigned to the
// same worker. The worker must still produce one ordered result per job, and
// every job must complete exactly once.
void test_prefetch_batches_and_preserves_order() {
    const ExperimentRequest request =
        load_experiment_file("experiments/rsi_sweep.json");

    expect(
        request.jobs.size() == 8,
        "the batch experiment must expand to eight jobs"
    );

    constexpr std::size_t kBatchLimit = 4;

    CoordinatorRun run;
    CoordinatorConfig config;
    run.configure(config);
    config.expected_workers = 1;
    config.max_job_attempts = 1;
    config.job_timeout = std::chrono::seconds(10);
    config.dispatch_batch_size = kBatchLimit;

    std::thread coordinator([&] { run.run(request, config); });
    ThreadJoiner coordinator_joiner(coordinator);

    const std::uint16_t port = run.wait_for_port();

    std::size_t max_batch = 0;
    std::size_t handled = 0;
    std::exception_ptr worker_error;

    std::thread worker([&] {
        try {
            serve_batches(
                port,
                "batch-worker",
                kBatchLimit,
                max_batch,
                handled
            );
        } catch (...) {
            worker_error = std::current_exception();
        }
    });
    ThreadJoiner worker_joiner(worker);

    coordinator.join();
    worker.join();

    if (run.error) {
        std::rethrow_exception(run.error);
    }

    if (worker_error) {
        std::rethrow_exception(worker_error);
    }

    expect(
        run.result.status == "completed",
        "batched dispatch must complete, got " + run.result.status
    );
    expect(
        run.result.summary.jobs_completed == request.jobs.size(),
        "every batched job must complete exactly once"
    );
    expect(
        run.result.summary.jobs_failed == 0,
        "batched dispatch must not fail a job"
    );
    expect(
        max_batch >= 2,
        "the coordinator must prefetch more than one job per worker"
    );
    expect(
        handled == request.jobs.size(),
        "the worker must account for every completed job"
    );
    expect(
        run.stats.job_retries == 0,
        "batched jobs must not need retries"
    );
    expect(
        run.stats.workers_disconnected == 0,
        "the healthy batched worker must stay connected"
    );
    expect(
        run.stats.job_attempts.size() == request.jobs.size(),
        "the coordinator must report one attempt entry per job"
    );

    for (std::size_t i = 0; i < request.jobs.size(); ++i) {
        expect(
            run.stats.job_attempts[i] == 1,
            "every batched job must be delivered exactly once"
        );
        expect(
            run.stats.job_states[i] == JobState::completed,
            "every batched job must finish completed"
        );
    }

    expect_request_order(request, run.result);
}

// Losing a worker with prefetched jobs must return every unacknowledged job to
// the queue. With another healthy worker and attempts remaining, they run again
// and the experiment still completes.
void test_lost_batch_is_requeued() {
    const ExperimentRequest request =
        load_experiment_file("experiments/rsi_sweep.json");

    expect(
        request.jobs.size() == 8,
        "the batch-loss experiment must expand to eight jobs"
    );

    CoordinatorRun run;
    CoordinatorConfig config;
    run.configure(config);
    config.expected_workers = 2;
    config.max_job_attempts = 2;
    config.job_timeout = std::chrono::seconds(10);
    config.dispatch_batch_size = 4;

    std::thread coordinator([&] { run.run(request, config); });
    ThreadJoiner coordinator_joiner(coordinator);

    const std::uint16_t port = run.wait_for_port();

    std::exception_ptr quitter_error;
    std::thread quitter([&] {
        try {
            comm::TcpChannel channel(net::connect_to("127.0.0.1", port));
            announce_worker(channel, "quitter-worker");

            // Hold the connection open until the coordinator assigns work,
            // then disappear without acknowledging any of it.
            if (const std::optional<std::string> text = channel.receive()) {
                (void)parse_protocol_envelope(*text);
            }

            channel.close();
        } catch (...) {
            quitter_error = std::current_exception();
        }
    });
    ThreadJoiner quitter_joiner(quitter);

    std::exception_ptr steady_error;
    std::thread steady([&] {
        try {
            serve_results(
                port,
                "steady-worker",
                std::chrono::milliseconds(10)
            );
        } catch (...) {
            steady_error = std::current_exception();
        }
    });
    ThreadJoiner steady_joiner(steady);

    coordinator.join();
    quitter.join();
    steady.join();

    if (run.error) {
        std::rethrow_exception(run.error);
    }

    if (quitter_error) {
        std::rethrow_exception(quitter_error);
    }

    if (steady_error) {
        std::rethrow_exception(steady_error);
    }

    expect(
        run.result.status == "completed",
        "requeued batch jobs must complete on the healthy worker, got " +
            run.result.status
    );
    expect(
        run.result.summary.jobs_completed == request.jobs.size(),
        "every job must complete after a batch is requeued"
    );
    expect(
        run.result.summary.jobs_failed == 0,
        "a lost worker must not fail jobs that still have attempts"
    );
    expect(
        run.stats.workers_disconnected == 1,
        "the coordinator must count the lost worker"
    );
    expect(
        run.stats.job_retries >= 1,
        "the lost worker's prefetched jobs must be retried"
    );

    bool retried_job = false;

    for (std::size_t i = 0; i < request.jobs.size(); ++i) {
        expect(
            run.stats.job_states[i] == JobState::completed,
            "every requeued job must finish completed"
        );
        expect(
            run.stats.job_attempts[i] >= 1,
            "every job must have at least one delivery"
        );

        if (run.stats.job_attempts[i] >= 2) {
            retried_job = true;
        }
    }

    expect(
        retried_job,
        "at least one job must show a second dispatch attempt"
    );

    const std::vector<WorkerDisconnectEvent> disconnects =
        run.take_disconnects();

    expect(
        disconnects.size() == 1,
        "exactly one worker loss must be reported"
    );
    expect(
        disconnects[0].worker_id == "quitter-worker",
        "the event must identify the lost worker"
    );
    expect(
        !disconnects[0].job_id.empty(),
        "the lost worker's in-flight job must be reported"
    );
    expect(
        disconnects[0].job_retry_scheduled,
        "the lost worker's in-flight job must be requeued"
    );

    expect_request_order(request, run.result);
}

void test_zero_dispatch_batch_is_rejected() {
    const ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");

    CoordinatorConfig config;
    config.dispatch_batch_size = 0;

    try {
        (void)run_distributed_experiment(request, config);
    } catch (const std::invalid_argument&) {
        return;
    }

    throw std::runtime_error(
        "a zero dispatch batch must be rejected before dispatch"
    );
}

}  // namespace

int main() {
    try {
        test_prefetch_batches_and_preserves_order();
        test_lost_batch_is_requeued();
        test_zero_dispatch_batch_is_rejected();

        std::cout << "coordinator_batch_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "coordinator_batch_test failed: " << error.what()
                  << '\n';
        return 1;
    }
}
