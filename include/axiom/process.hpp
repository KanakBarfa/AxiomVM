#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <span>
#include <string_view>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "axiom/ansi.hpp"
#include "axiom/common.hpp"
#include "axiom/diff.hpp"
#include "axiom/reactor.hpp"
#include "axiom/ring_buffer.hpp"
#include "axiom/wire.hpp"

namespace axiom::exec {

inline constexpr size_t RING_BUFFER_CAPACITY = 65536;

#ifndef CLONE_PIDFD
#define CLONE_PIDFD 0x00001000
#endif

#ifndef P_PIDFD
#define P_PIDFD 3
#endif

#ifndef __NR_clone3
#if defined(__x86_64__)
#define __NR_clone3 435
#elif defined(__aarch64__)
#define __NR_clone3 435
#endif
#endif

#ifndef __NR_pidfd_send_signal
#if defined(__x86_64__)
#define __NR_pidfd_send_signal 424
#elif defined(__aarch64__)
#define __NR_pidfd_send_signal 424
#endif
#endif

#ifndef __NR_pidfd_open
#if defined(__x86_64__)
#define __NR_pidfd_open 434
#elif defined(__aarch64__)
#define __NR_pidfd_open 434
#endif
#endif

/// Kernel clone arguments structure matching Linux clone3 system call.
struct [[gnu::packed]] KernelCloneArgs {
    uint64_t flags{0};
    uint64_t pidfd{0};
    uint64_t child_tid{0};
    uint64_t parent_tid{0};
    uint64_t exit_signal{0};
    uint64_t stack{0};
    uint64_t stack_size{0};
    uint64_t tls{0};
    uint64_t set_tid{0};
    uint64_t set_tid_size{0};
    uint64_t cgroup{0};
};

/// Reads architectural CPU cycle counter for execution profiling.
[[nodiscard]] inline auto read_cpu_cycles() noexcept -> uint64_t {
#if defined(__x86_64__) || defined(_M_X64)
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | static_cast<uint64_t>(lo);
#elif defined(__aarch64__)
    uint64_t val = 0;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
#endif
}

/// Parameters configuring execution of a sandboxed child process.
struct ExecOptions {
    std::string_view command{};
    std::string_view working_dir{"/tmp"};
    uint32_t timeout_ms{5000};
    uint32_t flags{wire::EXEC_FLAG_STRIP_ANSI | wire::EXEC_FLAG_CAPTURE_DIFF};
};

/// Summary result generated following child process completion.
struct ExecResult {
    int32_t exit_code{-1};
    uint32_t flags{0};
    uint64_t cpu_cycles{0};
    size_t output_len{0};
    size_t diff_len{0};
};

/// Executes commands in isolated child processes with epoll reactor tracking.
class ProcessRunner {
public:
    ProcessRunner() noexcept = default;

    /// Runs a child command and writes filtered output and diffs to provided spans.
    auto run(const ExecOptions& options, std::span<char> output_dest,
             std::span<char> diff_dest) noexcept -> Result<ExecResult> {
        ring_buffer_.clear();
        ansi_filter_.reset();

        diff::DirectorySnapshot pre_snapshot{};
        diff::DirectorySnapshot post_snapshot{};

        std::string_view watch_dir =
            options.working_dir.empty() ? std::string_view("/tmp") : options.working_dir;

        if ((options.flags & wire::EXEC_FLAG_CAPTURE_DIFF) != 0) {
            auto snap_res = pre_snapshot.capture(watch_dir);
            if (!snap_res) {
                (void)snap_res;
            }
        }

        int pipe_fds[2]{-1, -1};
        if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
            return std::unexpected(SystemError::ProcessSpawnError);
        }

        uint64_t start_cycles = read_cpu_cycles();

        int pidfd = -1;
        KernelCloneArgs cl_args{};
        cl_args.flags = CLONE_PIDFD;
        cl_args.exit_signal = SIGCHLD;
        cl_args.pidfd = reinterpret_cast<uint64_t>(&pidfd);

