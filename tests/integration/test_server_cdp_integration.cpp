#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include "axiom/cdp.hpp"
#include "axiom/common.hpp"
#include "axiom/server.hpp"
#include "axiom/signalfd.hpp"
#include "axiom/wire.hpp"

namespace {

/// Runs ApplianceServer event loop on background thread.
void run_server_loop(axiom::server::ApplianceServer* server) {
    auto res = server->run();
    (void)res;
}

/// Tests client-server TDS-Wire frame exchange for browser CDP actions.
void test_server_cdp_dispatch() {
    const char* socket_path = "/tmp/axiom_test_server_cdp.sock";
    unlink(socket_path);

    int listener_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener_fd < 0) {
        std::abort();
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(listener_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(listener_fd);
        std::abort();
    }

    if (listen(listener_fd, 4) != 0) {
        close(listener_fd);
        unlink(socket_path);
        std::abort();
    }

    std::array<int, 3> sigs = {SIGCHLD, SIGTERM, SIGINT};
    auto sig_res = axiom::SignalFd::create(sigs);
    if (!sig_res) {
        close(listener_fd);
        unlink(socket_path);
        std::abort();
    }

    axiom::server::ApplianceServer server{};
    auto init_res = server.init(listener_fd, std::move(*sig_res));
    if (!init_res) {
        close(listener_fd);
        unlink(socket_path);
        std::abort();
    }

    std::thread server_thread(run_server_loop, &server);

    // Client connection
    int client_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (client_fd < 0) {
        server.stop();
        server_thread.join();
        close(listener_fd);
        unlink(socket_path);
        std::abort();
    }

    if (connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(client_fd);
        server.stop();
        server_thread.join();
        close(listener_fd);
        unlink(socket_path);
        std::abort();
    }

    // Connect client to server loopback
    // Send Ping first to verify socket is live
    axiom::wire::FrameHeader ping_hdr{
        .magic = axiom::wire::WIRE_MAGIC,
        .opcode = static_cast<uint16_t>(axiom::wire::Opcode::Ping),
        .request_id = 1,
        .payload_len = 0,
    };
    std::array<uint8_t, axiom::wire::HEADER_SIZE> ping_buf{};
    auto enc_ping = axiom::wire::encode_header(ping_hdr, ping_buf);
    if (!enc_ping) {
        std::abort();
    }
    if (write(client_fd, ping_buf.data(), ping_buf.size()) !=
        static_cast<ssize_t>(ping_buf.size())) {
        std::abort();
    }

    std::array<uint8_t, axiom::wire::HEADER_SIZE> pong_buf{};
    ssize_t n = 0;
    while (n < static_cast<ssize_t>(pong_buf.size())) {
        ssize_t rd = read(client_fd, pong_buf.data() + n, pong_buf.size() - static_cast<size_t>(n));
        if (rd > 0) {
            n += rd;
        } else if (rd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(1000);
        }
    }
    auto pong_hdr = axiom::wire::decode_header(pong_buf);
    if (!pong_hdr || pong_hdr->opcode != static_cast<uint16_t>(axiom::wire::Opcode::Pong)) {
        std::abort();
    }

    // Test CdpAction: GetTree
    axiom::wire::CdpActionRequestHeader req_hdr{
        .action = static_cast<uint8_t>(axiom::wire::CdpActionType::GetTree),
        .flags = 0,
        .target_id = 0,
        .scroll_delta = 0,
        .payload1_len = 0,
        .payload2_len = 0,
    };

    axiom::wire::FrameHeader frame_hdr{
        .magic = axiom::wire::WIRE_MAGIC,
        .opcode = static_cast<uint16_t>(axiom::wire::Opcode::CdpAction),
        .request_id = 101,
        .payload_len = axiom::wire::CDP_REQ_HEADER_SIZE,
    };

    std::array<uint8_t, axiom::wire::HEADER_SIZE + axiom::wire::CDP_REQ_HEADER_SIZE> tx_buf{};
    auto enc1 =
        axiom::wire::encode_header(frame_hdr, std::span(tx_buf.data(), axiom::wire::HEADER_SIZE));
    auto enc2 =
        axiom::wire::encode_cdp_request(req_hdr, std::span(tx_buf.data() + axiom::wire::HEADER_SIZE,
                                                           axiom::wire::CDP_REQ_HEADER_SIZE));
    if (!enc1 || !enc2) {
        std::abort();
    }

    if (write(client_fd, tx_buf.data(), tx_buf.size()) != static_cast<ssize_t>(tx_buf.size())) {
        std::abort();
    }

    // Read response frame header
    std::array<uint8_t, axiom::wire::HEADER_SIZE> resp_frame_buf{};
    n = 0;
    while (n < static_cast<ssize_t>(resp_frame_buf.size())) {
        ssize_t rd = read(client_fd, resp_frame_buf.data() + n,
                          resp_frame_buf.size() - static_cast<size_t>(n));
        if (rd > 0) {
            n += rd;
        } else if (rd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(1000);
        }
    }

    auto out_frame_hdr = axiom::wire::decode_header(resp_frame_buf);
    if (!out_frame_hdr ||
        out_frame_hdr->opcode != static_cast<uint16_t>(axiom::wire::Opcode::CdpActionResponse) ||
        out_frame_hdr->request_id != 101) {
        std::abort();
    }

    // Read response payload
    std::array<uint8_t, 1024> resp_payload_buf{};
    size_t to_read = out_frame_hdr->payload_len;
    if (to_read < axiom::wire::CDP_RESP_HEADER_SIZE || to_read > resp_payload_buf.size()) {
        std::abort();
    }
    n = 0;
    while (n < static_cast<ssize_t>(to_read)) {
        ssize_t rd = read(client_fd, resp_payload_buf.data() + n, to_read - static_cast<size_t>(n));
        if (rd > 0) {
            n += rd;
        } else if (rd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(1000);
        }
    }

    auto cdp_resp = axiom::wire::decode_cdp_response(
        std::span(resp_payload_buf.data(), axiom::wire::CDP_RESP_HEADER_SIZE));
    if (!cdp_resp) {
        std::abort();
    }

    std::cout << "[test_server_cdp] GetTree response status: " << cdp_resp->status
              << " node_count: " << cdp_resp->node_count << "\n";

    // Test CdpAction: Scroll
    axiom::wire::CdpActionRequestHeader scroll_req_hdr{
        .action = static_cast<uint8_t>(axiom::wire::CdpActionType::Scroll),
        .flags = 0,
        .target_id = 0,
        .scroll_delta = 300,
        .payload1_len = 0,
        .payload2_len = 0,
    };

    axiom::wire::FrameHeader scroll_frame_hdr{
        .magic = axiom::wire::WIRE_MAGIC,
        .opcode = static_cast<uint16_t>(axiom::wire::Opcode::CdpAction),
        .request_id = 102,
        .payload_len = axiom::wire::CDP_REQ_HEADER_SIZE,
    };

    auto enc_s1 = axiom::wire::encode_header(scroll_frame_hdr,
                                             std::span(tx_buf.data(), axiom::wire::HEADER_SIZE));
    auto enc_s2 = axiom::wire::encode_cdp_request(
        scroll_req_hdr,
        std::span(tx_buf.data() + axiom::wire::HEADER_SIZE, axiom::wire::CDP_REQ_HEADER_SIZE));
    if (!enc_s1 || !enc_s2) {
        std::abort();
    }

    if (write(client_fd, tx_buf.data(), tx_buf.size()) != static_cast<ssize_t>(tx_buf.size())) {
        std::abort();
    }

    n = 0;
    while (n < static_cast<ssize_t>(resp_frame_buf.size())) {
        ssize_t rd = read(client_fd, resp_frame_buf.data() + n,
                          resp_frame_buf.size() - static_cast<size_t>(n));
        if (rd > 0) {
            n += rd;
        } else if (rd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(1000);
        }
    }

    out_frame_hdr = axiom::wire::decode_header(resp_frame_buf);
    if (!out_frame_hdr ||
        out_frame_hdr->opcode != static_cast<uint16_t>(axiom::wire::Opcode::CdpActionResponse) ||
        out_frame_hdr->request_id != 102) {
        std::abort();
    }

    // Send Shutdown
    server.stop();
    server_thread.join();
    close(client_fd);
    close(listener_fd);
    unlink(socket_path);

    std::cout << "[test_server_cdp] Server CDP dispatch integration test passed successfully.\n";
}

} // namespace

int main() {
    alarm(30);
    test_server_cdp_dispatch();
    std::cout << "[test_server_cdp_integration] All integration tests passed.\n";
    return 0;
}
