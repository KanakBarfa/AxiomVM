#include <array>
#include <cstdio>
#include <cstdlib>
#include <sys/reboot.h>
#include <unistd.h>
#include <utility>

#include "axiom/bootstrap.hpp"
#include "axiom/common.hpp"
#include "axiom/server.hpp"
#include "axiom/signalfd.hpp"
#include "axiom/vsock.hpp"

/// Entrypoint for guest PID 1 appliance daemon.
auto main(int argc, char* argv[]) -> int {
    (void)argc;
    (void)argv;

    pid_t current_pid = getpid();
    std::printf("[axiom-initd] Starting AxiomVM guest appliance (PID: %d)\n", current_pid);

    if (current_pid == 1) {
        auto mount_res = axiom::init::mount_essential_filesystems();
        if (!mount_res) {
            std::fprintf(stderr, "[axiom-initd] Warning: Failed to mount filesystems: %.*s\n",
                         static_cast<int>(axiom::to_string_view(mount_res.error()).size()),
                         axiom::to_string_view(mount_res.error()).data());
        } else {
            std::printf("[axiom-initd] Essential filesystems mounted successfully\n");
        }
    }

    const std::array<int, 3> trapped_signals{SIGCHLD, SIGTERM, SIGINT};
    auto signal_res = axiom::SignalFd::create(trapped_signals);
    if (!signal_res) {
        std::fprintf(stderr, "[axiom-initd] Fatal: Failed to initialize signalfd: %.*s\n",
                     static_cast<int>(axiom::to_string_view(signal_res.error()).size()),
                     axiom::to_string_view(signal_res.error()).data());
        return EXIT_FAILURE;
    }

    auto vsock_res = axiom::vsock::create_listener(axiom::vsock::DEFAULT_GUEST_PORT);
    if (!vsock_res) {
        std::fprintf(stderr,
                     "[axiom-initd] Fatal: Failed to bind vsock listener on port %u: %.*s\n",
                     axiom::vsock::DEFAULT_GUEST_PORT,
                     static_cast<int>(axiom::to_string_view(vsock_res.error()).size()),
                     axiom::to_string_view(vsock_res.error()).data());
        return EXIT_FAILURE;
    }

    int listener_fd = *vsock_res;
    std::printf("[axiom-initd] Virtio-vsock listener established on port %u\n",
                axiom::vsock::DEFAULT_GUEST_PORT);

    static axiom::server::ApplianceServer server;
    auto init_res = server.init(listener_fd, std::move(*signal_res));
    if (!init_res) {
        std::fprintf(stderr, "[axiom-initd] Fatal: Server initialization failed: %.*s\n",
                     static_cast<int>(axiom::to_string_view(init_res.error()).size()),
                     axiom::to_string_view(init_res.error()).data());
        close(listener_fd);
        return EXIT_FAILURE;
    }

    std::printf("[axiom-initd] Entering steady-state reactor loop\n");
    auto run_res = server.run();
    if (!run_res) {
        std::fprintf(stderr, "[axiom-initd] Server reactor returned error: %.*s\n",
                     static_cast<int>(axiom::to_string_view(run_res.error()).size()),
                     axiom::to_string_view(run_res.error()).data());
    }

    close(listener_fd);
    std::printf("[axiom-initd] Appliance runtime shutting down\n");

    if (current_pid == 1) {
        sync();
        reboot(RB_POWER_OFF);
    }

    return EXIT_SUCCESS;
}
