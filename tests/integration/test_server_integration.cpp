#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "axiom/common.hpp"
#include "axiom/server.hpp"
#include "axiom/signalfd.hpp"
#include "axiom/wire.hpp"

/// Runs an end-to-end integration test verifying TDS-Wire frame exchange and server shutdown
/// lifecycle.
auto main() -> int {
    alarm(30);

    const char* socket_path = "/tmp/axiom_test_server.sock";
    unlink(socket_path);

    int listener_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener_fd < 0) {
        std::perror("socket AF_UNIX failed");
        return EXIT_FAILURE;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(listener_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::perror("bind failed");
        close(listener_fd);
        return EXIT_FAILURE;
    }

    if (listen(listener_fd, 4) != 0) {
        std::perror("listen failed");
        close(listener_fd);
        unlink(socket_path);
        return EXIT_FAILURE;
    }

    const std::array<int, 3> watched_signals{SIGCHLD, SIGTERM, SIGINT};
    auto sfd_res = axiom::SignalFd::create(watched_signals);
    if (!sfd_res) {
        std::fprintf(stderr, "SignalFd::create failed\n");
        close(listener_fd);
        unlink(socket_path);
        return EXIT_FAILURE;
    }

    axiom::server::ApplianceServer server;
    auto init_res = server.init(listener_fd, std::move(*sfd_res));
    if (!init_res) {
        std::fprintf(stderr, "server.init failed\n");
        close(listener_fd);
        unlink(socket_path);
        return EXIT_FAILURE;
    }

    pid_t child_pid = fork();
    if (child_pid < 0) {
        std::perror("fork failed");
        close(listener_fd);
        unlink(socket_path);
        return EXIT_FAILURE;
    }

    if (child_pid == 0) {
        auto fail_exit = [](int fd, int code) {
            if (fd >= 0) {
                close(fd);
            }
            kill(getppid(), SIGINT);
            _exit(code);
        };

        // In client child process: connect to server, send Ping, receive Pong, send Shutdown
        int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (client_fd < 0) {
            fail_exit(-1, 1);
        }

        // Allow parent server reactor to start polling
        usleep(50000);

        sockaddr_un client_target{};
        client_target.sun_family = AF_UNIX;
        std::strncpy(client_target.sun_path, socket_path, sizeof(client_target.sun_path) - 1);

        if (connect(client_fd, reinterpret_cast<sockaddr*>(&client_target),
                    sizeof(client_target)) != 0) {
            fail_exit(client_fd, 2);
        }

        // Send Ping frame
        axiom::wire::FrameHeader ping_header{
            .magic = axiom::wire::WIRE_MAGIC,
            .opcode = static_cast<uint16_t>(axiom::wire::Opcode::Ping),
            .request_id = 0x1337,
            .payload_len = 0,
        };

        std::array<uint8_t, axiom::wire::HEADER_SIZE> tx_buf{};
        auto enc_res = axiom::wire::encode_header(ping_header, tx_buf);
        if (!enc_res) {
            fail_exit(client_fd, 3);
        }

        if (write(client_fd, tx_buf.data(), tx_buf.size()) != static_cast<ssize_t>(tx_buf.size())) {
            fail_exit(client_fd, 4);
        }

        // Read Pong frame
        std::array<uint8_t, axiom::wire::HEADER_SIZE> rx_buf{};
        ssize_t bytes_read = read(client_fd, rx_buf.data(), rx_buf.size());
        if (bytes_read != static_cast<ssize_t>(rx_buf.size())) {
            fail_exit(client_fd, 5);
        }

        auto dec_res = axiom::wire::decode_header(rx_buf);
        if (!dec_res) {
            fail_exit(client_fd, 6);
        }

        const auto& pong = *dec_res;
        if (pong.magic != axiom::wire::WIRE_MAGIC ||
            pong.opcode != static_cast<uint16_t>(axiom::wire::Opcode::Pong) ||
            pong.request_id != 0x1337 || pong.payload_len != 0) {
            fail_exit(client_fd, 7);
        }

        // Send Shutdown frame
        axiom::wire::FrameHeader shutdown_header{
            .magic = axiom::wire::WIRE_MAGIC,
            .opcode = static_cast<uint16_t>(axiom::wire::Opcode::Shutdown),
            .request_id = 0x1338,
            .payload_len = 0,
        };

        enc_res = axiom::wire::encode_header(shutdown_header, tx_buf);
        if (!enc_res) {
            fail_exit(client_fd, 8);
        }

        if (write(client_fd, tx_buf.data(), tx_buf.size()) != static_cast<ssize_t>(tx_buf.size())) {
            fail_exit(client_fd, 9);
        }

        // Wait for server to close socket upon shutdown
        char dummy = 0;
        while (read(client_fd, &dummy, 1) > 0) {
        }
        close(client_fd);
        _exit(0);
    }

    // In server parent process: run reactor until client requests shutdown
    auto run_res = server.run();
    close(listener_fd);
    unlink(socket_path);

    if (!run_res) {
        std::fprintf(stderr, "server.run failed\n");
        return EXIT_FAILURE;
    }

    int child_status = 0;
    pid_t wp = waitpid(child_pid, &child_status, 0);
    if (wp < 0 && errno != ECHILD) {
        std::perror("waitpid failed");
        return EXIT_FAILURE;
    }

    if (wp > 0 && (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)) {
        std::fprintf(stderr, "Child exited with error status: %d\n", WEXITSTATUS(child_status));
        return EXIT_FAILURE;
    }

    std::printf("[integration-test] Server frame exchange and shutdown completed successfully\n");
    return EXIT_SUCCESS;
}
