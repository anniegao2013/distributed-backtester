#include "comm/tcp_channel.h"
#include "coordinator/coordinator.h"
#include "experiment/experiment.h"
#include "net/tcp.h"
#include "protocol/protocol.h"
#include "worker/tcp_transport.h"
#include "worker/worker.h"

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
    comm::TcpClientChannel& channel,
    const std::string& worker_id
) {
    WorkerReadyMessage ready;
    ready.message_id = worker_id + ":ready:0001";
    ready.worker_id = worker_id;
    channel.send(worker_ready_to_json(ready, /*pretty=*/false));
}

// A worker that dies while idle must be removed from scheduling. Its queued
// jobs belong to the healthy worker, and the run must still complete.
void test_idle_worker_disconnect() {
    const ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");

    expect(
        request.jobs.size() == 4,
        "the sample experiment must expand to four jobs"
    );

    CoordinatorRun run;
    CoordinatorConfig config;
    run.configure(config);
    config.expected_workers = 2;

    std::thread coordinator([&] { run.run(request, config); });
    ThreadJoiner coordinator_joiner(coordinator);

    const std::uint16_t port = run.wait_for_port();

    // Announce a worker and close its connection before any work is assigned.
    {
        TcpTransport flaky("127.0.0.1", port);
        flaky.connect();
        announce_worker(flaky, "flaky-worker");
    }

    // Let the close reach the coordinator before the healthy worker connects,
    // so dispatch starts with the flaky connection already gone.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::exception_ptr worker_error;
    std::thread worker([&] {
        try {
            TcpTransport transport("127.0.0.1", port);
            WorkerConfig worker_config;
            worker_config.worker_id = "healthy-worker";
            (void)run_worker(transport, worker_config);
        } catch (...) {
            worker_error = std::current_exception();
        }
    });
    ThreadJoiner worker_joiner(worker);

    coordinator.join();
    worker.join();

    if (worker_error) {
        std::rethrow_exception(worker_error);
    }

    if (run.error) {
        std::rethrow_exception(run.error);
    }

    expect(
        run.result.status == "completed",
        "a healthy worker must finish every job, got " + run.result.status
    );
    expect(
        run.result.summary.jobs_completed == request.jobs.size(),
        "every job must complete on the healthy worker"
    );
    expect(
        run.result.summary.jobs_failed == 0,
        "an idle disconnect must not fail any job"
    );
    expect(
        run.stats.workers_registered == 2,
        "both workers must register before dispatch"
    );
    expect(
        run.stats.workers_disconnected == 1,
        "the coordinator must count the lost worker"
    );
    expect(
        run.stats.transport_failures == 1,
        "one transport failure must be recorded"
    );
    expect(
        run.stats.jobs_dispatched == request.jobs.size(),
        "every job must still be dispatched to the healthy worker"
    );

    const std::vector<WorkerDisconnectEvent> disconnects =
        run.take_disconnects();

    expect(disconnects.size() == 1, "exactly one disconnect must be reported");
    expect(
        disconnects[0].worker_id == "flaky-worker",
        "the event must identify the lost worker"
    );
    expect(
        disconnects[0].job_id.empty() && !disconnects[0].job_failed,
        "an idle disconnect must not fail a job"
    );
    expect(
        disconnects[0].reason.find("waiting for work") != std::string::npos ||
            disconnects[0].reason.find("before a job could be assigned") !=
                std::string::npos,
        "the disconnect reason must explain that no job was lost"
    );
}

