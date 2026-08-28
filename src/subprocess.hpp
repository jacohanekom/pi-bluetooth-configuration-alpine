#pragma once
/**
 * subprocess.hpp -- run an external program with an argv array (no shell),
 * capture its combined stdout+stderr, and enforce a wall-clock timeout.
 *
 * argv is passed straight to execvp() -- there is no shell in between, so
 * SSIDs/passphrases coming off the BLE link can never be interpreted as
 * shell metacharacters no matter what bytes they contain.
 */
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

struct CommandResult {
    int exit_code = -1;
    std::string output;
    bool timed_out = false;
};

inline CommandResult run_command(const std::vector<std::string>& argv, int timeout_sec = 15) {
    CommandResult result;
    int out_pipe[2];
    if (pipe(out_pipe) != 0) {
        result.output = std::string("pipe() failed: ") + strerror(errno);
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.output = std::string("fork() failed: ") + strerror(errno);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return result;
    }

    if (pid == 0) {
        // Child: stdout and stderr both feed the same pipe so command
        // errors show up in the captured output for logging.
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);

        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);

        execvp(cargv[0], cargv.data());
        _exit(127); // execvp only returns on failure
    }

    // Parent
    close(out_pipe[1]);

    int flags = fcntl(out_pipe[0], F_GETFL, 0);
    fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK);

    time_t deadline = time(nullptr) + timeout_sec;
    bool exited = false;
    int status = 0;

    while (true) {
        char buf[4096];
        ssize_t n = read(out_pipe[0], buf, sizeof(buf));
        if (n > 0) {
            result.output.append(buf, n);
        } else if (n == 0) {
            // Writer closed -- child exited or execvp failed.
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }

        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            exited = true;
            // Drain any remaining buffered output.
            while ((n = read(out_pipe[0], buf, sizeof(buf))) > 0)
                result.output.append(buf, n);
            break;
        }

        if (time(nullptr) >= deadline) {
            result.timed_out = true;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            exited = true;
            break;
        }

        usleep(50 * 1000);
    }

    close(out_pipe[0]);

    if (exited && !result.timed_out) {
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return result;
}
