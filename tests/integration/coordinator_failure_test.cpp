#include "comm/tcp_channel.h"
#include "coordinator/coordinator.h"
#include "experiment/experiment.h"
#include "net/tcp.h"
#include "protocol/protocol.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void play_failing_worker(std::uint16_t port) {
    comm::TcpClientChannel channel("127.0.0.1", port);
    channel.connect();

    WorkerReadyMessage ready;
    ready.message_id = "failure-worker:ready:0001";
    ready.worker_id = "failure-worker";
    channel.send(worker_ready_to_json(ready, /*pretty=*/false));

    const std::optional<std::string> job_text = channel.receive();
    expect(job_text.has_value(), "fake worker must receive one job");

    const BacktestJobMessage job = parse_backtest_job_json(*job_text);

    // Report a terminal failure, then disappear. The coordinator must mark the
    // in-flight job failed and finish the still-queued jobs without waiting for
    // this worker to reconnect.
    channel.send(
        worker_error_to_json(
            make_worker_error(
                "execution_error",
                "simulated worker failure",
                "",
                job.message_id,
                ready.worker_id
            ),
            /*pretty=*/false
        )
    );

    channel.close();
}

}  // namespace

int main() {
    try {
        const ExperimentRequest request =
            load_experiment_file("experiments/crossover_sweep.json");

        expect(
            request.jobs.size() == 4,
            "the sample experiment must expand to four jobs"
        );

        CoordinatorConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = 0;
        config.expected_workers = 1;

        std::mutex mutex;
        std::condition_variable condition;
        bool listening = false;
        bool coordinator_done = false;
        std::uint16_t port = 0;
        std::exception_ptr coordinator_error;
        ExperimentResult distributed;
        CoordinatorStats stats;

        config.on_listening = [&](const net::TcpEndpoint& endpoint) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                port = endpoint.port;
                listening = true;
            }
            condition.notify_all();
        };

        std::thread coordinator([&] {
            try {
                distributed =
                    run_distributed_experiment(request, config, &stats);
            } catch (...) {
                coordinator_error = std::current_exception();
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                coordinator_done = true;
            }
            condition.notify_all();
        });

        {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [&] {
                return listening || coordinator_done;
            });
        }

        if (coordinator_error) {
            std::rethrow_exception(coordinator_error);
        }

        expect(listening, "coordinator must report its listening endpoint");

        play_failing_worker(port);
        coordinator.join();

        if (coordinator_error) {
            std::rethrow_exception(coordinator_error);
        }

        expect(
            distributed.status == "failed",
            "a run with no completed jobs must be failed, got " +
                distributed.status
        );
        expect(
            distributed.results.size() == request.jobs.size(),
            "every job must still have exactly one outcome"
        );
        expect(
            distributed.summary.jobs_total == request.jobs.size(),
            "the aggregate must count every terminal job"
        );
        expect(
            distributed.summary.jobs_completed == 0,
            "the fake worker must not complete any job"
        );
        expect(
            distributed.summary.jobs_failed == request.jobs.size(),
            "all jobs must be terminal as failed"
        );

        for (std::size_t i = 0; i < distributed.results.size(); ++i) {
            const ExperimentJobOutcome& outcome = distributed.results[i];

            expect(
                outcome.job.job_id == request.jobs[i].job_id,
                "failed outcomes must stay in request order"
            );
            expect(!outcome.ok, "no job may be reported as successful");
            expect(
                !outcome.error.empty(),
                "every failed outcome must carry a reason"
            );
        }

        const ExperimentJobOutcome& last =
            distributed.results.back();

        expect(
            last.error == "job was not dispatched to a worker",
            "queued jobs must be marked as undispatched"
        );

        std::cout << "coordinator_failure_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "coordinator_failure_test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
