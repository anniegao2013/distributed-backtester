#include "core/json_value.h"
#include "protocol/protocol.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

std::string read_file(const std::string& path) {
    std::ifstream stream(path);

    if (!stream) {
        throw std::runtime_error("Could not open: " + path);
    }

    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

void write_file(const std::string& path, const std::string& contents) {
    std::ofstream stream(path);

    if (!stream) {
        throw std::runtime_error("Could not write: " + path);
    }

    stream << contents;

    if (!stream) {
        throw std::runtime_error("Could not write: " + path);
    }
}

int run_command(const std::string& command) {
    const int status = std::system(command.c_str());

    if (status == -1) {
        return -1;
    }

    return status;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;

    while (std::getline(stream, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }

    return lines;
}

BacktestJobMessage make_job(
    const std::string& message_id,
    const std::string& job_id
) {
    BacktestJobMessage message;
    message.message_id = message_id;
    message.job.experiment_id = "worker-e2e";
    message.job.job_id = job_id;
    message.job.strategy_id = "ma";
    message.job.strategy_type = "moving_average_crossover";
    message.job.parameters = {
        {"short_window", 20.0},
        {"long_window", 100.0}
    };
    message.job.data = {
        "data/market/sp500_1950_2018.csv",
        "SP500",
        "2000-01-01",
        "2010-12-31"
    };
    message.job.execution = {10000.0, 0.001, 0.0005};
    return message;
}

}  // namespace

// Runs the worker executable end-to-end through its stdio transport. The
// executable must read one compact JSON document per line and write exactly
// one reply per input message.
int main() {
    try {
        const std::string executable = EXECUTABLE_PATH;
        const std::string prefix =
            (std::filesystem::temp_directory_path() /
             ("backtester_worker_e2e_" + std::to_string(getpid())))
                .string();

        const std::string input_path = prefix + ".in";
        const std::string output_path = prefix + ".out";
        const std::string error_path = prefix + ".err";

        const BacktestJobMessage first =
            make_job("worker-e2e:ma-1:0001", "ma-1");
        const BacktestJobMessage second =
            make_job("worker-e2e:ma-2:0002", "ma-2");

        write_file(
            input_path,
            backtest_job_to_json(first, false) + "\n" +
                backtest_job_to_json(second, false) + "\n"
        );

        const int exit_code = run_command(
            "'" + executable +
            "' --worker-id worker-e2e < '" + input_path +
            "' > '" + output_path +
            "' 2> '" + error_path + "'"
        );

        if (exit_code != 0) {
            std::cerr << "Worker exited with code " << exit_code << '\n';
            std::cerr << read_file(error_path) << '\n';
            return 1;
        }

        const std::vector<std::string> lines =
            split_lines(read_file(output_path));

        if (lines.size() != 2) {
            std::cerr << "Expected two replies, got " << lines.size() << '\n';
            return 1;
        }

        const BacktestResultMessage first_reply =
            parse_backtest_result_json(lines[0]);
        const BacktestResultMessage second_reply =
            parse_backtest_result_json(lines[1]);

        if (first_reply.message_id != first.message_id ||
            second_reply.message_id != second.message_id) {
            std::cerr << "Replies did not echo message_id\n";
            return 1;
        }

        if (first_reply.worker_id != "worker-e2e" ||
            second_reply.worker_id != "worker-e2e") {
            std::cerr << "Replies did not echo worker_id\n";
            return 1;
        }

        if (first_reply.result.status != "completed" ||
            second_reply.result.status != "completed") {
            std::cerr << "Expected both jobs to complete\n";
            return 1;
        }

        if (first_reply.result.job_id != "ma-1" ||
            second_reply.result.job_id != "ma-2") {
            std::cerr << "Replies did not echo job identity\n";
            return 1;
        }

        // A malformed line must produce one protocol_error, and the process
        // must still exit cleanly at end of input.
        write_file(input_path, "{not json\n");

        const int malformed_exit = run_command(
            "'" + executable +
            "' < '" + input_path +
            "' > '" + output_path +
            "' 2> '" + error_path + "'"
        );

        if (malformed_exit != 0) {
            std::cerr << "Worker failed on malformed input\n";
            std::cerr << read_file(error_path) << '\n';
            return 1;
        }

        const std::vector<std::string> malformed_lines =
            split_lines(read_file(output_path));

        if (malformed_lines.size() != 1) {
            std::cerr << "Expected one protocol error reply\n";
            return 1;
        }

        const ProtocolErrorMessage error =
            parse_protocol_error_json(malformed_lines.front());

        if (error.error.type != "json_parse_error") {
            std::cerr << "Expected json_parse_error\n";
            return 1;
        }

        std::filesystem::remove(input_path);
        std::filesystem::remove(output_path);
        std::filesystem::remove(error_path);

        std::cout << "Worker executable test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Worker executable test failed: " << error.what()
                  << '\n';
        return 1;
    }
}
