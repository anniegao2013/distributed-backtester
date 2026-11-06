#include "coordinator/coordinator.h"

#include "comm/tcp_channel.h"
#include "coordinator/job_queue.h"
#include "coordinator/job_state.h"
#include "core/market_data.h"
#include "experiment/result_collector.h"
#include "protocol/protocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <sys/resource.h>

namespace {

constexpr std::size_t kMaxIdentifierLength = 128;

// Serializes log lines so concurrent worker threads cannot interleave them.
class Logger {
public:
    explicit Logger(const CoordinatorConfig& config) : config_(config) {}

    void log(const std::string& text) {
        if (!config_.log) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        config_.log(text);
    }

private:
    const CoordinatorConfig& config_;
    std::mutex mutex_;
};

// Serializes job-state callbacks from concurrent worker threads. State changes
// are committed by JobStateTracker before the callback runs, so an observer
// never sees a partially applied transition.
class JobStateObserver {
public:
    explicit JobStateObserver(const CoordinatorConfig& config)
        : config_(config) {}

    void publish(const JobStateTransition& transition) noexcept {
        if (!config_.on_job_state_changed) {
            return;
        }

        try {
            std::lock_guard<std::mutex> lock(mutex_);
            config_.on_job_state_changed(transition);
        } catch (...) {
            // Observers are read-only: a throwing callback must not be able to
            // leave the coordinator's state table inconsistent or kill a
            // worker thread. The transition is already committed.
        }
    }

private:
    const CoordinatorConfig& config_;
    std::mutex mutex_;
};

double process_cpu_seconds() {
    struct rusage usage;

    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }

    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
           usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
}

std::string make_run_message_id(
    const std::string& experiment_id,
    const std::string& job_id,
    std::size_t index
) {
    const std::string sequence = std::to_string(index + 1);
    const std::string candidate =
        experiment_id + ":" + job_id + ":" + sequence;

    if (candidate.size() <= kMaxIdentifierLength) {
        return candidate;
    }

    // Keep the correlation key valid even when the experiment and job IDs are
    // close to their individual limits.
    return "job:" + sequence;
}

std::string make_shutdown_message_id(
    const std::string& experiment_id,
    std::size_t index
) {
    const std::string sequence = std::to_string(index + 1);
    const std::string candidate =
        experiment_id + ":shutdown:" + sequence;

    if (candidate.size() <= kMaxIdentifierLength) {
        return candidate;
    }

    return "shutdown:" + sequence;
}

std::vector<BacktestJobMessage> build_job_messages(
    const ExperimentRequest& request
) {
    std::vector<BacktestJobMessage> messages;
    messages.reserve(request.jobs.size());

    for (std::size_t i = 0; i < request.jobs.size(); ++i) {
        const ExperimentJob& job = request.jobs[i];

        BacktestJobMessage message;
        message.message_id =
            make_run_message_id(request.experiment_id, job.job_id, i);
        message.job.experiment_id = request.experiment_id;
        message.job.job_id = job.job_id;
        message.job.strategy_id = job.strategy_id;
        message.job.strategy_type = job.strategy_type;
        message.job.parameters = job.parameters;
        message.job.data = {
            request.data.path,
            request.data.asset,
            request.data.start_date,
            request.data.end_date
        };
        message.job.execution = {
            request.execution.initial_cash,
            request.execution.transaction_cost_rate,
            request.execution.slippage_rate
        };
        message.job.output.include_trades =
            request.output.include_trades;

        messages.push_back(std::move(message));
    }

    return messages;
}

ExperimentJobOutcome failed_outcome(
    const ExperimentJob& job,
    const std::string& error
) {
    ExperimentJobOutcome outcome;
    outcome.job = job;
    outcome.error = error.empty()
        ? "worker did not produce a result"
        : error;
    return outcome;
}

ExperimentJobOutcome failed_outcome(
    const BacktestJob& job,
    const std::string& error
) {
    return failed_outcome(
        ExperimentJob{
            job.job_id,
            job.strategy_id,
            job.strategy_type,
            job.parameters
        },
        error
    );
}

