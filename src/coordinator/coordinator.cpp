#include "coordinator/coordinator.h"

#include "comm/tcp_channel.h"
#include "coordinator/job_queue.h"
#include "coordinator/job_state.h"
#include "coordinator/persistent_state.h"
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
#include <unordered_set>
#include <utility>
#include <vector>
#include <sys/resource.h>

namespace {

constexpr std::size_t kMaxIdentifierLength = 128;

// How often an idle worker thread wakes up to check whether its connection is
// still alive. This bounds crash detection latency while the queue is empty.
constexpr std::chrono::milliseconds kWorkerLivenessPollInterval{100};

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

// Serializes disconnect notifications from concurrent worker threads.
class WorkerDisconnectObserver {
public:
    explicit WorkerDisconnectObserver(const CoordinatorConfig& config)
        : config_(config) {}

    void publish(const WorkerDisconnectEvent& event) noexcept {
        if (!config_.on_worker_disconnected) {
            return;
        }

        try {
            std::lock_guard<std::mutex> lock(mutex_);
            config_.on_worker_disconnected(event);
        } catch (...) {
            // The worker has already been removed from scheduling and the
            // event is recorded in the stats. An observer must not be able to
            // interrupt the remaining run.
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
    std::size_t index,
    std::size_t attempt
) {
    const std::string sequence = std::to_string(index + 1);
    const std::string retry_suffix =
        attempt > 1
            ? ":retry:" + std::to_string(attempt)
            : std::string();

    const std::string candidate =
        experiment_id + ":" + job_id + ":" + sequence + retry_suffix;

    if (candidate.size() <= kMaxIdentifierLength) {
        return candidate;
    }

    // Keep the correlation key valid even when the experiment and job IDs are
    // close to their individual limits. The sequence still makes the key
    // unique across jobs and the retry suffix makes it unique across attempts.
    return "job:" + sequence + retry_suffix;
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
            make_run_message_id(
                request.experiment_id,
                job.job_id,
                i,
                /*attempt=*/1
            );
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

    // Set by the worker's own thread when the connection is lost. Read by the
    // main thread only after every worker thread has joined.
    bool lost = false;
};

struct SharedState {
    std::atomic<std::size_t> jobs_dispatched{0};
    std::atomic<std::size_t> results_received{0};
    std::atomic<std::size_t> worker_errors{0};
    std::atomic<std::size_t> transport_failures{0};
    std::atomic<std::size_t> workers_disconnected{0};
    std::atomic<std::size_t> job_retries{0};
    std::atomic<std::size_t> duplicate_results{0};
    std::atomic<std::size_t> persistence_errors{0};
};

// Tracks the message_id of every attempt whose reply has been handled. The
// coordinator reads exactly one message per dispatch, so a worker that submits
// the same result twice would otherwise have the extra copy consumed as the
// response to its next job. Replies are keyed by message_id because it is the
// wire correlation key and is unique per attempt within a run.
class AttemptLedger {
public:
    bool handled(const std::string& message_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return handled_.count(message_id) > 0;
    }

    void mark_handled(const std::string& message_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        handled_.insert(message_id);
    }

private:
    mutable std::mutex mutex_;
    std::unordered_set<std::string> handled_;
};

// Thread-safe attempt counter indexed by request order. The queue also carries
// the attempt number, but the coordinator needs a stable per-job view for
// stats and for explaining jobs left pending when every worker is gone.
class AttemptTracker {
public:
    explicit AttemptTracker(std::size_t count) : attempts_(count, 0) {}

    // Restores persisted attempt counts. `initial_attempts` must have one
    // entry per job.
    explicit AttemptTracker(std::vector<std::size_t> initial_attempts)
        : attempts_(std::move(initial_attempts)) {}

    // Records a successful delivery and returns the new attempt number
    // (1 for the first attempt).
    std::size_t record_attempt(std::size_t index) {
        std::lock_guard<std::mutex> lock(mutex_);
        return ++attempts_.at(index);
    }

    std::size_t attempts(std::size_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return attempts_.at(index);
    }

    std::vector<std::size_t> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return attempts_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::size_t> attempts_;
};

// Serializes state-file writes and snapshots the coordinator's three state
// views in a recovery-safe order: states, then attempts, then outcomes. A
// terminal state is always committed after its outcome, so taking the outcomes
// snapshot last means a terminal state never appears in the file without the
// outcome that justifies it. If a state lags an outcome, load() prefers the
// outcome.
class StatePersister {
public:
    StatePersister(
        CoordinatorStateStore* store,
        const JobStateTracker& states,
        const AttemptTracker& attempts,
        const ExperimentResultCollector& results,
        SharedState& shared,
        Logger& logger
    ) : store_(store),
        states_(states),
        attempts_(attempts),
        results_(results),
        shared_(shared),
        logger_(logger) {}

    void persist() {
        if (store_ == nullptr) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        try {
            store_->save(
                CoordinatorStateSnapshot{
                    states_.snapshot(),
                    attempts_.snapshot(),
                    results_.snapshot()
                }
            );
        } catch (const std::exception& error) {
            shared_.persistence_errors.fetch_add(1);
            logger_.log(
                "could not persist coordinator state: " +
                std::string(error.what())
            );
        }
    }

private:
    CoordinatorStateStore* store_;
    const JobStateTracker& states_;
    const AttemptTracker& attempts_;
    const ExperimentResultCollector& results_;
    SharedState& shared_;
    Logger& logger_;
    std::mutex mutex_;
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

void send_job(
    WorkerConnection& worker,
    const BacktestJobMessage& job,
    SharedState& shared
) {
    worker.channel.send(backtest_job_to_json(job, /*pretty=*/false));
    shared.jobs_dispatched.fetch_add(1);
}

// One delivery attempt. The outcome is only terminal when `retryable` is
// false; otherwise the caller may requeue the job while attempts remain.
struct AttemptResult {
    ExperimentJobOutcome outcome;
    bool retryable = false;
};

AttemptResult receive_result(
    WorkerConnection& worker,
    const BacktestJobMessage& job,
    SharedState& shared,
    bool retry_failed_results,
    AttemptLedger& ledger,
    Logger& logger
) {
    for (;;) {
        const std::optional<std::string> reply = worker.channel.receive();

        if (!reply) {
            throw std::runtime_error(
                "connection closed before a result was received"
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
                // A worker may send the same result more than once. The extra
                // copy is sitting between attempts; ignore it instead of
                // mistaking it for the current job's reply. Only an id the
                // coordinator previously handled is a duplicate; an unknown id
                // is still a correlation error.
                if (ledger.handled(message.message_id)) {
                    shared.duplicate_results.fetch_add(1);
                    logger.log(
                        "ignoring duplicate backtest_result for message_id '" +
                        message.message_id + "' from worker '" +
                        worker.worker_id + "'"
                    );
                    continue;
                }

                throw std::runtime_error(
                    "Worker '" + worker.worker_id +
                    "' replied to message_id '" + message.message_id +
                    "' while running '" + job.message_id + "'"
                );
            }

            ledger.mark_handled(job.message_id);
            shared.results_received.fetch_add(1);

            // "invalid" is deterministic worker-side validation and never
            // benefits from another attempt. "failed" is a runtime failure
            // that may be specific to one worker, so it is retryable when
            // enabled.
            const bool retryable =
                message.result.status == "failed" && retry_failed_results;

            return AttemptResult{
                result_to_outcome(job, message),
                retryable
            };
        }

        if (envelope.type == "worker_error" ||
            envelope.type == "protocol_error") {
            const WorkerErrorMessage message =
                parse_worker_error_json(*reply);

            // worker_error may omit message_id when the worker could not even
            // parse the envelope. Such an uncorrelated error can only belong
            // to the attempt currently in flight.
            if (!message.message_id.empty() &&
                message.message_id != job.message_id) {
                if (ledger.handled(message.message_id)) {
                    shared.duplicate_results.fetch_add(1);
                    logger.log(
                        "ignoring duplicate " + envelope.type +
                        " for message_id '" + message.message_id +
                        "' from worker '" + worker.worker_id + "'"
                    );
                    continue;
                }

                throw std::runtime_error(
                    "Worker '" + worker.worker_id +
                    "' replied to message_id '" + message.message_id +
                    "' while running '" + job.message_id + "'"
                );
            }

            ledger.mark_handled(job.message_id);
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

            // A worker_error means no backtest_result is available for this
            // attempt. The job itself remains valid, so another worker can run
            // it.
            return AttemptResult{std::move(outcome), /*retryable=*/true};
        }

        throw std::runtime_error(
            "Worker '" + worker.worker_id +
            "' replied with unexpected message type '" + envelope.type + "'"
        );
    }
}

// Records a lost worker exactly once and notifies observers. The worker is
// removed from scheduling by returning from its loop; any in-flight job has
// already been failed and any undispatched job has already been requeued.
void report_worker_lost(
    WorkerConnection& worker,
    const std::string& reason,
    const std::string& job_id,
    bool job_failed,
    bool job_retry_scheduled,
    SharedState& shared,
    WorkerDisconnectObserver& disconnect_observer,
    Logger& logger
) {
    if (worker.lost) {
        return;
    }

    worker.lost = true;
    shared.workers_disconnected.fetch_add(1);
    shared.transport_failures.fetch_add(1);

    logger.log(
        "worker '" + worker.worker_id + "' disconnected: " + reason
    );

    disconnect_observer.publish(
        WorkerDisconnectEvent{
            worker.worker_id,
            reason,
            job_id,
            job_failed,
            job_retry_scheduled
        }
    );
}

// Returns a failed attempt to the queue while attempts remain. The new attempt
// number is reflected in a fresh correlation key. State moves back to pending
// before the job becomes visible to other workers, so a concurrent mark_running
// always starts from pending.
bool schedule_retry(
    QueuedJob& queued,
    const CoordinatorConfig& config,
    const std::string& reason,
    JobQueue& queue,
    JobStateTracker& states,
    JobStateObserver& state_observer,
    Logger& logger
) {
    if (queued.attempts >= config.max_job_attempts) {
        return false;
    }

    const std::size_t next_attempt = queued.attempts + 1;

    queued.message.message_id = make_run_message_id(
        queued.message.job.experiment_id,
        queued.message.job.job_id,
        queued.index,
        next_attempt
    );

    logger.log(
        "retrying job '" + queued.message.job.job_id + "' after attempt " +
        std::to_string(queued.attempts) + " of " +
        std::to_string(config.max_job_attempts) + ": " + reason
    );

    state_observer.publish(states.mark_pending(queued.index));
    queue.requeue_back(std::move(queued));
    return true;
}

void worker_loop(
    WorkerConnection& worker,
    JobQueue& queue,
    JobStateTracker& states,
    JobStateObserver& state_observer,
    WorkerDisconnectObserver& disconnect_observer,
    ExperimentResultCollector& results,
    SharedState& shared,
    AttemptTracker& attempts,
    AttemptLedger& ledger,
    StatePersister& persister,
    const CoordinatorConfig& config,
    JobGate& gate,
    Logger& logger
) {
    for (;;) {
        gate.acquire();

        // A worker that dies while idle closes its socket, but the coordinator
        // is not reading from it. Check liveness before waiting so the loss is
        // noticed immediately instead of when the next job is assigned.
        if (worker.channel.peer_closed()) {
            gate.release();
            report_worker_lost(
                worker,
                "connection closed while waiting for work",
                "",
                false,
                false,
                shared,
                disconnect_observer,
                logger
            );
            return;
        }

        std::optional<QueuedJob> queued =
            queue.pop_for(kWorkerLivenessPollInterval);

        if (!queued) {
            gate.release();

            // A closed, empty queue is not enough to stop: another worker may
            // still fail a job and requeue it for a retry. Stay available until
            // every job has a terminal outcome.
            if (queue.closed() && results.complete()) {
                return;
            }

            continue;
        }

        // The worker may have dropped between the check above and the pop.
        if (worker.channel.peer_closed()) {
            queue.requeue_front(std::move(*queued));
            gate.release();
            report_worker_lost(
                worker,
                "connection closed before a job could be assigned",
                "",
                false,
                false,
                shared,
                disconnect_observer,
                logger
            );
            return;
        }

        const std::size_t index = queued->index;
        const std::string job_id = queued->message.job.job_id;

        try {
            send_job(worker, queued->message, shared);
        } catch (const std::exception& error) {
            // The job never reached the worker, so no attempt was consumed. It
            // is still pending and can be offered to a healthy worker.
            queue.requeue_front(std::move(*queued));
            gate.release();
            report_worker_lost(
                worker,
                "could not assign work: " + std::string(error.what()),
                "",
                false,
                false,
                shared,
                disconnect_observer,
                logger
            );
            return;
        }

        // The job is running only once it has been handed to the worker. The
        // attempt counter advances only for deliveries that actually happened.
        queued->attempts = attempts.record_attempt(index);

        if (queued->attempts > 1) {
            shared.job_retries.fetch_add(1);
        }

        logger.log(
            "assigning job '" + job_id + "' to worker '" +
            worker.worker_id + "' (attempt " +
            std::to_string(queued->attempts) + " of " +
            std::to_string(config.max_job_attempts) + ")"
        );
        state_observer.publish(states.mark_running(index));
        persister.persist();

        try {
            AttemptResult attempt =
                receive_result(
                    worker,
                    queued->message,
                    shared,
                    config.retry_failed_results,
                    ledger,
                    logger
                );

            const bool completed = attempt.outcome.ok;

            logger.log(
                completed
                    ? "worker '" + worker.worker_id + "' completed job '" +
                          job_id + "'"
                    : "worker '" + worker.worker_id + "' failed job '" +
                          job_id + "'"
            );

            if (attempt.retryable &&
                schedule_retry(
                    *queued,
                    config,
                    attempt.outcome.error,
                    queue,
                    states,
                    state_observer,
                    logger
                )) {
                // The job is back in the queue with a higher attempt count.
                // Persist that before another worker can claim it.
                persister.persist();
                gate.release();
                continue;
            }

            // The outcome is terminal: the attempt succeeded, the failure is
            // non-retryable, or the attempt limit has been reached. The
            // collector keeps the first outcome if a duplicate slips through
            // some other path, so never publish a second terminal transition.
            if (results.record(index, std::move(attempt.outcome))) {
                state_observer.publish(
                    completed
                        ? states.mark_completed(index)
                        : states.mark_failed(index)
                );
                persister.persist();
            } else {
                shared.duplicate_results.fetch_add(1);
                logger.log(
                    "ignoring duplicate terminal outcome for job '" +
                    job_id + "'"
                );
            }

            gate.release();
        } catch (const std::exception& error) {
            // The job was delivered but its reply is gone, or the reply could
            // not be correlated. The worker is removed from scheduling. The
            // job returns to the queue when attempts remain.
            const bool retry_scheduled = schedule_retry(
                *queued,
                config,
                error.what(),
                queue,
                states,
                state_observer,
                logger
            );

            if (!retry_scheduled) {
                if (results.record(
                        index,
                        failed_outcome(queued->message.job, error.what())
                    )) {
                    state_observer.publish(states.mark_failed(index));
                    persister.persist();
                } else {
                    shared.duplicate_results.fetch_add(1);
                }
            } else {
                persister.persist();
            }

            gate.release();
            report_worker_lost(
                worker,
                "lost while running job '" + job_id + "': " +
                    error.what(),
                job_id,
                !retry_scheduled,
                retry_scheduled,
                shared,
                disconnect_observer,
                logger
            );
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

    if (config.max_job_attempts == 0) {
        throw std::invalid_argument(
            "Coordinator requires at least one attempt per job"
        );
    }

    Logger logger(config);

    // Durable state, when enabled, is loaded before any socket is opened so a
    // malformed or mismatched file fails the run without leaving a listener
    // behind.
    std::optional<CoordinatorStateStore> state_store;
    CoordinatorStateSnapshot snapshot;
    CoordinatorRecovery recovery;

    if (config.state_path.empty()) {
        snapshot.states.assign(request.jobs.size(), JobState::pending);
        snapshot.attempts.assign(request.jobs.size(), 0);
        snapshot.outcomes.assign(request.jobs.size(), std::nullopt);
    } else {
        state_store.emplace(config.state_path, request);
        logger.log("loading coordinator state from " + config.state_path);
        recovery = state_store->load(snapshot);

        if (recovery.recovered) {
            logger.log(
                "recovered " + std::to_string(recovery.jobs_completed) +
                " completed and " + std::to_string(recovery.jobs_failed) +
                " failed job(s); requeued " +
                std::to_string(recovery.jobs_requeued) +
                " interrupted job(s)"
            );
        }
    }

    // A job whose result was unknown when the previous coordinator stopped is
    // back to pending. If its persisted attempt count already reached the
    // policy limit, no worker may run it again, so fail it during recovery
    // instead of queueing work that cannot be delivered.
    std::size_t jobs_exhausted = 0;

    for (std::size_t i = 0; i < request.jobs.size(); ++i) {
        if (snapshot.outcomes[i] ||
            snapshot.attempts[i] < config.max_job_attempts) {
            continue;
        }

        const std::string reason =
            "job was interrupted by a coordinator restart after " +
            std::to_string(snapshot.attempts[i]) +
            " attempt(s); no attempts remain";

        snapshot.outcomes[i] = failed_outcome(request.jobs[i], reason);
        snapshot.states[i] = JobState::failed;
        ++jobs_exhausted;

        logger.log("job '" + request.jobs[i].job_id + "' " + reason);
    }

    SharedState shared;

    // The coordinator owns job state. Both local threads and remote workers
    // report through the same collector, while the tracker records the
    // pending -> running -> pending/completed/failed lifecycle.
    JobStateTracker states(request.jobs, snapshot.states);
    JobStateObserver state_observer(config);
    WorkerDisconnectObserver disconnect_observer(config);
    AttemptTracker attempts(snapshot.attempts);
    AttemptLedger ledger;
    ExperimentResultCollector results(request.jobs);

    for (std::size_t i = 0; i < snapshot.outcomes.size(); ++i) {
        if (snapshot.outcomes[i]) {
            results.record(i, std::move(*snapshot.outcomes[i]));
        }
    }

    StatePersister persister(
        state_store ? &*state_store : nullptr,
        states,
        attempts,
        results,
        shared,
        logger
    );

    // Write the normalized startup view (running -> pending, exhausted ->
    // failed) before accepting any work.
    persister.persist();

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

    const std::size_t max_concurrent =
        choose_max_concurrent(request, config.expected_workers);

    auto publish_stats = [&](std::size_t workers_registered) {
        if (stats == nullptr) {
            return;
        }

        stats->workers_registered = workers_registered;
        stats->jobs_dispatched = shared.jobs_dispatched.load();
        stats->results_received = shared.results_received.load();
        stats->worker_errors = shared.worker_errors.load();
        stats->transport_failures = shared.transport_failures.load();
        stats->workers_disconnected = shared.workers_disconnected.load();
        stats->duplicate_results = shared.duplicate_results.load();
        stats->job_retries = shared.job_retries.load();
        stats->state_recovered = recovery.recovered;
        stats->jobs_recovered =
            recovery.jobs_completed + recovery.jobs_failed;
        stats->jobs_requeued_after_restart = recovery.jobs_requeued;
        stats->jobs_exhausted_after_restart = jobs_exhausted;
        stats->persistence_errors = shared.persistence_errors.load();
        stats->job_attempts = attempts.snapshot();
        stats->job_states = states.snapshot();
    };

    // Every job already has a terminal outcome: this experiment finished in an
    // earlier process, so return the aggregated result without binding a
    // socket or waiting for workers.
    if (results.complete()) {
        const auto wall_start = std::chrono::steady_clock::now();
        const double cpu_start = process_cpu_seconds();

        const double wall_seconds =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - wall_start
            ).count();
        const double cpu_seconds = process_cpu_seconds() - cpu_start;

        std::vector<ExperimentJobOutcome> outcomes = results.take();

        publish_stats(/*workers_registered=*/0);

        return build_experiment_result(
            request,
            data_summary,
            std::move(outcomes),
            wall_seconds,
            cpu_seconds,
            max_concurrent
        );
    }

    // Bind now so workers can start and connect while the coordinator is still
    // preparing the run. Connections queue in the listen backlog.
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

    // Only pending jobs enter the FIFO queue. Recovered terminal jobs keep the
    // outcomes restored above; recovered running jobs were normalized to
    // pending and carry their attempt count forward.
    JobQueue queue;

    std::vector<BacktestJobMessage> messages = build_job_messages(request);
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (states.state(i) != JobState::pending) {
            continue;
        }

        const std::size_t previous_attempts = attempts.attempts(i);

        // A resumed job's next delivery is attempt previous_attempts + 1, so
        // its correlation key must match that attempt.
        messages[i].message_id = make_run_message_id(
            request.experiment_id,
            request.jobs[i].job_id,
            i,
            previous_attempts + 1
        );

        queue.push(
            QueuedJob{i, std::move(messages[i]), previous_attempts}
        );
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
            std::ref(disconnect_observer),
            std::ref(results),
            std::ref(shared),
            std::ref(attempts),
            std::ref(ledger),
            std::ref(persister),
            std::ref(config),
            std::ref(gate),
            std::ref(logger)
        );
    }

    for (std::thread& thread : threads) {
        thread.join();
    }

    // A job is terminal when it has completed or failed. If every worker
    // stopped while jobs were still queued, those jobs can no longer run, so
    // record them as failed before declaring the experiment complete. A job
    // that was requeued for a retry but never picked up again gets a message
    // that reflects the attempts already made.
    const std::size_t abandoned = results.fail_remaining(
        [&attempts](std::size_t index) {
            const std::size_t made = attempts.attempts(index);

            if (made == 0) {
                return std::string("job was not dispatched to a worker");
            }

            return "job was not completed after " +
                   std::to_string(made) +
                   " attempt(s); no worker remained to retry it";
        }
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
            " job(s) were not completed because no worker remained"
        );
    }

    // Every job is terminal now. Persist once more so a relaunch sees the
    // final outcomes and does not wait for workers that are already gone.
    persister.persist();

    results.wait();

    const auto wall_end = std::chrono::steady_clock::now();
    const double cpu_end = process_cpu_seconds();

    std::vector<ExperimentJobOutcome> outcomes = results.take();

    for (std::size_t i = 0; i < workers.size(); ++i) {
        // A lost worker has already been logged and counted; do not report the
        // same failure again while shutting down.
        if (!workers[i].lost) {
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
                // The worker disappeared after its last result. It has no work
                // left, but the loss is still real and worth recording.
                report_worker_lost(
                    workers[i],
                    "could not send shutdown: " +
                        std::string(error.what()),
                    "",
                    false,
                    false,
                    shared,
                    disconnect_observer,
                    logger
                );
            }
        }

        workers[i].channel.close();
    }

    const double wall_seconds =
        std::chrono::duration<double>(wall_end - wall_start).count();

    if (stats != nullptr) {
        publish_stats(workers.size());
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
