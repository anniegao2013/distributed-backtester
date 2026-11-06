#pragma once

#include "experiment/experiment.h"

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Collects exactly one terminal outcome per finalized experiment job.
//
// This is the boundary between job execution and experiment aggregation. A
// local thread and a remote worker both report through record(index, outcome),
// where `index` is the job's position in ExperimentRequest::jobs. The collector
// therefore does not know or care which execution backend produced a result.
//
// Results may arrive in any order. A job is terminal as soon as it has a
// completed or failed outcome; the run is complete when every job is terminal.
// take() always returns outcomes in request order so aggregation stays
// deterministic.
class ExperimentResultCollector {
public:
    explicit ExperimentResultCollector(std::vector<ExperimentJob> jobs);

    ExperimentResultCollector(const ExperimentResultCollector&) = delete;
    ExperimentResultCollector& operator=(
        const ExperimentResultCollector&
    ) = delete;

    std::size_t size() const noexcept;

    // Number of jobs with a terminal outcome, and the number still pending.
    std::size_t terminal_count() const;
    std::size_t remaining() const;

    // True once terminal_count() == size().
    bool complete() const;

    // Records the terminal outcome for jobs[index]. Throws std::out_of_range
    // for an invalid index and std::logic_error if that job already terminated.
    void record(std::size_t index, ExperimentJobOutcome outcome);

    // Marks every still-pending job as failed with `error`. This terminates
    // jobs that no execution backend is left to run, such as jobs still queued
    // after every worker has failed. Completed jobs are never overwritten.
    // Returns the number of jobs newly marked terminal.
    std::size_t fail_remaining(const std::string& error);

    // Blocks until every job has a terminal outcome.
    void wait() const;

    // Returns outcomes in request order. Throws std::logic_error if any job is
    // still pending. The collector is consumed and may be taken only once.
    std::vector<ExperimentJobOutcome> take();

private:
    std::vector<ExperimentJob> jobs_;
    std::vector<std::optional<ExperimentJobOutcome>> outcomes_;

    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    std::size_t terminal_count_ = 0;
    bool taken_ = false;
};