ExperimentJobOutcome result_to_outcome(
    const BacktestJobMessage& job,
    const BacktestResultMessage& message
) {
    ExperimentJobOutcome outcome;
    outcome.job = {
        job.job.job_id,
        job.job.strategy_id,
        job.job.strategy_type,
        job.job.parameters
    };
    outcome.duration_seconds = message.result.duration_seconds;
    outcome.ok = message.result.status == "completed";

    if (outcome.ok) {
        outcome.result.job_id = message.result.job_id;
        outcome.result.asset = job.job.data.asset;
        outcome.result.parameters = message.result.parameters;

        if (message.result.metrics) {
            outcome.result.metrics = *message.result.metrics;
        }

        if (message.result.trades) {
            outcome.result.trades = *message.result.trades;
        }

        return outcome;
    }

    if (message.result.error && !message.result.error->message.empty()) {
        outcome.error = message.result.error->message;
    } else {
        outcome.error = "Backtest " + message.result.status;
    }

    return outcome;
}

struct WorkerConnection {
    comm::TcpChannel channel;
    std::string worker_id;
};

struct SharedState {
    std::atomic<std::size_t> jobs_dispatched{0};
    std::atomic<std::size_t> results_received{0};
    std::atomic<std::size_t> worker_errors{0};
    std::atomic<std::size_t> transport_failures{0};
};

// Bounds the number of jobs in flight independently of the number of
// connected workers, so execution.max_concurrent_jobs is honoured.
class JobGate {
public:
    explicit JobGate(std::size_t permits) : available_(permits) {}

    void acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return available_ > 0; });
        --available_;
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++available_;
        }
        condition_.notify_one();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t available_;
};

ExperimentJobOutcome dispatch_one(
    WorkerConnection& worker,
    const BacktestJobMessage& job,
    SharedState& shared
) {
    worker.channel.send(backtest_job_to_json(job, /*pretty=*/false));
    shared.jobs_dispatched.fetch_add(1);

    const std::optional<std::string> reply = worker.channel.receive();

    if (!reply) {
        throw std::runtime_error(
            "Worker '" + worker.worker_id +
            "' closed the connection while running job " + job.job.job_id
        );
    }

    const ProtocolEnvelope envelope = parse_protocol_envelope(*reply);

    if (envelope.protocol_version != kBacktestProtocolVersion) {
        throw std::runtime_error(
            "Worker '" + worker.worker_id +
            "' replied with unsupported protocol_version " +
            std::to_string(envelope.protocol_version)
        );
    }

    if (envelope.type == "backtest_result") {
        const BacktestResultMessage message =
            parse_backtest_result_json(*reply);

        if (message.message_id != job.message_id) {
            throw std::runtime_error(
                "Worker '" + worker.worker_id +
                "' replied to message_id '" + message.message_id +
                "' while running '" + job.message_id + "'"
            );
        }

        shared.results_received.fetch_add(1);
        return result_to_outcome(job, message);
    }

    if (envelope.type == "worker_error" ||
        envelope.type == "protocol_error") {
        const WorkerErrorMessage message =
            parse_worker_error_json(*reply);

        shared.worker_errors.fetch_add(1);

        ExperimentJobOutcome outcome;
        outcome.job = {
            job.job.job_id,
            job.job.strategy_id,
            job.job.strategy_type,
            job.job.parameters
        };
        outcome.error = message.error.message.empty()
            ? "Worker '" + worker.worker_id + "' reported " +
                  message.error.type
            : message.error.message;
        return outcome;
    }

    throw std::runtime_error(
        "Worker '" + worker.worker_id +
        "' replied with unexpected message type '" + envelope.type + "'"
    );
}

