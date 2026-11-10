#pragma once

#include "experiment/experiment.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

// Lifecycle of one job as observed by the coordinator.
//
//   pending    waiting in the coordinator's FIFO job queue
//   running    assigned to a worker and currently executing
//   completed  a successful result was received
//   failed     execution, transport, or result processing failed
//
// Workers report results but do not own job state. The coordinator updates
// this table, making it the single source of truth for every job.
enum class JobState {
    pending,
    running,
    completed,
    failed
};

const char* job_state_name(JobState state) noexcept;

// One committed state change, suitable for logs or progress observers.
struct JobStateTransition {
    std::size_t index = 0;
    std::string job_id;
    JobState from = JobState::pending;
    JobState to = JobState::pending;
};

// Thread-safe state table indexed by ExperimentRequest::jobs position.
//
// Valid transitions are:
//
//   pending -> running -> completed
//                      -> failed
//                      -> pending   (retryable attempt, job requeued)
//   pending -> failed              (no worker was available)
//
// A running job returns to pending only when its failure is retryable and it
// still has attempts left. Terminal states are final. Invalid indexes or
// transitions throw std::out_of_range / std::logic_error.
class JobStateTracker {
public:
    explicit JobStateTracker(const std::vector<ExperimentJob>& jobs);

    JobStateTracker(const JobStateTracker&) = delete;
    JobStateTracker& operator=(const JobStateTracker&) = delete;

    std::size_t size() const noexcept;

    JobState state(std::size_t index) const;
    std::vector<JobState> snapshot() const;
    std::size_t count(JobState state) const;
    bool all_terminal() const;

    JobStateTransition mark_running(std::size_t index);

    // Moves a running job back to pending so it can be dispatched again.
    // Used only when a retryable attempt failed and attempts remain.
    JobStateTransition mark_pending(std::size_t index);

    JobStateTransition mark_completed(std::size_t index);
    JobStateTransition mark_failed(std::size_t index);

    // Marks every pending job failed and returns those transitions in request
    // order. Running and terminal jobs are left untouched.
    std::vector<JobStateTransition> fail_pending();

private:
    JobStateTransition transition(std::size_t index, JobState to);

    std::vector<std::string> job_ids_;
    std::vector<JobState> states_;

    mutable std::mutex mutex_;
};
