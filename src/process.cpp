#include "process.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>

namespace jjmcp {
namespace {

struct Fd {
    int value = -1;
    Fd() = default;
    explicit Fd(int fd) : value(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value(other.value) { other.value = -1; }
    Fd& operator=(Fd&& other) noexcept
    {
        if (this != &other) {
            close_now();
            value = other.value;
            other.value = -1;
        }
        return *this;
    }
    ~Fd() { close_now(); }

    void close_now()
    {
        if (value >= 0) {
            ::close(value);
            value = -1;
        }
    }
};

Result<void> make_pipe(std::array<int, 2>& pipe_fds)
{
    if (::pipe(pipe_fds.data()) != 0) {
        return Result<void>::failure(std::string("pipe failed: ") + std::strerror(errno));
    }
    return Result<void>::success();
}

void set_nonblocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

void append_available(int fd, std::string& output, bool& open)
{
    char buffer[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            output.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            open = false;
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        open = false;
        return;
    }
}

std::vector<char*> make_exec_argv(const std::vector<std::string>& argv)
{
    std::vector<char*> result;
    result.reserve(argv.size() + 1);
    for (const auto& arg : argv) {
        result.push_back(const_cast<char*>(arg.c_str()));
    }
    result.push_back(nullptr);
    return result;
}

} // namespace

Result<ProcessResult> ProcessRunner::run(const RunSpec& spec) const
{
    if (spec.argv.empty() || spec.argv.front().empty()) {
        return Result<ProcessResult>::failure("process argv is empty");
    }

    std::array<int, 2> stdout_pipe{-1, -1};
    std::array<int, 2> stderr_pipe{-1, -1};
    std::array<int, 2> stdin_pipe{-1, -1};

    if (auto r = make_pipe(stdout_pipe); !r) {
        return Result<ProcessResult>::failure(r.error());
    }
    if (auto r = make_pipe(stderr_pipe); !r) {
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        return Result<ProcessResult>::failure(r.error());
    }

    const bool has_stdin = !spec.stdin_data.empty();
    if (has_stdin) {
        if (auto r = make_pipe(stdin_pipe); !r) {
            ::close(stdout_pipe[0]);
            ::close(stdout_pipe[1]);
            ::close(stderr_pipe[0]);
            ::close(stderr_pipe[1]);
            return Result<ProcessResult>::failure(r.error());
        }
    }

    // argv must be materialized before the fork: the child may only call async-signal-safe
    // functions, and background pollers fork concurrently with the worker thread.
    auto exec_argv = make_exec_argv(spec.argv);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        if (has_stdin) {
            ::close(stdin_pipe[0]);
            ::close(stdin_pipe[1]);
        }
        return Result<ProcessResult>::failure(std::string("fork failed: ") + std::strerror(errno));
    }

    if (pid == 0) {
        if (has_stdin) {
            (void)::dup2(stdin_pipe[0], STDIN_FILENO);
        }
        (void)::dup2(stdout_pipe[1], STDOUT_FILENO);
        (void)::dup2(stderr_pipe[1], STDERR_FILENO);

        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        if (has_stdin) {
            ::close(stdin_pipe[0]);
            ::close(stdin_pipe[1]);
        }

        ::execvp(exec_argv[0], exec_argv.data());
        static constexpr char kExecFailed[] = "execvp failed\n";
        const ssize_t ignored = ::write(STDERR_FILENO, kExecFailed, sizeof(kExecFailed) - 1);
        (void)ignored;
        _exit(127);
    }

    Fd out_read(stdout_pipe[0]);
    Fd err_read(stderr_pipe[0]);
    Fd out_write(stdout_pipe[1]);
    Fd err_write(stderr_pipe[1]);
    Fd in_read(has_stdin ? stdin_pipe[0] : -1);
    Fd in_write(has_stdin ? stdin_pipe[1] : -1);

    out_write.close_now();
    err_write.close_now();
    in_read.close_now();

    if (has_stdin) {
        const char* data = spec.stdin_data.data();
        std::size_t remaining = spec.stdin_data.size();
        while (remaining > 0) {
            const ssize_t n = ::write(in_write.value, data, remaining);
            if (n > 0) {
                data += n;
                remaining -= static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EPIPE || errno == EINVAL)) {
                break;
            }
            if (n < 0) {
                break;
            }
        }
        in_write.close_now();
    }

    set_nonblocking(out_read.value);
    set_nonblocking(err_read.value);

    ProcessResult result;
    bool stdout_open = true;
    bool stderr_open = true;
    bool child_done = false;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + spec.timeout;

    while (stdout_open || stderr_open || !child_done) {
        if (!child_done) {
            const pid_t waited = ::waitpid(pid, &status, WNOHANG);
            if (waited == pid) {
                child_done = true;
            }
        }

        if (!stdout_open && !stderr_open && child_done) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            result.timed_out = true;
            (void)::kill(pid, SIGKILL);
            (void)::waitpid(pid, &status, 0);
            child_done = true;
            append_available(out_read.value, result.stdout_text, stdout_open);
            append_available(err_read.value, result.stderr_text, stderr_open);
            break;
        }

        std::array<pollfd, 2> fds{};
        nfds_t count = 0;
        if (stdout_open) {
            fds[count++] = pollfd{out_read.value, POLLIN | POLLHUP | POLLERR, 0};
        }
        if (stderr_open) {
            fds[count++] = pollfd{err_read.value, POLLIN | POLLHUP | POLLERR, 0};
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int wait_ms = static_cast<int>(std::clamp<long long>(remaining.count(), 1, 100));
        const int poll_result = count == 0 ? 0 : ::poll(fds.data(), count, wait_ms);
        if (poll_result < 0 && errno != EINTR) {
            break;
        }

        nfds_t index = 0;
        if (stdout_open) {
            if (count == 0 || fds[index].revents != 0) {
                append_available(out_read.value, result.stdout_text, stdout_open);
            }
            ++index;
        }
        if (stderr_open) {
            if (count == 0 || fds[index].revents != 0) {
                append_available(err_read.value, result.stderr_text, stderr_open);
            }
        }
    }

    if (!child_done) {
        (void)::waitpid(pid, &status, 0);
    }

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.signaled = true;
        result.signal_number = WTERMSIG(status);
        result.exit_code = 128 + result.signal_number;
    }

    return Result<ProcessResult>::success(std::move(result));
}

std::string describe_process_result(const ProcessResult& result)
{
    if (result.timed_out) {
        return "timed out";
    }
    if (result.signaled) {
        return "terminated by signal " + std::to_string(result.signal_number);
    }
    return "exit code " + std::to_string(result.exit_code);
}

} // namespace jjmcp
