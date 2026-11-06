#include "net/tcp.h"
#include "protocol/protocol.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

BacktestJobMessage make_job() {
    BacktestJobMessage message;
    message.message_id = "worker-tcp:ma-1:0001";
    message.job.experiment_id = "worker-tcp";
    message.job.job_id = "ma-1";
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

// Runs the worker executable as a TCP client against a listener owned by this
// test. It verifies the --connect path, worker_ready handshake, one job result,
// and clean exit after shutdown.
int main() {
    try {
        net::TcpListener listener =
            net::TcpListener::bind("127.0.0.1", 0, 1);
        const std::string endpoint =
            "127.0.0.1:" + std::to_string(listener.port());
        const std::string executable = EXECUTABLE_PATH;

        const pid_t child = ::fork();

        if (child < 0) {
            throw std::runtime_error("Could not fork the worker");
        }

        if (child == 0) {
            listener.close();
            ::execl(
                executable.c_str(),
                "backtester_worker",
                "--worker-id",
                "worker-tcp",
                "--connect",
                endpoint.c_str(),
                static_cast<char*>(nullptr)
            );
            ::_exit(127);
        }

        net::TcpSocket socket = listener.accept();

        const auto ready_frame = socket.receive_frame();
        expect(ready_frame.has_value(), "worker must send worker_ready");

        const WorkerReadyMessage ready =
            parse_worker_ready_json(*ready_frame);
        expect(
            ready.worker_id == "worker-tcp",
            "worker_ready must identify the worker"
        );

        const BacktestJobMessage job = make_job();
        socket.send_frame(backtest_job_to_json(job, /*pretty=*/false));

        const auto result_frame = socket.receive_frame();
        expect(result_frame.has_value(), "worker must send a result");

        const BacktestResultMessage result =
            parse_backtest_result_json(*result_frame);
        expect(
            result.message_id == job.message_id,
            "result must echo message_id"
        );
        expect(
            result.worker_id == "worker-tcp",
            "result must echo worker_id"
        );
        expect(
            result.result.status == "completed",
            "the job must complete"
        );
        expect(
            result.result.metrics.has_value(),
            "a completed result must carry metrics"
        );

        ShutdownMessage shutdown;
        shutdown.message_id = "worker-tcp:shutdown:0001";
        shutdown.reason = "test complete";
        socket.send_frame(shutdown_to_json(shutdown, /*pretty=*/false));
        socket.close();

        int status = 0;
        expect(
            ::waitpid(child, &status, 0) == child,
            "could not wait for the worker"
        );
        expect(
            WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "worker must exit cleanly after shutdown"
        );

        std::cout << "TCP worker executable test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TCP worker executable test failed: " << error.what()
                  << '\n';
        return 1;
    }
}
