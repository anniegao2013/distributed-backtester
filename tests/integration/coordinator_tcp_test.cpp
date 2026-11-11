#include "coordinator/coordinator.h"
#include "experiment/experiment.h"
#include "net/tcp.h"
#include "worker/tcp_transport.h"
#include "worker/worker.h"

#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
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

void expect_metrics_match(
    const BacktestMetrics& distributed,
    const BacktestMetrics& local,
    const std::string& job_id
) {
    // Values travel as JSON doubles, so only round-off can differ.
    expect(
        std::abs(distributed.total_return - local.total_return) < 1e-12,
        "total_return mismatch for " + job_id
    );
    expect(
        std::abs(distributed.sharpe_ratio - local.sharpe_ratio) < 1e-12,
        "sharpe_ratio mismatch for " + job_id
    );
    expect(
        std::abs(distributed.max_drawdown - local.max_drawdown) < 1e-12,
        "max_drawdown mismatch for " + job_id
    );
    expect(
        distributed.number_of_trades == local.number_of_trades,
        "number_of_trades mismatch for " + job_id
    );
    expect(
        std::abs(distributed.total_costs - local.total_costs) < 1e-9,
        "total_costs mismatch for " + job_id
    );
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

        const ExperimentResult local = run_experiment(request);

        CoordinatorConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = 0;
        config.expected_workers = 2;

        std::mutex mutex;
        std::condition_variable condition;
        bool listening = false;
        bool coordinator_done = false;
        std::uint16_t port = 0;
        std::exception_ptr coordinator_error;
        ExperimentResult distributed;
        CoordinatorStats stats;

        std::vector<std::vector<JobStateTransition>> transitions(
            request.jobs.size()
        );
        std::mutex transition_mutex;

        config.on_job_state_changed = [&](const JobStateTransition& change) {
            std::lock_guard<std::mutex> lock(transition_mutex);
            transitions.at(change.index).push_back(change);
        };

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

        std::vector<std::thread> workers;
        std::vector<std::exception_ptr> worker_errors(2);

        for (std::size_t i = 0; i < worker_errors.size(); ++i) {
            workers.emplace_back([&, i] {
                try {
                    TcpTransport transport("127.0.0.1", port);

                    WorkerConfig worker_config;
                    worker_config.worker_id =
                        "worker-" + std::to_string(i + 1);
                    worker_config.log = [](const std::string& line) {
                        std::cerr << "[worker] " << line << '\n';
                    };

                    (void)run_worker(transport, worker_config);
                } catch (...) {
                    worker_errors[i] = std::current_exception();
                }
            });
        }

        coordinator.join();

        for (std::thread& worker : workers) {
            worker.join();
        }

        if (coordinator_error) {
            std::rethrow_exception(coordinator_error);
        }

        for (const std::exception_ptr& error : worker_errors) {
            if (error) {
                std::rethrow_exception(error);
            }
        }

        expect(
            distributed.status == "completed",
            "distributed run must complete, got " + distributed.status
        );
        expect(
            stats.workers_registered == 2,
            "coordinator must register both workers"
        );
        expect(
            stats.jobs_dispatched == request.jobs.size(),
            "coordinator must dispatch every job"
        );
        expect(
            stats.results_received == request.jobs.size(),
            "coordinator must receive one result per job"
        );
        expect(
            stats.worker_errors == 0,
            "workers must not report protocol errors"
        );
        expect(
            stats.transport_failures == 0,
            "no worker connection should fail"
        );
        expect(
            stats.workers_disconnected == 0,
            "every worker must stay connected for the whole run"
        );
        expect(
            distributed.results.size() == request.jobs.size(),
            "distributed run must return one outcome per job"
        );
        expect(
            distributed.summary.jobs_completed == request.jobs.size(),
            "every distributed job must complete"
        );
        expect(
            distributed.summary.jobs_failed == 0,
            "no distributed job may fail"
        );
        expect(
            !distributed.summary.best_job_id.empty(),
            "the aggregate must select a best job"
        );
        expect(
            stats.job_states.size() == request.jobs.size(),
            "the coordinator must report a state for every job"
        );

        for (std::size_t i = 0; i < request.jobs.size(); ++i) {
            expect(
                stats.job_states[i] == JobState::completed,
                "every distributed job must finish completed"
            );

            const std::vector<JobStateTransition>& job_transitions =
                transitions[i];

            expect(
                job_transitions.size() == 2,
                "a completed job must move pending -> running -> completed"
            );
            expect(
                job_transitions[0].from == JobState::pending &&
                    job_transitions[0].to == JobState::running,
                "the first transition must enter running"
            );
            expect(
                job_transitions[1].from == JobState::running &&
                    job_transitions[1].to == JobState::completed,
                "the second transition must enter completed"
            );
            expect(
                job_transitions[0].job_id == request.jobs[i].job_id,
                "state transitions must identify the request job"
            );
        }

        for (std::size_t i = 0; i < distributed.results.size(); ++i) {
            const ExperimentJobOutcome& outcome = distributed.results[i];

            expect(
                outcome.job.job_id == request.jobs[i].job_id,
                "distributed outcomes must stay in request order"
            );
            expect(outcome.ok, "distributed job must succeed");
            expect_metrics_match(
                outcome.result.metrics,
                local.results[i].result.metrics,
                outcome.job.job_id
            );
        }

        std::cout << "coordinator_tcp_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "coordinator_tcp_test failed: " << error.what() << '\n';
        return 1;
    }
}
