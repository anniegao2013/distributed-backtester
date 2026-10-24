#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

// Runs the backtester executable end-to-end against the real market data
// and asserts that the full pipeline executes: data load, concurrent
// backtests over the parameter grid, per-combination results, timing, and a
// ranked summary with a best configuration.
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

    if (!contains("parameter combinations concurrently")) {
        std::cerr << "Missing: concurrent grid execution message\n";
        return 1;
    }

    if (!contains("Completed") || !contains("backtests in") ||
        !contains("seconds")) {
        std::cerr << "Missing: completion message with timing\n";
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

    // Both strategies should be exercised side by side by the demo grid.
    if (!contains("crossover-1") || !contains("crossover-378") ||
        !contains("rsi-1") || !contains("rsi-16")) {
        std::cerr << "Missing: expected crossover and RSI job IDs\n";
        return 1;
    }

    std::cout << "End-to-end test passed\n";
    return 0;
}
