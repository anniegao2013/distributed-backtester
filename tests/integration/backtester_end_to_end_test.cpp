#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

// Runs a shell command and captures stdout+stderr. Returns the process exit
// code (0 on success).
int run_command(const std::string& command, std::string& output) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return -1;
    }

    std::ostringstream captured;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        captured << buffer;
    }

    output = captured.str();
    return pclose(pipe);
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// Runs the backtester executable end-to-end against the real market data and
// asserts that the full pipeline executes: data load, concurrent backtests
// over the parameter grid, per-combination results, timing, and a ranked
// summary with a best configuration. Also exercises the JSON experiment and
// service-style stdin entry points.
int main() {
    const std::string executable = EXECUTABLE_PATH;

    std::string full_output;
    const int exit_code = run_command(executable + " 2>&1", full_output);

    if (exit_code != 0) {
        std::cerr << "Backtester exited with code " << exit_code << '\n';
        std::cerr << full_output << '\n';
        return 1;
    }

    // Structural assertions on the end-to-end output.
    if (!contains(full_output, "Successfully loaded")) {
        std::cerr << "Missing: market data load message\n";
        return 1;
    }

    if (!contains(full_output, "parameter combinations concurrently")) {
        std::cerr << "Missing: concurrent grid execution message\n";
        return 1;
    }

    if (!contains(full_output, "Completed") ||
        !contains(full_output, "backtests in") ||
        !contains(full_output, "seconds")) {
        std::cerr << "Missing: completion message with timing\n";
        return 1;
    }

    if (!contains(full_output, "=== Ranked Results (by Sharpe ratio) ===")) {
        std::cerr << "Missing: ranked results table\n";
        return 1;
    }

    if (!contains(full_output, "Best configuration:")) {
        std::cerr << "Missing: best configuration summary\n";
        return 1;
    }

    // All strategies should be exercised side by side by the demo grid.
    if (!contains(full_output, "crossover-1") ||
        !contains(full_output, "crossover-378") ||
        !contains(full_output, "rsi-1") ||
        !contains(full_output, "rsi-16") ||
        !contains(full_output, "donchian-1") ||
        !contains(full_output, "donchian-9")) {
        std::cerr << "Missing: expected crossover, RSI, and Donchian job IDs\n";
        return 1;
    }

    // A JSON experiment file should run through the same pipeline and emit a
    // machine-readable response.
    std::string json_output;
    const int json_exit = run_command(
        executable +
            " --experiment experiments/crossover_sweep.json --json 2>&1",
        json_output
    );

    if (json_exit != 0) {
        std::cerr << "JSON experiment exited with code " << json_exit << '\n';
        std::cerr << json_output << '\n';
        return 1;
    }

    if (!contains(json_output, "\"status\": \"completed\"") ||
        !contains(json_output, "\"job_id\": \"crossover-1\"") ||
        !contains(json_output, "\"job_id\": \"crossover-4\"")) {
        std::cerr << "Missing: expected JSON experiment response\n";
        std::cerr << json_output << '\n';
        return 1;
    }

    // The service-style entry point reads a request from stdin and reports
    // schema errors as JSON with a non-crashing exit code.
    std::string invalid_output;
    const int invalid_exit = run_command(
        "printf '%s' '{\"schema_version\":1}' | " + executable +
            " --experiment - --json 2>&1",
        invalid_output
    );

    if (invalid_exit != 0) {
        std::cerr << "Invalid JSON request exited with code "
                  << invalid_exit << '\n';
        std::cerr << invalid_output << '\n';
        return 1;
    }

    if (!contains(invalid_output, "\"status\": \"invalid\"") ||
        !contains(invalid_output, "\"type\": \"validation_error\"")) {
        std::cerr << "Missing: expected structured validation error\n";
        std::cerr << invalid_output << '\n';
        return 1;
    }

    std::cout << "End-to-end test passed\n";
    return 0;
}
