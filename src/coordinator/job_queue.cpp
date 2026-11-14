#include "coordinator/job_queue.h"

#include <cstddef>
#include <stdexcept>
#include <utility>

std::size_t JobQueue::next_index() const {
    if (jobs_.empty()) {
        return 0;
    }

    if (policy_ == SchedulingPolicy::Fifo) {
        return 0;
    }

    if (policy_ == SchedulingPolicy::Lifo) {
        return jobs_.size() - 1;
    }

    // Priority policies scan the pending set. Sweeps are small enough that an
    // O(n) scan per pop is cheaper than maintaining a separate heap, and the
    // deque preserves insertion order for equal-cost jobs.
    const bool longest =
        policy_ == SchedulingPolicy::LongestJobFirst;
    std::size_t best = 0;
    double best_cost = estimate_job_cost(jobs_.front().message.job);

    for (std::size_t i = 1; i < jobs_.size(); ++i) {
        const double cost = estimate_job_cost(jobs_[i].message.job);

        if ((longest && cost > best_cost) ||
            (!longest && cost < best_cost)) {
            best = i;
            best_cost = cost;
        }
    }

    return best;
}

std::optional<QueuedJob> JobQueue::take_next() {
    if (jobs_.empty()) {
        return std::nullopt;
    }

    auto position = jobs_.begin() +
                    static_cast<std::ptrdiff_t>(next_index());
    QueuedJob job = std::move(*position);
    jobs_.erase(position);
    return job;
}

void JobQueue::push(QueuedJob job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (closed_) {
            throw std::logic_error(
                "cannot push to a closed job queue"
            );
        }

        jobs_.push_back(std::move(job));
    }

    condition_.notify_one();
}

std::optional<QueuedJob> JobQueue::pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] {
        return closed_ || !jobs_.empty();
    });

    // A closed queue can still have jobs left; only an empty closed queue
    // signals that every job has been distributed.
    return take_next();
}

std::optional<QueuedJob> JobQueue::pop_for(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait_for(lock, timeout, [this] {
        return closed_ || !jobs_.empty();
    });

    return take_next();
}

void JobQueue::requeue_front(QueuedJob job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // LIFO pops from the back, so the logical front of a stack is its back.
        if (policy_ == SchedulingPolicy::Lifo) {
            jobs_.push_back(std::move(job));
        } else {
            jobs_.push_front(std::move(job));
        }
    }

    condition_.notify_one();
}

void JobQueue::requeue_back(QueuedJob job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // LIFO pops from the back, so the logical back of a stack is its front.
        if (policy_ == SchedulingPolicy::Lifo) {
            jobs_.push_front(std::move(job));
        } else {
            jobs_.push_back(std::move(job));
        }
    }

    condition_.notify_one();
}

void JobQueue::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }

    condition_.notify_all();
}

bool JobQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.empty();
}

std::size_t JobQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.size();
}

bool JobQueue::closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}