        pid_t child_pid = static_cast<pid_t>(syscall(__NR_clone3, &cl_args, sizeof(cl_args)));

        if (child_pid < 0 && errno == ENOSYS) {
            child_pid = fork();
            if (child_pid > 0) {
                pidfd = static_cast<int>(syscall(__NR_pidfd_open, child_pid, 0));
            }
        }

        if (child_pid < 0) {
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            return std::unexpected(SystemError::ProcessSpawnError);
        }

        if (child_pid == 0) {
            execute_child(options, pipe_fds);
            _exit(127);
        }

        close(pipe_fds[1]);

        int fl = fcntl(pipe_fds[0], F_GETFL, 0);
        if (fl >= 0) {
            (void)fcntl(pipe_fds[0], F_SETFL, fl | O_NONBLOCK);
        }

        ExecResult result{};
        auto wait_res = monitor_child(child_pid, pidfd, pipe_fds[0], options.timeout_ms, result);
        if (!wait_res) {
            close(pipe_fds[0]);
            if (pidfd >= 0) {
                close(pidfd);
            }
            return std::unexpected(wait_res.error());
        }

        close(pipe_fds[0]);
        if (pidfd >= 0) {
            close(pidfd);
        }

        uint64_t end_cycles = read_cpu_cycles();
        result.cpu_cycles = (end_cycles >= start_cycles) ? (end_cycles - start_cycles) : 0;

        if (ring_buffer_.is_truncated()) {
            result.flags |= wire::EXEC_RESP_TRUNCATED;
        }

        if ((options.flags & wire::EXEC_FLAG_STRIP_ANSI) != 0) {
            std::array<char, RING_BUFFER_CAPACITY> raw_linear{};
            size_t raw_len = ring_buffer_.linearize(raw_linear);
            result.output_len =
                ansi_filter_.filter(std::string_view(raw_linear.data(), raw_len), output_dest);
        } else {
            result.output_len = ring_buffer_.linearize(output_dest);
        }

        if ((options.flags & wire::EXEC_FLAG_CAPTURE_DIFF) != 0) {
            auto post_res = post_snapshot.capture(watch_dir);
            if (post_res) {
                result.diff_len = diff::compute_diff(pre_snapshot, post_snapshot, diff_dest);
            }
        }

        return result;
    }

private:
    RingBuffer<RING_BUFFER_CAPACITY> ring_buffer_{};
    ansi::AnsiFilter ansi_filter_{};

    static void execute_child(const ExecOptions& options, const int pipe_fds[2]) noexcept {
        sigset_t unblock_mask;
        sigemptyset(&unblock_mask);
        sigprocmask(SIG_SETMASK, &unblock_mask, nullptr);

        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);

        if (!options.working_dir.empty()) {
            char cwd_buf[256];
            size_t len = std::min(options.working_dir.size(), sizeof(cwd_buf) - 1);
            std::memcpy(cwd_buf, options.working_dir.data(), len);
            cwd_buf[len] = '\0';
            if (chdir(cwd_buf) != 0) {
                // Ignore error in child process if working directory change fails
            }
        }

        char cmd_buf[512];
        size_t cmd_len = std::min(options.command.size(), sizeof(cmd_buf) - 1);
        std::memcpy(cmd_buf, options.command.data(), cmd_len);
        cmd_buf[cmd_len] = '\0';

        const char* const envp[] = {
            "PATH=/bin:/sbin:/usr/bin:/usr/sbin",
            "HOME=/root",
            "TERM=dumb",
            nullptr,
        };

