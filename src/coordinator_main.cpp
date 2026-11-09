#include "coordinator/coordinator.h"
#include "experiment/experiment.h"
#include "net/tcp.h"

#include <cstddef>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

constexpr const char* kUsage =
    "Usage: backtester_coordinator [options]\n"
    "\n"
    "Starts a TCP server, waits for backtest workers to connect, expands one\n"
    "experiment into jobs, and dispatches each job to a worker that is ready\n"
    "for work. When every job has a result the coordinator aggregates them and\n"
    "sends shutdown to each worker.\n"
    "\n"
    "Options:\n"
    "  -e, --experiment PATH   Experiment JSON to run (required). Use '-' to\n"
    "                          read the request from stdin.\n"
    "  -l, --listen HOST:PORT  Address to listen on. Default: 127.0.0.1:9000.\n"
    "                          Port 0 selects an ephemeral port.\n"
    "  -n, --workers N         Number of workers to accept before dispatching.\n"
    "                          Default: 1.\n"
    "      --json              Emit the aggregated result as JSON.\n"
    "  -h, --help              Show this help.\n"
    "\n"
    "Examples:\n"
    "  backtester_coordinator -e experiments/crossover_sweep.json -n 2\n"
    "  backtester_coordinator -e experiments/demo.json --json\n";

std::string read_stream(std::istream& stream) {
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

std::string read_file(const std::string& path) {
    if (path == "-") {
        return read_stream(std::cin);
    }

    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error(
            "Could not open experiment file '" + path + "'"
        );
    }

    std::string contents = read_stream(stream);
    if (!stream.good() && !stream.eof()) {
        throw std::runtime_error(
            "Could not read experiment file '" + path + "'"
        );
    }

    return contents;
}

struct Options {
    bool help = false;
    bool json = false;
    std::string experiment_path;
    net::TcpEndpoint listen{"127.0.0.1", 9000};
    std::size_t workers = 1;
};

std::size_t parse_worker_count(const std::string& text) {
    if (text.empty()) {
        throw std::invalid_argument("--workers requires a positive integer");
    }

    for (char c : text) {
        if (c < '0' || c > '9') {
            throw std::invalid_argument(
                "--workers requires a positive integer, got '" + text + "'"
            );
        }
    }

    unsigned long value = 0;

    try {
        value = std::stoul(text);
    } catch (const std::exception&) {
        throw std::invalid_argument(
            "--workers value is out of range: '" + text + "'"
        );
    }

    if (value == 0 || value > 1024) {
        throw std::invalid_argument(
            "--workers must be between 1 and 1024"
        );
    }

    return static_cast<std::size_t>(value);
}

Options parse_options(int argc, char* argv[]) {
    Options options;
    bool has_experiment = false;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];

        if (argument == "-h" || argument == "--help") {
            options.help = true;
        } else if (argument == "--json") {
            options.json = true;
        } else if (argument == "-e" || argument == "--experiment") {
            if (i + 1 >= argc) {
                throw std::invalid_argument(
                    argument + " requires a file path (or '-')"
                );
            }

            options.experiment_path = argv[++i];
            has_experiment = true;
        } else if (argument == "-l" || argument == "--listen") {
            if (i + 1 >= argc) {
                throw std::invalid_argument(
                    argument + " requires HOST:PORT"
                );
            }

            options.listen = net::parse_tcp_endpoint(argv[++i]);
        } else if (argument == "-n" || argument == "--workers") {
            if (i + 1 >= argc) {
                throw std::invalid_argument(
                    argument + " requires a worker count"
                );
            }

            options.workers = parse_worker_count(argv[++i]);
        } else {
            throw std::invalid_argument(
                "Unknown argument '" + argument + "'"
            );
        }
    }

    if (!options.help && !has_experiment) {
        throw std::invalid_argument(
            "An experiment is required; pass --experiment PATH"
        );
    }

    return options;
}

void print_result(
    const ExperimentResult& result,
    const CoordinatorStats& stats
) {
    std::cout << "Experiment: " << result.experiment_id << '\n';
    std::cout << "Status: " << result.status << '\n';
    std::cout << "Workers: " << stats.workers_registered << '\n';

    if (stats.workers_disconnected > 0) {
        std::cout << "Workers disconnected: "
                  << stats.workers_disconnected << '\n';
    }

    std::cout << "Jobs completed: " << result.summary.jobs_completed
              << " of " << result.summary.jobs_total << '\n';
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "Wall time: " << result.summary.wall_seconds << " s\n";
    std::cout << "Throughput: "
              << result.summary.performance.throughput << " jobs/s\n";

    for (const ExperimentJobOutcome& outcome : result.results) {
        if (!outcome.ok) {
            std::cerr << "Job " << outcome.job.job_id
                      << " failed: " << outcome.error << '\n';
        }
    }

    if (!result.summary.best_job_id.empty()) {
        std::cout << "Best configuration: " << result.summary.best_job_id
                  << " (Sharpe " << result.summary.best_sharpe_ratio
                  << ", total return "
                  << result.summary.best_total_return * 100.0 << "%)\n";
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const Options options = parse_options(argc, argv);

        if (options.help) {
            std::cout << kUsage;
            return 0;
        }

        const ExperimentRequest request =
            parse_experiment_json(read_file(options.experiment_path));

        CoordinatorConfig config;
        config.listen_host = options.listen.host;
        config.listen_port = options.listen.port;
        config.expected_workers = options.workers;
        config.log = [](const std::string& message) {
            std::cerr << "[coordinator] " << message << '\n';
        };

        CoordinatorStats stats;
        const ExperimentResult result =
            run_distributed_experiment(request, config, &stats);

        if (options.json) {
            std::cout << experiment_result_to_json(result, true);
            return 0;
        }

        print_result(result, stats);
        return 0;
    } catch (const ExperimentValidationError& error) {
        std::cerr << "Invalid experiment";
        if (!error.path().empty()) {
            std::cerr << " at " << error.path();
        }
        std::cerr << ": " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Coordinator error: " << error.what() << '\n';
        return 1;
    }
}
