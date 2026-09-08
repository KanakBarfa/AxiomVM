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

/// Asserts condition or exits with failure.
static void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "[FAIL] %s\n", msg);
        std::exit(EXIT_FAILURE);
    }
}

/// End-to-end integration test verifying TDS-Wire Exec frame exchange and filtered output RPC.
auto main() -> int {
    alarm(30);

    const char* socket_path = "/tmp/axiom_test_server_exec.sock";
    unlink(socket_path);

    int listener_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    check(listener_fd >= 0, "socket AF_UNIX failed");

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    check(bind(listener_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind failed");
    check(listen(listener_fd, 4) == 0, "listen failed");

    const std::array<int, 3> watched_signals{SIGCHLD, SIGTERM, SIGINT};
    auto sfd_res = axiom::SignalFd::create(watched_signals);
    check(sfd_res.has_value(), "SignalFd::create failed");

    static axiom::server::ApplianceServer server;
    auto init_res = server.init(listener_fd, std::move(*sfd_res));
    check(init_res.has_value(), "server.init failed");

    pid_t child_pid = fork();
    check(child_pid >= 0, "fork failed");

    if (child_pid == 0) {
        auto fail_exit = [](int fd, int code) {
            if (fd >= 0) {
                close(fd);
            }
            kill(getppid(), SIGINT);
            _exit(code);
        };

        int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (client_fd < 0) {
            fail_exit(-1, 1);
        }

        usleep(50000);

        sockaddr_un client_target{};
        client_target.sun_family = AF_UNIX;
        std::strncpy(client_target.sun_path, socket_path, sizeof(client_target.sun_path) - 1);

        if (connect(client_fd, reinterpret_cast<sockaddr*>(&client_target),
                    sizeof(client_target)) != 0) {
            fail_exit(client_fd, 2);
        }

        // Send Exec frame: command with ANSI colors
        const char* cmd = "printf '\\033[1;34mHeader:\\033[0m Content\\n'";
        uint16_t cmd_len = static_cast<uint16_t>(std::strlen(cmd));
        const char* cwd = "/tmp";
        uint16_t cwd_len = static_cast<uint16_t>(std::strlen(cwd));

        axiom::wire::ExecRequestHeader req_hdr{
            .flags = axiom::wire::EXEC_FLAG_STRIP_ANSI,
            .timeout_ms = 5000,
            .cmd_len = cmd_len,
            .cwd_len = cwd_len,
        };

        uint32_t payload_len =
            static_cast<uint32_t>(axiom::wire::EXEC_REQ_HEADER_SIZE) + cmd_len + cwd_len;
        axiom::wire::FrameHeader exec_frame{
            .magic = axiom::wire::WIRE_MAGIC,
            .opcode = static_cast<uint16_t>(axiom::wire::Opcode::Exec),
            .request_id = 0x4242,
            .payload_len = payload_len,
        };

        std::array<uint8_t, 512> tx_buf{};
        auto enc1 = axiom::wire::encode_header(exec_frame,
                                               std::span(tx_buf.data(), axiom::wire::HEADER_SIZE));
        auto enc2 = axiom::wire::encode_exec_request(
            req_hdr,
            std::span(tx_buf.data() + axiom::wire::HEADER_SIZE, axiom::wire::EXEC_REQ_HEADER_SIZE));
        if (!enc1 || !enc2) {
            fail_exit(client_fd, 3);
        }

        std::memcpy(tx_buf.data() + axiom::wire::HEADER_SIZE + axiom::wire::EXEC_REQ_HEADER_SIZE,
                    cmd, cmd_len);
        std::memcpy(tx_buf.data() + axiom::wire::HEADER_SIZE + axiom::wire::EXEC_REQ_HEADER_SIZE +
                        cmd_len,
                    cwd, cwd_len);

        size_t total_tx = axiom::wire::HEADER_SIZE + payload_len;
        if (write(client_fd, tx_buf.data(), total_tx) != static_cast<ssize_t>(total_tx)) {
            fail_exit(client_fd, 4);
        }

        // Read ExecOutput response frame header
        std::array<uint8_t, axiom::wire::HEADER_SIZE> rx_hdr_buf{};
        if (read(client_fd, rx_hdr_buf.data(), rx_hdr_buf.size()) !=
            static_cast<ssize_t>(rx_hdr_buf.size())) {
            fail_exit(client_fd, 5);
        }

        auto dec_hdr = axiom::wire::decode_header(rx_hdr_buf);
        if (!dec_hdr || dec_hdr->opcode != static_cast<uint16_t>(axiom::wire::Opcode::ExecOutput) ||
            dec_hdr->request_id != 0x4242) {
            fail_exit(client_fd, 6);
        }

        // Read ExecOutput response payload
        std::array<uint8_t, 1024> rx_payload{};
        size_t payload_read = 0;
        while (payload_read < dec_hdr->payload_len) {
            ssize_t n = read(client_fd, rx_payload.data() + payload_read,
                             dec_hdr->payload_len - payload_read);
            if (n <= 0) {
                fail_exit(client_fd, 7);
            }
            payload_read += static_cast<size_t>(n);
        }

        auto dec_resp = axiom::wire::decode_exec_response(rx_payload);
        if (!dec_resp || dec_resp->exit_code != 0 || dec_resp->cpu_cycles == 0) {
            fail_exit(client_fd, 8);
        }

        const char* out_data =
            reinterpret_cast<const char*>(rx_payload.data() + axiom::wire::EXEC_RESP_HEADER_SIZE);
        std::string_view out_view(out_data, dec_resp->output_len);
        if (out_view != "Header: Content\n") {
            fail_exit(client_fd, 9);
        }

        // Send Shutdown frame
        axiom::wire::FrameHeader shut_hdr{
            .magic = axiom::wire::WIRE_MAGIC,
            .opcode = static_cast<uint16_t>(axiom::wire::Opcode::Shutdown),
            .request_id = 0x9999,
            .payload_len = 0,
        };
        (void)axiom::wire::encode_header(shut_hdr, rx_hdr_buf);
        if (write(client_fd, rx_hdr_buf.data(), rx_hdr_buf.size()) !=
            static_cast<ssize_t>(rx_hdr_buf.size())) {
            fail_exit(client_fd, 10);
        }

        // Wait for server to close socket upon shutdown
        char dummy = 0;
        while (read(client_fd, &dummy, 1) > 0) {
        }
        close(client_fd);
        _exit(0);
    }

    auto run_res = server.run();
    close(listener_fd);
    unlink(socket_path);

    check(run_res.has_value(), "server.run returned error");

    int status = 0;
    pid_t wp = waitpid(child_pid, &status, 0);
    check(wp > 0 || (wp < 0 && errno == ECHILD), "waitpid failed");
    if (wp > 0) {
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "Child exited with failure");
    }

    std::printf("[integration-test] Server Exec RPC integration test passed\n");
    return EXIT_SUCCESS;
}