        if (access("/bin/sh", X_OK) == 0) {
            execl("/bin/sh", "sh", "-c", cmd_buf, nullptr);
        } else {
            char* argv[] = {cmd_buf, nullptr};
            execve(cmd_buf, argv, const_cast<char* const*>(envp));
        }
    }

    void drain_pipe(int fd) noexcept {
        std::array<uint8_t, 1024> chunk{};
        while (true) {
            ssize_t n = read(fd, chunk.data(), chunk.size());
            if (n > 0) {
                (void)ring_buffer_.write(
                    std::span<const uint8_t>(chunk.data(), static_cast<size_t>(n)));
            } else {
                break;
            }
        }
    }

    auto monitor_child(pid_t child_pid, int pidfd, int pipe_fd, uint32_t timeout_ms,
                       ExecResult& result) noexcept -> Result<void> {
        auto reactor_res = Reactor::create();
        if (!reactor_res) {
            return std::unexpected(reactor_res.error());
        }
        Reactor reactor = std::move(*reactor_res);

        if (pidfd >= 0) {
            auto add_res = reactor.add(pidfd, EPOLLIN);
            if (!add_res) {
                return std::unexpected(add_res.error());
            }
        }

        auto pipe_add = reactor.add(pipe_fd, EPOLLIN | EPOLLHUP);
        if (!pipe_add) {
            return std::unexpected(pipe_add.error());
        }

        bool child_done = false;
        bool pipe_closed = false;
        std::array<epoll_event, 8> events{};
        auto start_time = std::chrono::steady_clock::now();

        while (!child_done || !pipe_closed) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
            int remaining_ms = static_cast<int>(timeout_ms) - static_cast<int>(elapsed_ms);

            if (remaining_ms <= 0) {
                result.flags |= wire::EXEC_RESP_TIMED_OUT;
                kill_child(child_pid, pidfd);
                remaining_ms = 500;
            }

            auto wait_res = reactor.wait(events, std::max(10, remaining_ms));
            if (!wait_res) {
                break;
            }

            size_t count = *wait_res;
            if (count == 0 && remaining_ms <= 10) {
                result.flags |= wire::EXEC_RESP_TIMED_OUT;
                kill_child(child_pid, pidfd);
            }

            for (size_t i = 0; i < count; ++i) {
                int event_fd = events[i].data.fd;
                uint32_t event_flags = events[i].events;

                if (event_fd == pipe_fd) {
                    if ((event_flags & EPOLLIN) != 0) {
                        drain_pipe(pipe_fd);
                    }
                    if ((event_flags & (EPOLLHUP | EPOLLERR)) != 0) {
                        drain_pipe(pipe_fd);
                        pipe_closed = true;
                        (void)reactor.remove(pipe_fd);
                    }
                } else if (pidfd >= 0 && event_fd == pidfd) {
                    siginfo_t info{};
                    std::memset(&info, 0, sizeof(info));
                    if (waitid(static_cast<idtype_t>(P_PIDFD), static_cast<id_t>(pidfd), &info,
                               WEXITED | WNOHANG) == 0 &&
                        info.si_pid == child_pid) {
                        child_done = true;
                        if (info.si_code == CLD_EXITED) {
                            result.exit_code = info.si_status;
                        } else {
                            result.exit_code = 128 + info.si_status;
                        }
                        (void)reactor.remove(pidfd);
                    }
                }
            }

            if (!child_done && pidfd < 0) {
                int status = 0;
                pid_t reaped = waitpid(child_pid, &status, WNOHANG);
                if (reaped == child_pid) {
                    child_done = true;
                    if (WIFEXITED(status)) {
                        result.exit_code = WEXITSTATUS(status);
                    } else if (WIFSIGNALED(status)) {
                        result.exit_code = 128 + WTERMSIG(status);
                    }
                }
            }

            if (child_done && !pipe_closed) {
                drain_pipe(pipe_fd);
                pipe_closed = true;
                (void)reactor.remove(pipe_fd);
            }
        }

        if (!child_done) {
            int status = 0;
            waitpid(child_pid, &status, WNOHANG);
        }

        return {};
    }

    static void kill_child(pid_t child_pid, int pidfd) noexcept {
        if (pidfd >= 0) {
            syscall(__NR_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0);
        } else {
            kill(child_pid, SIGKILL);
        }
    }
};

} // namespace axiom::exec