// If the only worker disappears while idle, the coordinator must not block
// forever waiting for work that cannot run. No job should be marked running,
// and every queued job must still reach a terminal state.
void test_all_workers_disconnect_while_idle() {
    const ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");

    CoordinatorRun run;
    CoordinatorConfig config;
    run.configure(config);
    config.expected_workers = 1;

    std::thread coordinator([&] { run.run(request, config); });
    ThreadJoiner coordinator_joiner(coordinator);

    const std::uint16_t port = run.wait_for_port();

    {
        TcpTransport flaky("127.0.0.1", port);
        flaky.connect();
        announce_worker(flaky, "flaky-worker");
    }

    // Let the close reach the coordinator before dispatch starts.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    coordinator.join();

    if (run.error) {
        std::rethrow_exception(run.error);
    }

    expect(
        run.result.status == "failed",
        "a run with no connected workers must be failed"
    );
    expect(
        run.result.summary.jobs_completed == 0,
        "no job may run without a worker"
    );
    expect(
        run.result.summary.jobs_failed == request.jobs.size(),
        "every queued job must become terminal"
    );
    expect(
        run.stats.jobs_dispatched == 0,
        "no job may be assigned to a disconnected worker"
    );
    expect(
        run.stats.workers_disconnected == 1,
        "the coordinator must count the lost worker"
    );

    for (const ExperimentJobOutcome& outcome : run.result.results) {
        expect(
            outcome.error == "job was not dispatched to a worker",
            "queued jobs must be marked undispatched rather than hang"
        );
    }
}

// A worker that disappears after receiving a job takes that job down with it,
// but the coordinator must detect the loss, finish the remaining jobs, and not
// wait forever for a reply.
void test_worker_crash_during_job() {
    const ExperimentRequest request =
        load_experiment_file("experiments/crossover_sweep.json");

    CoordinatorRun run;
    CoordinatorConfig config;
    run.configure(config);
    config.expected_workers = 1;

    std::thread coordinator([&] { run.run(request, config); });
    ThreadJoiner coordinator_joiner(coordinator);

    const std::uint16_t port = run.wait_for_port();

    TcpTransport crashing("127.0.0.1", port);
    crashing.connect();
    announce_worker(crashing, "crashing-worker");

    const std::optional<std::string> job_text = crashing.receive();
    expect(job_text.has_value(), "the worker must receive a job");

    const BacktestJobMessage job = parse_backtest_job_json(*job_text);

    // Close without sending a result, simulating a crashed process.
    crashing.close();

    coordinator.join();

    if (run.error) {
        std::rethrow_exception(run.error);
    }

    expect(
        run.result.status == "failed",
        "a run with no completed jobs must be failed"
    );
    expect(
        run.result.summary.jobs_completed == 0,
        "the crashed worker must not complete a job"
    );
    expect(
        run.result.summary.jobs_failed == request.jobs.size(),
        "every job must be terminal as failed"
    );
    expect(
        run.stats.workers_disconnected == 1,
        "the coordinator must count the crashed worker"
    );
    expect(
        run.stats.transport_failures == 1,
        "the lost reply must be recorded as a transport failure"
    );
    expect(
        run.stats.jobs_dispatched == 1,
        "only the in-flight job may be dispatched"
    );

    const std::vector<WorkerDisconnectEvent> disconnects =
        run.take_disconnects();

    expect(disconnects.size() == 1, "exactly one disconnect must be reported");
    expect(
        disconnects[0].worker_id == "crashing-worker",
        "the event must identify the crashed worker"
    );
    expect(
        disconnects[0].job_id == job.job.job_id,
        "the event must identify the in-flight job"
    );
    expect(
        !disconnects[0].job_failed,
        "the in-flight job must be requeued instead of failed immediately"
    );
    expect(
        disconnects[0].job_retry_scheduled,
        "the in-flight job must be scheduled for a retry"
    );

    expect(
        run.result.results[0].error ==
            "job was not completed after 1 attempt(s); "
            "no worker remained to retry it",
        "the requeued job must explain why it could not be retried"
    );

    for (std::size_t i = 1; i < run.result.results.size(); ++i) {
        expect(
            run.result.results[i].error ==
                "job was not dispatched to a worker",
            "queued jobs must be marked undispatched"
        );
    }

    expect(
        run.stats.job_states.size() == request.jobs.size(),
        "the coordinator must report a state for every job"
    );

    for (JobState state : run.stats.job_states) {
        expect(state == JobState::failed, "every job must finish failed");
    }
}

}  // namespace

int main() {
    try {
        test_idle_worker_disconnect();
        test_all_workers_disconnect_while_idle();
        test_worker_crash_during_job();

        std::cout << "coordinator_disconnect_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "coordinator_disconnect_test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
