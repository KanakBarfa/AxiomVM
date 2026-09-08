#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "axiom/common.hpp"
#include "axiom/server.hpp"
#include "axiom/signalfd.hpp"
#include "axiom/wire.hpp"

namespace {

void check(bool condition, const char* msg) {
    if (!condition) {
        std::fprintf(stderr, "Assertion failed: %s\n", msg);
        std::abort();
    }
}

auto read_all(int fd, void* buf, size_t count) -> bool {
    auto* ptr = static_cast<uint8_t*>(buf);
    size_t total = 0;
    while (total < count) {
        ssize_t n = read(fd, ptr + total, count - total);
        if (n <= 0) {
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

/// Runs client-server RPC integration test verifying AstSymbols, AstSlice, and AstPatch.
auto main() -> int {
    alarm(30);

    const char* socket_path = "/tmp/axiom_test_ast_server.sock";
    const char* target_file = "/tmp/axiom_test_target.cpp";
    unlink(socket_path);
    unlink(target_file);

    // Prepare target C++ source file
    {
        std::ofstream ofs(target_file);
        ofs << "#include <cstdio>\n"
            << "class Calculator {\n"
            << "public:\n"
            << "    int add(int a, int b) {\n"
            << "        return a + b;\n"
            << "    }\n"
            << "    int multiply(int a, int b);\n"
            << "};\n"
            << "int main() {\n"
            << "    return 0;\n"
            << "}\n";
    }

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

    axiom::server::ApplianceServer server{};
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

        usleep(30000);

        sockaddr_un target{};
        target.sun_family = AF_UNIX;
        std::strncpy(target.sun_path, socket_path, sizeof(target.sun_path) - 1);

        if (connect(client_fd, reinterpret_cast<sockaddr*>(&target), sizeof(target)) != 0) {
            fail_exit(client_fd, 2);
        }

        // 1. Send AstSymbols request
        std::string_view path_view(target_file);
        axiom::wire::AstSymbolsRequestHeader sym_req{
            .path_len = static_cast<uint32_t>(path_view.size()),
        };
        uint32_t sym_req_payload_len =
            static_cast<uint32_t>(axiom::wire::AST_SYMBOLS_REQ_HEADER_SIZE + path_view.size());
        axiom::wire::FrameHeader f_hdr{
            .magic = axiom::wire::WIRE_MAGIC,
            .opcode = static_cast<uint16_t>(axiom::wire::Opcode::AstSymbols),
            .request_id = 101,
            .payload_len = sym_req_payload_len,
        };

        std::array<uint8_t, axiom::wire::HEADER_SIZE + axiom::wire::AST_SYMBOLS_REQ_HEADER_SIZE>
            tx_buf{};
        (void)axiom::wire::encode_header(f_hdr, std::span(tx_buf.data(), axiom::wire::HEADER_SIZE));
        (void)axiom::wire::encode_ast_symbols_request(
            sym_req, std::span(tx_buf.data() + axiom::wire::HEADER_SIZE,
                               axiom::wire::AST_SYMBOLS_REQ_HEADER_SIZE));

        if (write(client_fd, tx_buf.data(), tx_buf.size()) != static_cast<ssize_t>(tx_buf.size()) ||
            write(client_fd, path_view.data(), path_view.size()) !=
                static_cast<ssize_t>(path_view.size())) {
            fail_exit(client_fd, 3);
        }

        // Receive AstSymbols response
        std::array<uint8_t, axiom::wire::HEADER_SIZE + axiom::wire::AST_SYMBOLS_RESP_HEADER_SIZE>
            rx_hdr{};
        if (!read_all(client_fd, rx_hdr.data(), rx_hdr.size())) {
            fail_exit(client_fd, 4);
        }

        auto dec_f = axiom::wire::decode_header(std::span(rx_hdr.data(), axiom::wire::HEADER_SIZE));
        if (!dec_f ||
            dec_f->opcode != static_cast<uint16_t>(axiom::wire::Opcode::AstSymbolsResponse)) {
            fail_exit(client_fd, 5);
        }

        auto dec_sym_resp = axiom::wire::decode_ast_symbols_response(std::span(
            rx_hdr.data() + axiom::wire::HEADER_SIZE, axiom::wire::AST_SYMBOLS_RESP_HEADER_SIZE));
        if (!dec_sym_resp || dec_sym_resp->status != 0 || dec_sym_resp->symbol_count < 3) {
            fail_exit(client_fd, 6);
        }

        std::string sym_text(dec_sym_resp->payload_len, '\0');
        if (dec_sym_resp->payload_len > 0) {
            if (!read_all(client_fd, sym_text.data(), dec_sym_resp->payload_len)) {
                fail_exit(client_fd, 7);
            }
        }
        if (sym_text.find("class Calculator") == std::string::npos ||
            sym_text.find("method Calculator::add") == std::string::npos) {
            fail_exit(client_fd, 8);
        }

        // 2. Send AstSlice request
        std::string_view sym_name = "Calculator::add";
        axiom::wire::AstSliceRequestHeader slice_req{
            .path_len = static_cast<uint16_t>(path_view.size()),
            .symbol_len = static_cast<uint16_t>(sym_name.size()),
        };
        axiom::wire::FrameHeader f_slice_hdr{
            .magic = axiom::wire::WIRE_MAGIC,
            .opcode = static_cast<uint16_t>(axiom::wire::Opcode::AstSlice),
            .request_id = 102,
            .payload_len = static_cast<uint32_t>(axiom::wire::AST_SLICE_REQ_HEADER_SIZE +
                                                 path_view.size() + sym_name.size()),
        };
        std::array<uint8_t, axiom::wire::HEADER_SIZE + axiom::wire::AST_SLICE_REQ_HEADER_SIZE>
            slice_tx{};
        (void)axiom::wire::encode_header(f_slice_hdr,
                                         std::span(slice_tx.data(), axiom::wire::HEADER_SIZE));
        (void)axiom::wire::encode_ast_slice_request(
            slice_req, std::span(slice_tx.data() + axiom::wire::HEADER_SIZE,
                                 axiom::wire::AST_SLICE_REQ_HEADER_SIZE));

        if (write(client_fd, slice_tx.data(), slice_tx.size()) !=
                static_cast<ssize_t>(slice_tx.size()) ||
            write(client_fd, path_view.data(), path_view.size()) !=
                static_cast<ssize_t>(path_view.size()) ||
            write(client_fd, sym_name.data(), sym_name.size()) !=
                static_cast<ssize_t>(sym_name.size())) {
            fail_exit(client_fd, 9);
        }

        std::array<uint8_t, axiom::wire::HEADER_SIZE + axiom::wire::AST_SLICE_RESP_HEADER_SIZE>
            rx_slice_hdr{};
        if (!read_all(client_fd, rx_slice_hdr.data(), rx_slice_hdr.size())) {
            fail_exit(client_fd, 10);
        }

        auto dec_slice_resp = axiom::wire::decode_ast_slice_response(
            std::span(rx_slice_hdr.data() + axiom::wire::HEADER_SIZE,
                      axiom::wire::AST_SLICE_RESP_HEADER_SIZE));
        if (!dec_slice_resp || dec_slice_resp->status != 0 || dec_slice_resp->start_line != 4) {
            fail_exit(client_fd, 11);
        }

        std::string slice_text(dec_slice_resp->content_len, '\0');
        if (!read_all(client_fd, slice_text.data(), dec_slice_resp->content_len)) {
            fail_exit(client_fd, 12);
        }
        if (slice_text.find("return a + b;") == std::string::npos) {
            fail_exit(client_fd, 13);
        }

        // 3. Send AstPatch request
        std::string_view replacement =
            "    int add(int a, int b) {\n        return a + b + 100;\n    }";
        axiom::wire::AstPatchRequestHeader patch_req{
            .path_len = static_cast<uint16_t>(path_view.size()),
            .symbol_len = static_cast<uint16_t>(sym_name.size()),
            .replacement_len = static_cast<uint32_t>(replacement.size()),
        };
        axiom::wire::FrameHeader f_patch_hdr{
            .magic = axiom::wire::WIRE_MAGIC,
            .opcode = static_cast<uint16_t>(axiom::wire::Opcode::AstPatch),
            .request_id = 103,
            .payload_len =
                static_cast<uint32_t>(axiom::wire::AST_PATCH_REQ_HEADER_SIZE + path_view.size() +
                                      sym_name.size() + replacement.size()),
        };
        std::array<uint8_t, axiom::wire::HEADER_SIZE + axiom::wire::AST_PATCH_REQ_HEADER_SIZE>
            patch_tx{};
        (void)axiom::wire::encode_header(f_patch_hdr,
                                         std::span(patch_tx.data(), axiom::wire::HEADER_SIZE));
        (void)axiom::wire::encode_ast_patch_request(
            patch_req, std::span(patch_tx.data() + axiom::wire::HEADER_SIZE,
                                 axiom::wire::AST_PATCH_REQ_HEADER_SIZE));

        if (write(client_fd, patch_tx.data(), patch_tx.size()) !=
                static_cast<ssize_t>(patch_tx.size()) ||
            write(client_fd, path_view.data(), path_view.size()) !=
                static_cast<ssize_t>(path_view.size())) {
            fail_exit(client_fd, 14);
        }
        if (write(client_fd, sym_name.data(), sym_name.size()) !=
                static_cast<ssize_t>(sym_name.size()) ||
            write(client_fd, replacement.data(), replacement.size()) !=
                static_cast<ssize_t>(replacement.size())) {
            fail_exit(client_fd, 14);
        }

        std::array<uint8_t, axiom::wire::HEADER_SIZE + axiom::wire::AST_PATCH_RESP_HEADER_SIZE>
            rx_patch_hdr{};
        if (!read_all(client_fd, rx_patch_hdr.data(), rx_patch_hdr.size())) {
            fail_exit(client_fd, 15);
        }

        auto dec_patch_resp = axiom::wire::decode_ast_patch_response(
            std::span(rx_patch_hdr.data() + axiom::wire::HEADER_SIZE,
                      axiom::wire::AST_PATCH_RESP_HEADER_SIZE));
        if (!dec_patch_resp || dec_patch_resp->status != 0) {
            fail_exit(client_fd, 16);
        }

        // Send Shutdown frame
        axiom::wire::FrameHeader shut_hdr{
            .magic = axiom::wire::WIRE_MAGIC,
            .opcode = static_cast<uint16_t>(axiom::wire::Opcode::Shutdown),
            .request_id = 104,
            .payload_len = 0,
        };
        std::array<uint8_t, axiom::wire::HEADER_SIZE> shut_buf{};
        (void)axiom::wire::encode_header(shut_hdr, shut_buf);
        if (write(client_fd, shut_buf.data(), shut_buf.size()) < 0) {
            // Ignore socket write error on shutdown
        }

        // Wait for server to close socket
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

    // Verify file on disk was patched
    std::ifstream ifs(target_file);
    std::string patched_content((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
    unlink(target_file);
    check(patched_content.find("return a + b + 100;") != std::string::npos,
          "Patched file content mismatch on disk");

    std::printf("[integration-test] Server AST RPC integration test passed\n");
    return EXIT_SUCCESS;
}
