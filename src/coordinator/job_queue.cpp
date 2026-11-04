#include "coordinator/job_queue.h"

#include <stdexcept>
#include <utility>

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
    if (jobs_.empty()) {
        return std::nullopt;
    }

    QueuedJob job = std::move(jobs_.front());
    jobs_.pop_front();
    return job;
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
