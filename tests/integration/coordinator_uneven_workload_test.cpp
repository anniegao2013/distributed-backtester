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

// A slow worker and a slow job must not block FIFO dispatch or force retries.
void test_uneven_job_durations() {
    constexpr std::size_t kJobs = 8;
    constexpr int kRounds = 3;

    for (int round = 0; round < kRounds; ++round) {
        ExperimentRequest request =
            load_experiment_file("experiments/rsi_sweep.json");

        expect(
            request.jobs.size() == kJobs,
            "the uneven-workload experiment must expand to eight jobs"
        );

        // One job is much slower than the rest, whichever worker gets it.
        const std::string slow_job_id = request.jobs[kJobs / 2].job_id;

        std::vector<std::size_t> completed_by_worker(2, 0);

        CoordinatorRun run;
        CoordinatorConfig config;
        run.configure(config);
        config.expected_workers = 2;
        config.max_job_attempts = 2;
        config.job_timeout = std::chrono::seconds(10);

        std::thread coordinator([&] {
            run.run_coordinator(request, config);
        });
        ThreadJoiner coordinator_joiner(coordinator);

        const std::uint16_t port = run.wait_for_port();

        std::vector<std::exception_ptr> worker_errors(2);
        std::vector<std::thread> workers;
        workers.reserve(2);

        for (std::size_t worker_index = 0; worker_index < 2; ++worker_index) {
            workers.emplace_back([&, worker_index] {
                try {
                    const bool slow_worker = worker_index == 0;
                    const std::string worker_id = slow_worker
                        ? "slow-worker"
                        : "fast-worker";

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

                        if (slow_worker) {
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(100)
                            );
                        }

                        if (job.job.job_id == slow_job_id) {
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(250)
                            );
                        }

                        ++completed_by_worker[worker_index];
                        channel.send(
                            backtest_result_to_json(
                                completed_result(job, worker_id),
                                /*pretty=*/false
                            )
                        );
                    }
                } catch (...) {
                    worker_errors[worker_index] =
                        std::current_exception();
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

        const std::string prefix =
            "round " + std::to_string(round + 1) + ": ";

        expect(
            run.result.status == "completed",
            prefix + "uneven workload must complete, got " +
                run.result.status + " with " +
                std::to_string(run.stats.job_retries) + " retr(ies), " +
                std::to_string(run.stats.job_timeouts) + " timeout(s)"
        );
        expect(
            run.result.summary.jobs_completed == kJobs,
            prefix + "every job must complete exactly once"
        );
        expect(
            run.result.summary.jobs_failed == 0,
            prefix + "no job may fail under uneven workload"
        );
        expect(
            run.stats.job_retries == 0,
            prefix + "uneven durations must not cause retries"
        );
        expect(
            run.stats.workers_disconnected == 0,
            prefix + "both healthy workers must stay connected"
        );
        expect(
            run.stats.job_timeouts == 0,
            prefix + "no healthy or merely slow worker may time out"
        );
        expect(
            run.stats.job_attempts.size() == kJobs,
            prefix + "the coordinator must report one entry per job"
        );

        for (std::size_t i = 0; i < kJobs; ++i) {
            expect(
                run.stats.job_attempts[i] == 1,
                prefix + "every job must be delivered exactly once"
            );
            expect(
                run.stats.job_states[i] == JobState::completed,
                prefix + "every job must finish completed"
            );
        }

        const std::size_t handled = completed_by_worker[0] +
                                    completed_by_worker[1];

        expect(
            handled == kJobs,
            prefix + "workers must account for every completed job"
        );
        expect(
            completed_by_worker[1] > completed_by_worker[0],
            prefix + "the fast worker must handle the majority "
                     "(slow=" + std::to_string(completed_by_worker[0]) +
                     ", fast=" + std::to_string(completed_by_worker[1]) + ")"
        );
        expect(
            completed_by_worker[1] >= 5,
            prefix + "the fast worker must handle at least five of eight jobs"
        );
    }
}

}  // namespace

int main() {
    try {
        test_uneven_job_durations();

        std::cout << "coordinator_uneven_workload_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "coordinator_uneven_workload_test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
