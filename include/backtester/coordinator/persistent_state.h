#pragma once

#include "coordinator/job_state.h"
#include "experiment/experiment.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// Durable coordinator-owned job state.
//
// The coordinator can checkpoint its state table, per-job attempt counts, and
// terminal outcomes to one JSON document. A later coordinator process pointed
// at the same path resumes from that document instead of starting over:
//
//   - completed and failed jobs keep their persisted outcomes;
//   - jobs that were running when the previous process stopped are returned to
//     pending, because their result may never have been produced;
//   - pending jobs stay pending.
//
// The file is versioned and tied to one experiment. Loading validates the
// experiment ID and the identity (job ID, strategy, parameters) of every job,
// so a stale state file cannot be applied to a different sweep.

// The three coordinated views the coordinator checkpoints together.
struct CoordinatorStateSnapshot {
    // One entry per ExperimentRequest::jobs, in request order.
    std::vector<JobState> states;
    std::vector<std::size_t> attempts;
    std::vector<std::optional<ExperimentJobOutcome>> outcomes;
};

// Summary of what load() found and how it normalized the state.
struct CoordinatorRecovery {
    // True when an existing state file was loaded. False for a fresh run.
    bool recovered = false;

    // Terminal outcomes restored from the file. A job that was running when the
    // previous process stopped is not counted here even if it had an outcome;
    // it is reconciled to that outcome.
    std::size_t jobs_completed = 0;
    std::size_t jobs_failed = 0;

    // Jobs persisted as running were returned to pending.
    std::size_t jobs_requeued = 0;
};

class CoordinatorStateStore {
public:
    // `path` must be non-empty. The request supplies the identity that a later
    // load validates and the job metadata used when restoring outcomes.
    CoordinatorStateStore(
        std::string path,
        const ExperimentRequest& request
    );

    CoordinatorStateStore(const CoordinatorStateStore&) = delete;
    CoordinatorStateStore& operator=(const CoordinatorStateStore&) = delete;

    // Populates `snapshot` for a fresh run when no file exists, or with the
    // normalized persisted state when one does. Throws std::runtime_error when
    // the file is unreadable, malformed, belongs to a different experiment, or
    // contains an inconsistent job table.
    CoordinatorRecovery load(CoordinatorStateSnapshot& snapshot) const;

    // Atomically replaces the state file. `snapshot` must have exactly one
    // entry per request job. A terminal outcome always wins over the state
    // vector, so a snapshot taken while a terminal transition is in progress
    // cannot persist a running or pending job next to a terminal outcome.
    void save(const CoordinatorStateSnapshot& snapshot) const;

    const std::string& path() const noexcept;

private:
    std::string path_;
    std::string experiment_id_;
    std::vector<ExperimentJob> jobs_;
};
