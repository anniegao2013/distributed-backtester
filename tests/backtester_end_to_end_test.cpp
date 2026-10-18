#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

// Runs the backtester executable end-to-end against the real market data
// and asserts that the full pipeline executes: data load, sequential
// backtests over the parameter grid, per-combination results, and a ranked
// summary with a best configuration.
int main() {
    const std::string executable = EXECUTABLE_PATH;

    // Open a pipe to capture stdout.
    const std::string command = executable + " 2>&1";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        std::cerr << "Failed to launch " << executable << '\n';
        return 1;
    }

    std::ostringstream output;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output << buffer;
    }

    const int exit_code = pclose(pipe);
    const std::string full_output = output.str();

    if (exit_code != 0) {
        std::cerr << "Backtester exited with code " << exit_code << '\n';
        std::cerr << full_output << '\n';
        return 1;
    }

    // Structural assertions on the end-to-end output.
    auto contains = [&full_output](const std::string& needle) {
        return full_output.find(needle) != std::string::npos;
    };

    if (!contains("Successfully loaded")) {
        std::cerr << "Missing: market data load message\n";
        return 1;
    }

    if (!contains("Running 9 parameter combinations concurrently")) {
        std::cerr << "Missing: concurrent grid execution message\n";
        return 1;
    }

    if (!contains("Completed 9 of 9 backtests")) {
        std::cerr << "Missing: completion message\n";
        return 1;
    }

    if (!contains("=== Ranked Results (by Sharpe ratio) ===")) {
        std::cerr << "Missing: ranked results table\n";
        return 1;
    }

    if (!contains("Best configuration:")) {
        std::cerr << "Missing: best configuration summary\n";
        return 1;
    }

    // Each job ID grid-1 .. grid-9 should appear in the results.
    for (int i = 1; i <= 9; ++i) {
        if (!contains("grid-" + std::to_string(i))) {
            std::cerr << "Missing: result for grid-" << i << '\n';
            return 1;
        }
    }

    std::cout << "End-to-end test passed\n";
    return 0;
}
