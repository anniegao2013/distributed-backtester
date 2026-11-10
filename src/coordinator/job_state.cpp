#include "coordinator/job_state.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace {

bool can_transition(JobState from, JobState to) {
    switch (to) {
    case JobState::pending:
        // Only a running job can be returned to the queue for a retry.
        return from == JobState::running;

    case JobState::running:
        return from == JobState::pending;

    case JobState::completed:
        return from == JobState::running;

    case JobState::failed:
        return from == JobState::pending || from == JobState::running;
    }

    return false;
}

}  // namespace

const char* job_state_name(JobState state) noexcept {
    switch (state) {
    case JobState::pending:
        return "pending";
    case JobState::running:
        return "running";
    case JobState::completed:
        return "completed";
    case JobState::failed:
        return "failed";
    }

    return "unknown";
}

JobStateTracker::JobStateTracker(const std::vector<ExperimentJob>& jobs)
    : states_(jobs.size(), JobState::pending) {
    job_ids_.reserve(jobs.size());

    for (const ExperimentJob& job : jobs) {
        job_ids_.push_back(job.job_id);
    }
}

std::size_t JobStateTracker::size() const noexcept {
    return states_.size();
}

JobState JobStateTracker::state(std::size_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (index >= states_.size()) {
        throw std::out_of_range(
            "job index " + std::to_string(index) +
            " is outside the experiment"
        );
    }

    return states_[index];
}

std::vector<JobState> JobStateTracker::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return states_;
}

std::size_t JobStateTracker::count(JobState state) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(
        std::count(states_.begin(), states_.end(), state)
    );
}

bool JobStateTracker::all_terminal() const {
    std::lock_guard<std::mutex> lock(mutex_);

    return std::all_of(
        states_.begin(),
        states_.end(),
        [](JobState state) {
            return state == JobState::completed ||
                   state == JobState::failed;
        }
    );
}

JobStateTransition JobStateTracker::mark_running(std::size_t index) {
    return transition(index, JobState::running);
}

JobStateTransition JobStateTracker::mark_pending(std::size_t index) {
    return transition(index, JobState::pending);
}

JobStateTransition JobStateTracker::mark_completed(std::size_t index) {
    return transition(index, JobState::completed);
}

JobStateTransition JobStateTracker::mark_failed(std::size_t index) {
    return transition(index, JobState::failed);
}

std::vector<JobStateTransition> JobStateTracker::fail_pending() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<JobStateTransition> transitions;
    transitions.reserve(states_.size());

    for (std::size_t i = 0; i < states_.size(); ++i) {
        if (states_[i] != JobState::pending) {
            continue;
        }

        const JobState from = states_[i];
        states_[i] = JobState::failed;

        transitions.push_back(
            JobStateTransition{
                i,
                job_ids_[i],
                from,
                JobState::failed
            }
        );
    }

    return transitions;
}

JobStateTransition JobStateTracker::transition(
    std::size_t index,
    JobState to
) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (index >= states_.size()) {
        throw std::out_of_range(
            "job index " + std::to_string(index) +
            " is outside the experiment"
        );
    }

    const JobState from = states_[index];

    if (!can_transition(from, to)) {
        throw std::logic_error(
            "job '" + job_ids_[index] + "' cannot transition from '" +
            job_state_name(from) + "' to '" + job_state_name(to) + "'"
        );
    }

    states_[index] = to;

    return JobStateTransition{
        index,
        job_ids_[index],
        from,
        to
    };
}
