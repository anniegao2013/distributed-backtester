#include "coordinator/scheduling.h"

#include <algorithm>
#include <cstddef>

namespace {

double parameter_or(
    const ParameterMap& parameters,
    const char* name,
    double fallback
) {
    const auto it = parameters.find(name);

    if (it == parameters.end()) {
        return fallback;
    }

    // Periods are validated positive before finalization; clamping keeps a
    // malformed or hand-built message from producing a negative estimate.
    return std::max(1.0, it->second);
}

double window_sum(
    const BacktestJob& job,
    const char* first,
    const char* second
) {
    return parameter_or(job.parameters, first, 1.0) +
           parameter_or(job.parameters, second, 1.0);
}

}  // namespace

const char* scheduling_policy_name(SchedulingPolicy policy) {
    switch (policy) {
        case SchedulingPolicy::Fifo:
            return "fifo";
        case SchedulingPolicy::Lifo:
            return "lifo";
        case SchedulingPolicy::ShortestJobFirst:
            return "shortest-first";
        case SchedulingPolicy::LongestJobFirst:
            return "longest-first";
    }

    return "fifo";
}

bool parse_scheduling_policy(
    const std::string& text,
    SchedulingPolicy& policy
) {
    if (text == "fifo") {
        policy = SchedulingPolicy::Fifo;
        return true;
    }

    if (text == "lifo") {
        policy = SchedulingPolicy::Lifo;
        return true;
    }

    if (text == "shortest-first" ||
        text == "shortest-job-first" ||
        text == "sjf") {
        policy = SchedulingPolicy::ShortestJobFirst;
        return true;
    }

    if (text == "longest-first" ||
        text == "longest-job-first" ||
        text == "ljf") {
        policy = SchedulingPolicy::LongestJobFirst;
        return true;
    }

    return false;
}

double estimate_job_cost(const BacktestJob& job) {
    // Fixed per-bar engine and portfolio work, in arbitrary relative units.
    constexpr double kFixedCost = 1.0;

    if (job.strategy_type == "moving_average_crossover") {
        // Each bar evaluates four trailing sums, but the two windows dominate.
        return kFixedCost +
               0.022 * window_sum(job, "short_window", "long_window");
    }

    if (job.strategy_type == "donchian_channel_breakout") {
        return kFixedCost +
               0.010 * window_sum(job, "entry_period", "exit_period");
    }

    // RSI is constant work per bar regardless of its period. Unknown strategy
    // types also fall back to a neutral estimate instead of being starved.
    return kFixedCost;
}
