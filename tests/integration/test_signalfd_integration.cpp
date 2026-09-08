#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

#include "axiom/common.hpp"
#include "axiom/reactor.hpp"
#include "axiom/signalfd.hpp"

/// Runs an end-to-end integration test verifying signalfd event notifications through epoll
/// reactor.
auto main() -> int {
    const std::array<int, 2> watched_signals{SIGCHLD, SIGUSR1};
    auto sfd_res = axiom::SignalFd::create(watched_signals);
    if (!sfd_res) {
        std::fprintf(stderr, "Failed to create signalfd\n");
        return EXIT_FAILURE;
    }
    axiom::SignalFd sfd = std::move(*sfd_res);

    auto reactor_res = axiom::Reactor::create();
    if (!reactor_res) {
        std::fprintf(stderr, "Failed to create reactor\n");
        return EXIT_FAILURE;
    }
    axiom::Reactor reactor = std::move(*reactor_res);

    auto add_res = reactor.add(sfd.fd(), EPOLLIN);
    if (!add_res) {
        std::fprintf(stderr, "Failed to add signalfd to reactor\n");
        return EXIT_FAILURE;
    }

    pid_t child_pid = fork();
    if (child_pid < 0) {
        std::perror("fork failed");
        return EXIT_FAILURE;
    }

    if (child_pid == 0) {
        // In child process: raise SIGUSR1 to parent, then terminate to produce SIGCHLD
        kill(getppid(), SIGUSR1);
        _exit(42);
    }

    // In parent process: wait for epoll events and verify signals received
    bool received_sigusr1 = false;
    bool received_sigchld = false;
    int iterations = 0;

    while ((!received_sigusr1 || !received_sigchld) && iterations < 10) {
        ++iterations;
        std::array<epoll_event, 4> events{};
        auto wait_res = reactor.wait(events, 1000);
        if (!wait_res || *wait_res == 0) {
            std::fprintf(stderr, "epoll wait timeout waiting for signals\n");
            break;
        }

        std::array<signalfd_siginfo, 4> sig_info{};
        auto read_res = sfd.read_signals(sig_info);
        if (!read_res) {
            std::fprintf(stderr, "Failed to read signals\n");
            return EXIT_FAILURE;
        }

        size_t count = *read_res;
        for (size_t i = 0; i < count; ++i) {
            if (sig_info[i].ssi_signo == SIGUSR1) {
                received_sigusr1 = true;
            } else if (sig_info[i].ssi_signo == SIGCHLD) {
                received_sigchld = true;
                int status = 0;
                pid_t reaped = waitpid(static_cast<pid_t>(sig_info[i].ssi_pid), &status, WNOHANG);
                if (reaped != child_pid || !WIFEXITED(status) || WEXITSTATUS(status) != 42) {
                    std::fprintf(stderr, "Child process exit status mismatch\n");
                    return EXIT_FAILURE;
                }
            }
        }
    }

    if (!received_sigusr1 || !received_sigchld) {
        std::fprintf(stderr, "Did not receive all expected signals\n");
        return EXIT_FAILURE;
    }

    std::printf("[integration-test] Signalfd and reactor integration test succeeded\n");
    return EXIT_SUCCESS;
}
