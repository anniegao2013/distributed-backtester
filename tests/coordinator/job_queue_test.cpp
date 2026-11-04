#include "coordinator/job_queue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

QueuedJob make_job(std::size_t index) {
    QueuedJob job;
    job.index = index;
    job.message.message_id = "message-" + std::to_string(index);
    job.message.job.job_id = "job-" + std::to_string(index);
    return job;
}

void test_fifo_order() {
    JobQueue queue;

    for (std::size_t i = 0; i < 3; ++i) {
        queue.push(make_job(i));
    }

    expect(!queue.empty(), "queue must hold the pushed jobs");
    expect(queue.size() == 3, "queue size must match the push count");

    queue.close();

    expect(queue.closed(), "close must mark the queue closed");
    expect(!queue.empty(), "close must not discard queued jobs");

    for (std::size_t i = 0; i < 3; ++i) {
        const std::optional<QueuedJob> job = queue.pop();

        expect(job.has_value(), "a closed queue must still yield queued jobs");
        expect(
            job->index == i,
            "jobs must be popped in FIFO insertion order"
        );
    }

    expect(
        !queue.pop().has_value(),
        "pop must return nullopt once a closed queue is empty"
    );
    expect(queue.empty(), "draining the queue must leave it empty");
    expect(queue.size() == 0, "draining the queue must reset its size");
}

void test_pop_blocks_until_push() {
    JobQueue queue;

    std::promise<void> started;
    std::future<void> ready = started.get_future();
    std::atomic<bool> completed{false};
    std::optional<QueuedJob> result;

    std::thread consumer([&] {
        started.set_value();
        result = queue.pop();
        completed.store(true);
    });

    ready.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    expect(
        !completed.load(),
        "pop must block while the queue is open and empty"
    );

    queue.push(make_job(7));
    consumer.join();

    expect(completed.load(), "push must wake a blocked pop");
    expect(result.has_value(), "the woken pop must return the pushed job");
    expect(result->index == 7, "the woken pop must return the new job");

    queue.close();
}

void test_close_wakes_blocked_pop() {
    JobQueue queue;

    std::promise<void> started;
    std::future<void> ready = started.get_future();
    std::atomic<bool> completed{false};
    std::optional<QueuedJob> result;

    std::thread consumer([&] {
        started.set_value();
        result = queue.pop();
        completed.store(true);
    });

    ready.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    expect(
        !completed.load(),
        "pop must block while the queue is open and empty"
    );

    queue.close();
    consumer.join();

    expect(completed.load(), "close must wake a blocked pop");
    expect(
        !result.has_value(),
        "a woken pop must report exhaustion when no jobs remain"
    );
}

void test_push_after_close_is_rejected() {
    JobQueue queue;
    queue.close();

    bool threw = false;

    try {
        queue.push(make_job(0));
    } catch (const std::logic_error&) {
        threw = true;
    }

    expect(threw, "push after close must throw std::logic_error");
    expect(queue.empty(), "a rejected push must not add a job");
}

void test_concurrent_distribution() {
    constexpr std::size_t kJobCount = 200;
    constexpr std::size_t kConsumerCount = 8;

    JobQueue queue;

    for (std::size_t i = 0; i < kJobCount; ++i) {
        queue.push(make_job(i));
    }

    queue.close();

    std::mutex mutex;
    std::exception_ptr consumer_error;
    std::vector<std::vector<std::size_t>> consumed(kConsumerCount);

    std::vector<std::thread> consumers;
    consumers.reserve(kConsumerCount);

    for (std::size_t i = 0; i < kConsumerCount; ++i) {
        consumers.emplace_back([&, i] {
            try {
                while (const std::optional<QueuedJob> job = queue.pop()) {
                    consumed[i].push_back(job->index);
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                consumer_error = std::current_exception();
            }
        });
    }

    for (std::thread& consumer : consumers) {
        consumer.join();
    }

    if (consumer_error) {
        std::rethrow_exception(consumer_error);
    }

    std::vector<std::size_t> all;
    all.reserve(kJobCount);

    for (const std::vector<std::size_t>& indices : consumed) {
        expect(
            std::is_sorted(indices.begin(), indices.end()),
            "each consumer must observe jobs in FIFO order"
        );

        all.insert(all.end(), indices.begin(), indices.end());
    }

    expect(
        all.size() == kJobCount,
        "concurrent consumers must receive every job exactly once"
    );

    std::sort(all.begin(), all.end());

    for (std::size_t i = 0; i < kJobCount; ++i) {
        expect(
            all[i] == i,
            "concurrent consumers must not duplicate or drop jobs"
        );
    }
}

}  // namespace

int main() {
    try {
        test_fifo_order();
        test_pop_blocks_until_push();
        test_close_wakes_blocked_pop();
        test_push_after_close_is_rejected();
        test_concurrent_distribution();

        std::cout << "job_queue_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "job_queue_tests failed: " << error.what() << '\n';
        return 1;
    }
}