void worker_loop(
    WorkerConnection& worker,
    JobQueue& queue,
    JobStateTracker& states,
    JobStateObserver& state_observer,
    ExperimentResultCollector& results,
    SharedState& shared,
    JobGate& gate,
    Logger& logger
) {
    for (;;) {
        gate.acquire();

        std::optional<QueuedJob> queued = queue.pop();

        // A nullopt means the queue is closed and empty: every job has
        // already been handed to a worker.
        if (!queued) {
            gate.release();
            return;
        }

        const std::size_t index = queued->index;
        const BacktestJobMessage& job = queued->message;

        // The coordinator transitions the job before handing it to the
        // worker, so "running" means assigned to a specific worker.
        state_observer.publish(states.mark_running(index));

        logger.log(
            "assigning job '" + job.job.job_id + "' to worker '" +
            worker.worker_id + "'"
        );

        try {
            ExperimentJobOutcome outcome =
                dispatch_one(worker, job, shared);
            const bool completed = outcome.ok;

            logger.log(
                completed
                    ? "worker '" + worker.worker_id + "' completed job '" +
                          job.job.job_id + "'"
                    : "worker '" + worker.worker_id + "' failed job '" +
                          job.job.job_id + "'"
            );

            results.record(index, std::move(outcome));
            state_observer.publish(
                completed
                    ? states.mark_completed(index)
                    : states.mark_failed(index)
            );
            gate.release();
        } catch (const std::exception& error) {
            results.record(
                index,
                failed_outcome(job.job, error.what())
            );
            state_observer.publish(states.mark_failed(index));
            shared.transport_failures.fetch_add(1);
            gate.release();

            logger.log(
                "worker '" + worker.worker_id + "' stopped after job '" +
                job.job.job_id + "': " + error.what()
            );

            // The connection is no longer usable. Remaining jobs stay in the
            // queue for the other workers.
            return;
        }
    }
}

std::size_t choose_max_concurrent(
    const ExperimentRequest& request,
    std::size_t workers
) {
    const std::size_t hardware_threads = std::max<std::size_t>(
        1,
        std::thread::hardware_concurrency()
    );
    const std::size_t requested =
        request.execution.max_concurrent_jobs > 0
            ? request.execution.max_concurrent_jobs
            : hardware_threads;

    return std::max<std::size_t>(
        1,
        std::min(requested, workers)
    );
}

}  // namespace

