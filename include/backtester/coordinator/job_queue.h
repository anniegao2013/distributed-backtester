#pragma once

#include "protocol/protocol.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

// The unit stored in the coordinator's job queue. The index preserves the
// position of the job in the finalized experiment's request order; outcomes are
// written back to that slot.
struct QueuedJob {
    std::size_t index = 0;
    BacktestJobMessage message;
};

// Thread-safe FIFO queue used to distribute jobs across workers.
//
// The coordinator pushes every expanded job once, then closes the queue.
// Workers call pop() to take the next job in insertion order. push() appends to
// the back and pop() removes from the front, so distribution is FIFO even when
// several workers pop concurrently. A blocked pop() wakes when a job arrives or
// when close() is called.
class JobQueue {
public:
    JobQueue() = default;

    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    // Appends one job to the back. Throws std::logic_error if the queue has
    // already been closed.
    void push(QueuedJob job);

    // Blocks until a job is available or the queue is closed. Returns the
    // oldest job, or std::nullopt once the queue is closed and empty.
    std::optional<QueuedJob> pop();

    // Stops accepting new jobs and wakes every blocked pop(). Jobs already
    // queued remain available until they are popped.
    void close();

    bool empty() const;
    std::size_t size() const;
    bool closed() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<QueuedJob> jobs_;
    bool closed_ = false;
};
