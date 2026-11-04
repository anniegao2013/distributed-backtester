#include "experiment/result_collector.h"

#include <stdexcept>
#include <utility>

namespace {

ExperimentJobOutcome failed_outcome(
    const ExperimentJob& job,
    const std::string& error
) {
    ExperimentJobOutcome outcome;
    outcome.job = job;
    outcome.error = error.empty()
        ? "job did not reach a terminal state"
        : error;
    return outcome;
}

}  // namespace

ExperimentResultCollector::ExperimentResultCollector(
    std::vector<ExperimentJob> jobs
) : jobs_(std::move(jobs)), outcomes_(jobs_.size()) {}

std::size_t ExperimentResultCollector::size() const noexcept {
    return jobs_.size();
}

std::size_t ExperimentResultCollector::terminal_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return terminal_count_;
}

std::size_t ExperimentResultCollector::remaining() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.size() - terminal_count_;
}

bool ExperimentResultCollector::complete() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return terminal_count_ == jobs_.size();
}

void ExperimentResultCollector::record(
    std::size_t index,
    ExperimentJobOutcome outcome
) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (index >= outcomes_.size()) {
            throw std::out_of_range(
                "job index " + std::to_string(index) +
                " is outside the experiment"
            );
        }

        if (outcomes_[index]) {
            throw std::logic_error(
                "job index " + std::to_string(index) +
                " already has a terminal outcome"
            );
        }

        outcomes_[index] = std::move(outcome);
        ++terminal_count_;
    }

    condition_.notify_all();
}

std::size_t ExperimentResultCollector::fail_remaining(
    const std::string& error
) {
    std::size_t marked = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        for (std::size_t i = 0; i < outcomes_.size(); ++i) {
            if (outcomes_[i]) {
                continue;
            }

            outcomes_[i] = failed_outcome(jobs_[i], error);
            ++terminal_count_;
            ++marked;
        }
    }

    if (marked > 0) {
        condition_.notify_all();
    }

    return marked;
}

void ExperimentResultCollector::wait() const {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] {
        return terminal_count_ == jobs_.size();
    });
}

std::vector<ExperimentJobOutcome> ExperimentResultCollector::take() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (terminal_count_ != jobs_.size()) {
        throw std::logic_error(
            "experiment is not complete: " +
            std::to_string(jobs_.size() - terminal_count_) +
            " job(s) are still pending"
        );
    }

    if (taken_) {
        throw std::logic_error(
            "experiment results have already been taken"
        );
    }

    taken_ = true;

    std::vector<ExperimentJobOutcome> outcomes;
    outcomes.reserve(outcomes_.size());

    for (std::optional<ExperimentJobOutcome>& outcome : outcomes_) {
        outcomes.push_back(std::move(*outcome));
    }

    return outcomes;
}