ExperimentResult run_distributed_experiment(
    const ExperimentRequest& request,
    const CoordinatorConfig& config,
    CoordinatorStats* stats
) {
    if (request.jobs.empty()) {
        throw std::invalid_argument(
            "Experiment has no jobs; call finalize_experiment() first"
        );
    }

    if (config.expected_workers == 0) {
        throw std::invalid_argument(
            "Coordinator requires at least one expected worker"
        );
    }

    Logger logger(config);

    // Bind first so workers can start and connect while the coordinator is
    // still preparing the run. Connections queue in the listen backlog.
    net::TcpListener listener = net::TcpListener::bind(
        config.listen_host,
        config.listen_port,
        static_cast<int>(
            std::max<std::size_t>(1, config.expected_workers)
        )
    );

    const net::TcpEndpoint bound{
        config.listen_host,
        listener.port()
    };

    logger.log(
        "listening for workers on " + net::format_tcp_endpoint(bound)
    );

    if (config.on_listening) {
        config.on_listening(bound);
    }

    // The coordinator needs the same data summary a local run reports. The
    // dataset is also the source of truth for the worker's date range.
    logger.log("loading market data from " + request.data.path);

    const std::vector<MarketBar> dataset =
        load_market_data(request.data.path);

    if (dataset.empty()) {
        throw std::runtime_error(
            "Market data file '" + request.data.path + "' is empty"
        );
    }

    const ExperimentDataSummary data_summary{
        dataset.size(),
        dataset.front().date,
        dataset.back().date
    };

    std::vector<WorkerConnection> workers;
    workers.reserve(config.expected_workers);

    while (workers.size() < config.expected_workers) {
        logger.log(
            "waiting for worker " + std::to_string(workers.size() + 1) +
            " of " + std::to_string(config.expected_workers)
        );

        comm::TcpChannel channel(listener.accept());
        const std::optional<std::string> announcement = channel.receive();

        if (!announcement) {
            throw std::runtime_error(
                "Worker closed the connection before sending worker_ready"
            );
        }

        WorkerReadyMessage ready;

        try {
            ready = parse_worker_ready_json(*announcement);
        } catch (const std::exception& error) {
            throw std::runtime_error(
                std::string("Invalid worker_ready message: ") +
                error.what()
            );
        }

        for (const WorkerConnection& existing : workers) {
            if (existing.worker_id == ready.worker_id) {
                throw std::runtime_error(
                    "Duplicate worker_id '" + ready.worker_id + "'"
                );
            }
        }

        logger.log(
            "worker '" + ready.worker_id + "' connected"
        );

        workers.push_back(
            WorkerConnection{std::move(channel), ready.worker_id}
        );
    }

    listener.close();

    const std::size_t max_concurrent =
        choose_max_concurrent(request, workers.size());

    SharedState shared;

    // The coordinator owns job state. Both local threads and remote workers
    // report through the same collector, while the tracker records the
    // pending -> running -> completed/failed lifecycle.
    JobStateTracker states(request.jobs);
    JobStateObserver state_observer(config);

    ExperimentResultCollector results(request.jobs);

    // Every expanded job enters the FIFO queue once, in request order. All jobs
    // are enqueued before any worker thread starts, so the queue is then closed
    // to signal that no further work will arrive.
    JobQueue queue;

    std::vector<BacktestJobMessage> messages = build_job_messages(request);
    for (std::size_t i = 0; i < messages.size(); ++i) {
        queue.push(QueuedJob{i, std::move(messages[i])});
    }
    queue.close();

    const auto wall_start = std::chrono::steady_clock::now();
    const double cpu_start = process_cpu_seconds();

    JobGate gate(max_concurrent);

    std::vector<std::thread> threads;
    threads.reserve(workers.size());

    for (WorkerConnection& worker : workers) {
        threads.emplace_back(
            worker_loop,
            std::ref(worker),
            std::ref(queue),
            std::ref(states),
            std::ref(state_observer),
            std::ref(results),
            std::ref(shared),
            std::ref(gate),
            std::ref(logger)
        );
    }

    for (std::thread& thread : threads) {
        thread.join();
    }

    // A job is terminal when it has completed or failed. If every worker
    // stopped while jobs were still queued, those jobs can no longer run, so
    // record them as failed before declaring the experiment complete.
    const std::size_t abandoned = results.fail_remaining(
        "job was not dispatched to a worker"
    );
    const std::vector<JobStateTransition> pending_failures =
        states.fail_pending();

    for (const JobStateTransition& transition : pending_failures) {
        state_observer.publish(transition);
    }

    if (abandoned != pending_failures.size()) {
        throw std::logic_error(
            "coordinator state mismatch while terminating pending jobs"
        );
    }

    if (abandoned > 0) {
        logger.log(
            std::to_string(abandoned) +
            " job(s) were not dispatched to a worker"
        );
    }

    results.wait();

    const auto wall_end = std::chrono::steady_clock::now();
    const double cpu_end = process_cpu_seconds();

    std::vector<ExperimentJobOutcome> outcomes = results.take();

    for (std::size_t i = 0; i < workers.size(); ++i) {
        ShutdownMessage shutdown;
        shutdown.message_id =
            make_shutdown_message_id(request.experiment_id, i);
        shutdown.reason = "experiment run complete";

        try {
            workers[i].channel.send(
                shutdown_to_json(shutdown, /*pretty=*/false)
            );
            logger.log(
                "sent shutdown to worker '" + workers[i].worker_id + "'"
            );
        } catch (const std::exception& error) {
            logger.log(
                "could not send shutdown to worker '" +
                workers[i].worker_id + "': " + error.what()
            );
        }

        workers[i].channel.close();
    }

    const double wall_seconds =
        std::chrono::duration<double>(wall_end - wall_start).count();

    if (stats != nullptr) {
        stats->workers_registered = workers.size();
        stats->jobs_dispatched = shared.jobs_dispatched.load();
        stats->results_received = shared.results_received.load();
        stats->worker_errors = shared.worker_errors.load();
        stats->transport_failures = shared.transport_failures.load();
        stats->job_states = states.snapshot();
    }

    return build_experiment_result(
        request,
        data_summary,
        std::move(outcomes),
        wall_seconds,
        cpu_end - cpu_start,
        max_concurrent
    );
}
