#include "coordinator/job_state.h"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<ExperimentJob> make_jobs(std::size_t count) {
    std::vector<ExperimentJob> jobs;
    jobs.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        ExperimentJob job;
        job.job_id = "job-" + std::to_string(i);
        jobs.push_back(job);
    }

    return jobs;
}

void test_initial_state_is_pending() {
    const std::vector<ExperimentJob> jobs = make_jobs(3);
    JobStateTracker tracker(jobs);

    expect(tracker.size() == 3, "tracker size must match the job count");
    expect(
        tracker.count(JobState::pending) == 3,
        "every job must start pending"
    );
    expect(
        tracker.count(JobState::running) == 0,
        "no job may start running"
    );
    expect(!tracker.all_terminal(), "pending jobs are not terminal");

    const std::vector<JobState> snapshot = tracker.snapshot();

    expect(snapshot.size() == 3, "snapshot must cover every job");

    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        expect(
            snapshot[i] == JobState::pending,
            "snapshot must preserve request order"
        );
        expect(
            tracker.state(i) == JobState::pending,
            "state() must agree with the snapshot"
        );
    }
}

void test_pending_running_completed() {
    const std::vector<ExperimentJob> jobs = make_jobs(1);
    JobStateTracker tracker(jobs);

    const JobStateTransition running = tracker.mark_running(0);

    expect(running.index == 0, "transition must identify the job index");
    expect(running.job_id == "job-0", "transition must identify the job");
    expect(
        running.from == JobState::pending,
        "running must start from pending"
    );
    expect(running.to == JobState::running, "transition must end running");
    expect(
        tracker.state(0) == JobState::running,
        "the job must now be running"
    );

    const JobStateTransition completed = tracker.mark_completed(0);

    expect(
        completed.from == JobState::running,
        "completed must start from running"
    );
    expect(
        completed.to == JobState::completed,
        "the job must end completed"
    );
    expect(tracker.all_terminal(), "completed is a terminal state");
    expect(
        tracker.count(JobState::completed) == 1,
        "the completed count must be updated"
    );
}

void test_running_can_fail() {
    const std::vector<ExperimentJob> jobs = make_jobs(1);
    JobStateTracker tracker(jobs);

    tracker.mark_running(0);

    const JobStateTransition failed = tracker.mark_failed(0);

    expect(
        failed.from == JobState::running,
        "a running job can fail"
    );
    expect(failed.to == JobState::failed, "the job must end failed");
    expect(tracker.all_terminal(), "failed is a terminal state");
    expect(
        tracker.count(JobState::failed) == 1,
        "the failed count must be updated"
    );
}

void test_pending_jobs_can_fail_without_running() {
    const std::vector<ExperimentJob> jobs = make_jobs(2);
    JobStateTracker tracker(jobs);

    const JobStateTransition failed = tracker.mark_failed(1);

    expect(
        failed.from == JobState::pending,
        "an undispatched job can fail directly"
    );
    expect(failed.to == JobState::failed, "the job must end failed");
    expect(
        tracker.state(0) == JobState::pending,
        "other jobs must be unchanged"
    );
    expect(
        !tracker.all_terminal(),
        "one pending job must keep the experiment active"
    );
}

void test_fail_pending_marks_only_pending_jobs() {
    const std::vector<ExperimentJob> jobs = make_jobs(4);
    JobStateTracker tracker(jobs);

    tracker.mark_running(1);
    tracker.mark_running(2);
    tracker.mark_completed(2);

    const std::vector<JobStateTransition> transitions =
        tracker.fail_pending();

    expect(
        transitions.size() == 2,
        "fail_pending must return one transition per pending job"
    );
    expect(transitions[0].index == 0, "transitions must stay in order");
    expect(transitions[1].index == 3, "transitions must stay in order");

    for (const JobStateTransition& transition : transitions) {
        expect(
            transition.from == JobState::pending,
            "fail_pending must only touch pending jobs"
        );
        expect(
            transition.to == JobState::failed,
            "fail_pending must terminate jobs as failed"
        );
    }

    expect(
        tracker.state(1) == JobState::running,
        "a running job must not be changed"
    );
    expect(
        tracker.state(2) == JobState::completed,
        "a completed job must not be changed"
    );
    expect(
        !tracker.all_terminal(),
        "the untouched running job keeps the experiment active"
    );
    expect(
        tracker.fail_pending().empty(),
        "a second call must have nothing left to fail"
    );

    tracker.mark_failed(1);
    expect(tracker.all_terminal(), "every job must now be terminal");
}

void test_invalid_transitions_are_rejected() {
    const std::vector<ExperimentJob> jobs = make_jobs(2);
    JobStateTracker tracker(jobs);

    bool pending_to_completed_threw = false;

    try {
        tracker.mark_completed(0);
    } catch (const std::logic_error&) {
        pending_to_completed_threw = true;
    }

    expect(
        pending_to_completed_threw,
        "pending -> completed must be rejected"
    );

    tracker.mark_running(0);
    tracker.mark_completed(0);

    bool terminal_threw = false;

    try {
        tracker.mark_running(0);
    } catch (const std::logic_error&) {
        terminal_threw = true;
    }

    expect(
        terminal_threw,
        "a terminal job must not transition again"
    );

    bool range_threw = false;

    try {
        (void)tracker.state(2);
    } catch (const std::out_of_range&) {
        range_threw = true;
    }

    expect(range_threw, "an invalid index must be rejected");
}

void test_state_names() {
    expect(
        std::string(job_state_name(JobState::pending)) == "pending",
        "pending must have a stable name"
    );
    expect(
        std::string(job_state_name(JobState::running)) == "running",
        "running must have a stable name"
    );
    expect(
        std::string(job_state_name(JobState::completed)) == "completed",
        "completed must have a stable name"
    );
    expect(
        std::string(job_state_name(JobState::failed)) == "failed",
        "failed must have a stable name"
    );
}

}  // namespace

int main() {
    try {
        test_initial_state_is_pending();
        test_pending_running_completed();
        test_running_can_fail();
        test_pending_jobs_can_fail_without_running();
        test_fail_pending_marks_only_pending_jobs();
        test_invalid_transitions_are_rejected();
        test_state_names();

        std::cout << "job_state_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "job_state_tests failed: " << error.what() << '\n';
        return 1;
    }
}
