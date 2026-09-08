#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <span>
#include <sys/epoll.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#include "axiom/ast.hpp"
#include "axiom/cdp.hpp"
#include "axiom/common.hpp"
#include "axiom/fixed_vector.hpp"
#include "axiom/process.hpp"
#include "axiom/reactor.hpp"
#include "axiom/signalfd.hpp"
#include "axiom/wire.hpp"

namespace axiom::server {

inline constexpr size_t MAX_CLIENTS = 4;
inline constexpr size_t BUFFER_CAPACITY = 65536;

/// Tracks per-connection reception state without heap allocations.
struct ClientSlot {
    int fd{-1};
    FixedVector<uint8_t, BUFFER_CAPACITY> rx_buffer{};

    /// Resets the client slot to inactive state.
    void reset() noexcept {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
        rx_buffer.clear();
    }
};

/// Orchestrates zero-heap steady-state event handling and frame dispatch.
class ApplianceServer {
public:
    ApplianceServer() noexcept = default;

    /// Initializes reactor with listener and signal file descriptors.
    auto init(int listener_fd, SignalFd signal_fd) noexcept -> Result<void> {
        auto reactor_res = Reactor::create();
        if (!reactor_res) {
            return std::unexpected(reactor_res.error());
        }
        reactor_ = std::move(*reactor_res);

        listener_fd_ = listener_fd;
        signal_source_ = std::move(signal_fd);

        auto res = reactor_.add(listener_fd_, EPOLLIN);
        if (!res) {
            return res;
        }

        res = reactor_.add(signal_source_.fd(), EPOLLIN);
        if (!res) {
            return res;
        }

        running_ = true;
        return {};
    }

    /// Runs the steady-state epoll event loop until shutdown is signaled.
    auto run() noexcept -> Result<void> {
        std::array<epoll_event, 16> events{};

        while (running_) {
            auto wait_res = reactor_.wait(events, 1000);
            if (!wait_res) {
                return std::unexpected(wait_res.error());
            }

            size_t num_events = *wait_res;
            for (size_t i = 0; i < num_events; ++i) {
                int event_fd = events[i].data.fd;
                uint32_t event_flags = events[i].events;

                if (event_fd == signal_source_.fd()) {
                    handle_signals();
                } else if (event_fd == listener_fd_) {
                    handle_accept();
                } else {
                    handle_client_event(event_fd, event_flags);
                }
            }
        }

        cleanup();
        return {};
    }

    /// Signals the server to stop processing new events and initiate clean shutdown.
    void stop() noexcept { running_ = false; }

    /// Returns whether the server loop is currently active.
    [[nodiscard]] auto is_running() const noexcept -> bool { return running_; }

    /// Returns mutable reference to the internal CDP bridge.
    [[nodiscard]] auto cdp_bridge() noexcept -> cdp::CdpBridge& { return cdp_bridge_; }

private:
    Reactor reactor_{};
    int listener_fd_{-1};
    SignalFd signal_source_{};
    bool running_{false};
    std::array<ClientSlot, MAX_CLIENTS> clients_{};
    exec::ProcessRunner process_runner_{};
    std::array<char, exec::RING_BUFFER_CAPACITY> exec_output_buf_{};
    std::array<char, 8192> exec_diff_buf_{};
    ast::AstEngine ast_engine_{};
    static constexpr size_t AST_FILE_BUFFER_CAPACITY = 256 * 1024;
    std::array<char, AST_FILE_BUFFER_CAPACITY> ast_file_buf_{};
    std::array<char, AST_FILE_BUFFER_CAPACITY> ast_work_buf_{};
    cdp::CdpBridge cdp_bridge_{};
    std::array<char, cdp::MAX_TREE_TEXT_SIZE> cdp_tree_buf_{};
    FixedVector<cdp::AXNode, cdp::MAX_AX_NODES> cdp_nodes_{};

    /// Processes pending signals delivered via signalfd.
    void handle_signals() noexcept {
        std::array<signalfd_siginfo, 16> sig_entries{};
        while (true) {
            auto read_res = signal_source_.read_signals(sig_entries);
            if (!read_res || *read_res == 0) {
                break;
            }

            size_t count = *read_res;
            for (size_t i = 0; i < count; ++i) {
                uint32_t sig = sig_entries[i].ssi_signo;
                if (sig == SIGCHLD) {
                    reap_children();
                } else if (sig == SIGTERM || sig == SIGINT) {
                    running_ = false;
                }
            }
        }
    }

    /// Reaps terminated child processes without blocking to prevent PID exhaustion.
    static void reap_children() noexcept {
        siginfo_t info{};
        while (true) {
            info.si_pid = 0;
            if (waitid(P_ALL, 0, &info, WNOHANG | WEXITED) != 0 || info.si_pid == 0) {
                break;
            }
        }
        int status = 0;
        while (waitpid(-1, &status, WNOHANG) > 0) {
        }
    }

    /// Accepts an incoming client connection.
    void handle_accept() noexcept {
        sockaddr client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client_fd =
            accept4(listener_fd_, &client_addr, &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            return;
        }

        ClientSlot* slot = find_free_slot();
        if (slot == nullptr) {
            close(client_fd);
            return;
        }

        slot->fd = client_fd;
        slot->rx_buffer.clear();

        if (!reactor_.add(client_fd, EPOLLIN | EPOLLRDHUP)) {
            slot->reset();
        }
    }

    /// Finds an available client slot or returns nullptr if at capacity.
    auto find_free_slot() noexcept -> ClientSlot* {
        for (auto& slot : clients_) {
            if (slot.fd < 0) {
                return &slot;
            }
        }
        return nullptr;
    }

    /// Finds the client slot associated with the specified file descriptor.
    auto find_slot_by_fd(int fd) noexcept -> ClientSlot* {
        for (auto& slot : clients_) {
            if (slot.fd == fd) {
                return &slot;
            }
        }
        return nullptr;
    }

    /// Handles events from an active client connection.
    void handle_client_event(int client_fd, uint32_t events) noexcept {
        ClientSlot* slot = find_slot_by_fd(client_fd);
        if (slot == nullptr) {
            (void)reactor_.remove(client_fd);
            close(client_fd);
            return;
        }

        if ((events & EPOLLIN) != 0) {
            std::array<uint8_t, 4096> chunk{};
            ssize_t bytes_read = read(client_fd, chunk.data(), chunk.size());
            if (bytes_read > 0) {
                for (ssize_t i = 0; i < bytes_read; ++i) {
                    if (!slot->rx_buffer.push_back(chunk[static_cast<size_t>(i)])) {
                        (void)reactor_.remove(client_fd);
                        slot->reset();
                        return;
                    }
                }

                process_client_frames(slot);
            } else {
                (void)reactor_.remove(client_fd);
                slot->reset();
                return;
            }
        }

        if (slot->fd >= 0 && (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0) {
            (void)reactor_.remove(client_fd);
            slot->reset();
            return;
        }
    }

    /// Parses and dispatches complete TDS-Wire frames from the client RX buffer.
    void process_client_frames(ClientSlot* slot) noexcept {
        while (slot->rx_buffer.size() >= wire::HEADER_SIZE) {
            auto header_res = wire::decode_header(slot->rx_buffer.as_span());
            if (!header_res) {
                (void)reactor_.remove(slot->fd);
                slot->reset();
                return;
            }

            const auto& header = *header_res;
            if (header.payload_len > BUFFER_CAPACITY - wire::HEADER_SIZE) {
                (void)reactor_.remove(slot->fd);
                slot->reset();
                return;
            }

            size_t total_frame_len = wire::HEADER_SIZE + header.payload_len;
            if (slot->rx_buffer.size() < total_frame_len) {
                return;
            }

            dispatch_frame(slot, header);

            size_t remaining = slot->rx_buffer.size() - total_frame_len;
            if (remaining > 0 && remaining <= BUFFER_CAPACITY) {
                std::memmove(slot->rx_buffer.data(), slot->rx_buffer.data() + total_frame_len,
                             remaining);
            }
            while (slot->rx_buffer.size() > remaining) {
                (void)slot->rx_buffer.pop_back();
            }
        }
    }

    /// Writes an entire buffer to a file descriptor without partial writes.
    static auto write_all(int fd, const void* data, size_t len) noexcept -> bool {
        const auto* ptr = static_cast<const uint8_t*>(data);
        size_t written = 0;
        while (written < len) {
            ssize_t n = write(fd, ptr + written, len - written);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    struct pollfd pfd{
                        .fd = fd,
                        .events = POLLOUT,
                        .revents = 0,
                    };
                    int ret = poll(&pfd, 1, 5000);
                    if (ret > 0 && (pfd.revents & POLLOUT) != 0) {
                        continue;
                    }
                }
                return false;
            }
            if (n == 0) {
                return false;
            }
            written += static_cast<size_t>(n);
        }
        return true;
    }

    /// Dispatches a decoded frame and sends any immediate response.
    void dispatch_frame(ClientSlot* slot, const wire::FrameHeader& header) noexcept {
        switch (static_cast<wire::Opcode>(header.opcode)) {
        case wire::Opcode::Ping: {
            wire::FrameHeader pong_header{
                .magic = wire::WIRE_MAGIC,
                .opcode = static_cast<uint16_t>(wire::Opcode::Pong),
                .request_id = header.request_id,
                .payload_len = 0,
            };
            std::array<uint8_t, wire::HEADER_SIZE> tx_buf{};
            auto encode_res = wire::encode_header(pong_header, tx_buf);
            if (encode_res) {
                if (!write_all(slot->fd, tx_buf.data(), tx_buf.size())) {
                    (void)reactor_.remove(slot->fd);
                    slot->reset();
                }
            }
            break;
        }
        case wire::Opcode::Exec: {
            handle_exec(slot, header);
            break;
        }
        case wire::Opcode::AstSymbols: {
            handle_ast_symbols(slot, header);
            break;
        }
        case wire::Opcode::AstSlice: {
            handle_ast_slice(slot, header);
            break;
        }
        case wire::Opcode::AstPatch: {
            handle_ast_patch(slot, header);
            break;
        }
        case wire::Opcode::CdpAction: {
            handle_cdp(slot, header);
            break;
        }
        case wire::Opcode::ReadFile: {
            handle_read_file(slot, header);
            break;
        }
        case wire::Opcode::WriteFile: {
            handle_write_file(slot, header);
            break;
        }
        case wire::Opcode::Shutdown: {
            running_ = false;
            break;
        }
        default:
            break;
        }
    }

    /// Handles an Exec command frame and transmits the filtered execution result.
    void handle_exec(ClientSlot* slot, const wire::FrameHeader& header) noexcept {
        if (header.payload_len < wire::EXEC_REQ_HEADER_SIZE) {
            return;
        }

        auto payload = slot->rx_buffer.as_span().subspan(wire::HEADER_SIZE, header.payload_len);
        auto req_res = wire::decode_exec_request(payload);
        if (!req_res) {
            return;
        }

        const auto& req = *req_res;
        size_t needed = wire::EXEC_REQ_HEADER_SIZE + req.cmd_len + req.cwd_len;
        if (header.payload_len < needed) {
            return;
        }

        const char* cmd_data =
            reinterpret_cast<const char*>(payload.data() + wire::EXEC_REQ_HEADER_SIZE);
        std::string_view cmd(cmd_data, req.cmd_len);

        const char* cwd_data = cmd_data + req.cmd_len;
        std::string_view cwd(cwd_data, req.cwd_len);

        exec::ExecOptions opts{
            .command = cmd,
            .working_dir = cwd.empty() ? std::string_view("/tmp") : cwd,
            .timeout_ms = req.timeout_ms > 0 ? req.timeout_ms : 5000,
            .flags = req.flags,
        };

        auto run_res = process_runner_.run(opts, exec_output_buf_, exec_diff_buf_);

        wire::ExecResponseHeader resp_hdr{};
        if (run_res) {
            resp_hdr.exit_code = run_res->exit_code;
            resp_hdr.flags = run_res->flags;
            resp_hdr.cpu_cycles = run_res->cpu_cycles;
            resp_hdr.output_len = static_cast<uint32_t>(run_res->output_len);
            resp_hdr.diff_len = static_cast<uint32_t>(run_res->diff_len);
        } else {
            resp_hdr.exit_code = -1;
            resp_hdr.flags = 0;
            resp_hdr.cpu_cycles = 0;
            resp_hdr.output_len = 0;
            resp_hdr.diff_len = 0;
        }

        uint32_t total_payload = static_cast<uint32_t>(wire::EXEC_RESP_HEADER_SIZE) +
                                 resp_hdr.output_len + resp_hdr.diff_len;

        wire::FrameHeader out_hdr{
            .magic = wire::WIRE_MAGIC,
            .opcode = static_cast<uint16_t>(wire::Opcode::ExecOutput),
            .request_id = header.request_id,
            .payload_len = total_payload,
        };

        std::array<uint8_t, wire::HEADER_SIZE + wire::EXEC_RESP_HEADER_SIZE> hdr_buf{};
        auto enc1 = wire::encode_header(out_hdr, std::span(hdr_buf.data(), wire::HEADER_SIZE));
        auto enc2 = wire::encode_exec_response(
            resp_hdr, std::span(hdr_buf.data() + wire::HEADER_SIZE, wire::EXEC_RESP_HEADER_SIZE));

        if (enc1 && enc2) {
            if (!write_all(slot->fd, hdr_buf.data(), hdr_buf.size())) {
                (void)reactor_.remove(slot->fd);
                slot->reset();
                return;
            }

            if (resp_hdr.output_len > 0) {
                if (!write_all(slot->fd, exec_output_buf_.data(), resp_hdr.output_len)) {
                    (void)reactor_.remove(slot->fd);
                    slot->reset();
                    return;
                }
            }

            if (resp_hdr.diff_len > 0) {
                if (!write_all(slot->fd, exec_diff_buf_.data(), resp_hdr.diff_len)) {
                    (void)reactor_.remove(slot->fd);
                    slot->reset();
                    return;
                }
            }
        }
    }

    /// Reads file contents up to buffer capacity without dynamic allocation.
    static auto read_file(std::string_view path, std::span<char> buf) noexcept -> Result<size_t> {
        if (path.empty() || path.size() >= 4096) {
            return std::unexpected(SystemError::InvalidArgument);
        }
        std::array<char, 4096> path_buf{};
        std::memcpy(path_buf.data(), path.data(), path.size());
        path_buf[path.size()] = '\0';

        int fd = open(path_buf.data(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return std::unexpected(SystemError::IoError);
        }

        size_t total_read = 0;
        while (total_read < buf.size()) {
            ssize_t n = read(fd, buf.data() + total_read, buf.size() - total_read);
            if (n == 0) {
                break;
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(fd);
                return std::unexpected(SystemError::IoError);
            }
            total_read += static_cast<size_t>(n);
        }

        close(fd);
        return total_read;
    }

    /// Writes buffer contents to a file atomically without dynamic allocation.
    static auto write_file(std::string_view path, std::string_view content) noexcept
        -> Result<void> {
        if (path.empty() || path.size() >= 4096) {
            return std::unexpected(SystemError::InvalidArgument);
        }
        std::array<char, 4096> path_buf{};
        std::memcpy(path_buf.data(), path.data(), path.size());
        path_buf[path.size()] = '\0';

        int fd = open(path_buf.data(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            return std::unexpected(SystemError::IoError);
        }

        if (!write_all(fd, content.data(), content.size())) {
            close(fd);
            return std::unexpected(SystemError::IoError);
        }

        close(fd);
        return {};
    }

    /// Handles an AstSymbols request and sends the formatted symbol table.
    void handle_ast_symbols(ClientSlot* slot, const wire::FrameHeader& header) noexcept {
        if (header.payload_len < wire::AST_SYMBOLS_REQ_HEADER_SIZE) {
            return;
        }

        auto payload = slot->rx_buffer.as_span().subspan(wire::HEADER_SIZE, header.payload_len);
        auto req_res = wire::decode_ast_symbols_request(payload);
        if (!req_res) {
            return;
        }

        const auto& req = *req_res;
        if (header.payload_len < wire::AST_SYMBOLS_REQ_HEADER_SIZE + req.path_len) {
            return;
        }

        const char* path_data =
            reinterpret_cast<const char*>(payload.data() + wire::AST_SYMBOLS_REQ_HEADER_SIZE);
        std::string_view path(path_data, req.path_len);

        wire::AstSymbolsResponseHeader resp_hdr{};
        size_t fmt_len = 0;

        auto read_res = read_file(path, ast_file_buf_);
        if (!read_res) {
            resp_hdr.status = -1;
        } else {
            std::string_view source(ast_file_buf_.data(), *read_res);
            ast::Lang lang = ast::detect_language(path);
            if (lang == ast::Lang::Unknown) {
                resp_hdr.status = -2;
            } else {
                FixedVector<ast::SymbolInfo, ast::MAX_SYMBOLS> symbols{};
                auto ext_res = ast_engine_.extract_symbols(source, lang, symbols);
                if (!ext_res) {
                    resp_hdr.status = -3;
                } else {
                    auto fmt_res = ast::AstEngine::format_symbols(symbols, ast_work_buf_);
                    if (!fmt_res) {
                        resp_hdr.status = -4;
                    } else {
                        fmt_len = *fmt_res;
                        resp_hdr.status = 0;
                        resp_hdr.symbol_count = static_cast<uint32_t>(symbols.size());
                        resp_hdr.payload_len = static_cast<uint32_t>(fmt_len);
                    }
                }
            }
        }

        uint32_t total_payload =
            static_cast<uint32_t>(wire::AST_SYMBOLS_RESP_HEADER_SIZE + fmt_len);
        wire::FrameHeader out_hdr{
            .magic = wire::WIRE_MAGIC,
            .opcode = static_cast<uint16_t>(wire::Opcode::AstSymbolsResponse),
            .request_id = header.request_id,
            .payload_len = total_payload,
        };

        std::array<uint8_t, wire::HEADER_SIZE + wire::AST_SYMBOLS_RESP_HEADER_SIZE> hdr_buf{};
        auto enc1 = wire::encode_header(out_hdr, std::span(hdr_buf.data(), wire::HEADER_SIZE));
        auto enc2 = wire::encode_ast_symbols_response(
            resp_hdr,
            std::span(hdr_buf.data() + wire::HEADER_SIZE, wire::AST_SYMBOLS_RESP_HEADER_SIZE));

        if (enc1 && enc2) {
            if (!write_all(slot->fd, hdr_buf.data(), hdr_buf.size())) {
                (void)reactor_.remove(slot->fd);
                slot->reset();
                return;
            }
            if (fmt_len > 0) {
                if (!write_all(slot->fd, ast_work_buf_.data(), fmt_len)) {
                    (void)reactor_.remove(slot->fd);
                    slot->reset();
                    return;
                }
            }
        }
    }

    /// Handles an AstSlice request and returns the extracted code slice.
    void handle_ast_slice(ClientSlot* slot, const wire::FrameHeader& header) noexcept {
        if (header.payload_len < wire::AST_SLICE_REQ_HEADER_SIZE) {
            return;
        }

        auto payload = slot->rx_buffer.as_span().subspan(wire::HEADER_SIZE, header.payload_len);
        auto req_res = wire::decode_ast_slice_request(payload);
        if (!req_res) {
            return;
        }

        const auto& req = *req_res;
        size_t needed = wire::AST_SLICE_REQ_HEADER_SIZE + req.path_len + req.symbol_len;
        if (header.payload_len < needed) {
            return;
        }

        const char* path_data =
            reinterpret_cast<const char*>(payload.data() + wire::AST_SLICE_REQ_HEADER_SIZE);
        std::string_view path(path_data, req.path_len);

        const char* sym_data = path_data + req.path_len;
        std::string_view sym_name(sym_data, req.symbol_len);

        wire::AstSliceResponseHeader resp_hdr{};
        ast::SymbolSlice slice{};

        auto read_res = read_file(path, ast_file_buf_);
        if (!read_res) {
            resp_hdr.status = -1;
        } else {
            std::string_view source(ast_file_buf_.data(), *read_res);
            ast::Lang lang = ast::detect_language(path);
            if (lang == ast::Lang::Unknown) {
                resp_hdr.status = -2;
            } else {
                auto slice_res = ast_engine_.slice_symbol(source, lang, sym_name, slice);
                if (!slice_res) {
                    resp_hdr.status = -3;
                } else {
                    resp_hdr.status = 0;
                    resp_hdr.start_line = slice.start_line;
                    resp_hdr.end_line = slice.end_line;
                    resp_hdr.content_len = static_cast<uint32_t>(slice.content.size());
                }
            }
        }

        uint32_t total_payload =
            static_cast<uint32_t>(wire::AST_SLICE_RESP_HEADER_SIZE + resp_hdr.content_len);
        wire::FrameHeader out_hdr{
            .magic = wire::WIRE_MAGIC,
            .opcode = static_cast<uint16_t>(wire::Opcode::AstSliceResponse),
            .request_id = header.request_id,
            .payload_len = total_payload,
        };

        std::array<uint8_t, wire::HEADER_SIZE + wire::AST_SLICE_RESP_HEADER_SIZE> hdr_buf{};
        auto enc1 = wire::encode_header(out_hdr, std::span(hdr_buf.data(), wire::HEADER_SIZE));
        auto enc2 =
            wire::encode_ast_slice_response(resp_hdr, std::span(hdr_buf.data() + wire::HEADER_SIZE,
                                                                wire::AST_SLICE_RESP_HEADER_SIZE));

        if (enc1 && enc2) {
            if (!write_all(slot->fd, hdr_buf.data(), hdr_buf.size())) {
                (void)reactor_.remove(slot->fd);
                slot->reset();
                return;
            }
            if (resp_hdr.content_len > 0) {
                if (!write_all(slot->fd, slice.content.data(), resp_hdr.content_len)) {
                    (void)reactor_.remove(slot->fd);
                    slot->reset();
                    return;
                }
            }
        }
    }

    /// Handles an AstPatch request and updates the target symbol on disk.
    void handle_ast_patch(ClientSlot* slot, const wire::FrameHeader& header) noexcept {
        if (header.payload_len < wire::AST_PATCH_REQ_HEADER_SIZE) {
            return;
        }

        auto payload = slot->rx_buffer.as_span().subspan(wire::HEADER_SIZE, header.payload_len);
        auto req_res = wire::decode_ast_patch_request(payload);
        if (!req_res) {
            return;
        }

        const auto& req = *req_res;
        size_t needed =
            wire::AST_PATCH_REQ_HEADER_SIZE + req.path_len + req.symbol_len + req.replacement_len;
        if (header.payload_len < needed) {
            return;
        }

        const char* path_data =
            reinterpret_cast<const char*>(payload.data() + wire::AST_PATCH_REQ_HEADER_SIZE);
        std::string_view path(path_data, req.path_len);

        const char* sym_data = path_data + req.path_len;
        std::string_view sym_name(sym_data, req.symbol_len);

        const char* rep_data = sym_data + req.symbol_len;
        std::string_view replacement(rep_data, req.replacement_len);

        wire::AstPatchResponseHeader resp_hdr{};

        auto read_res = read_file(path, ast_file_buf_);
        if (!read_res) {
            resp_hdr.status = -1;
        } else {
            std::string_view source(ast_file_buf_.data(), *read_res);
            ast::Lang lang = ast::detect_language(path);
            if (lang == ast::Lang::Unknown) {
                resp_hdr.status = -2;
            } else {
                ast::PatchResult p_res{};
                auto patch_res = ast_engine_.patch_symbol(source, lang, sym_name, replacement,
                                                          ast_work_buf_, p_res);
                if (!patch_res) {
                    resp_hdr.status = -3;
                } else {
                    auto write_res =
                        write_file(path, std::string_view(ast_work_buf_.data(), *patch_res));
                    if (!write_res) {
                        resp_hdr.status = -4;
                    } else {
                        resp_hdr.status = 0;
                        resp_hdr.old_start_line = p_res.old_start_line;
                        resp_hdr.old_end_line = p_res.old_end_line;
                        resp_hdr.new_end_line = p_res.new_end_line;
                    }
                }
            }
        }

        wire::FrameHeader out_hdr{
            .magic = wire::WIRE_MAGIC,
            .opcode = static_cast<uint16_t>(wire::Opcode::AstPatchResponse),
            .request_id = header.request_id,
            .payload_len = wire::AST_PATCH_RESP_HEADER_SIZE,
        };

        std::array<uint8_t, wire::HEADER_SIZE + wire::AST_PATCH_RESP_HEADER_SIZE> hdr_buf{};
        auto enc1 = wire::encode_header(out_hdr, std::span(hdr_buf.data(), wire::HEADER_SIZE));
        auto enc2 =
            wire::encode_ast_patch_response(resp_hdr, std::span(hdr_buf.data() + wire::HEADER_SIZE,
                                                                wire::AST_PATCH_RESP_HEADER_SIZE));

        if (enc1 && enc2) {
            if (!write_all(slot->fd, hdr_buf.data(), hdr_buf.size())) {
                (void)reactor_.remove(slot->fd);
                slot->reset();
                return;
            }
        }
    }

    /// Handles a browser CDP action request and transmits the filtered result.
    void handle_cdp(ClientSlot* slot, const wire::FrameHeader& header) noexcept {
        if (header.payload_len < wire::CDP_REQ_HEADER_SIZE) {
            return;
        }

        auto payload = slot->rx_buffer.as_span().subspan(wire::HEADER_SIZE, header.payload_len);
        auto req_res = wire::decode_cdp_request(payload);
        if (!req_res) {
            return;
        }

        const auto& req = *req_res;
        size_t needed = wire::CDP_REQ_HEADER_SIZE + req.payload1_len + req.payload2_len;
        if (header.payload_len < needed) {
            return;
        }

        const char* p1_data =
            reinterpret_cast<const char*>(payload.data() + wire::CDP_REQ_HEADER_SIZE);
        std::string_view p1(p1_data, req.payload1_len);

        wire::CdpActionResponseHeader resp_hdr{};
        size_t tree_text_len = 0;

        switch (static_cast<wire::CdpActionType>(req.action)) {
        case wire::CdpActionType::Navigate: {
            auto nav_res = cdp_bridge_.navigate(p1);
            if (!nav_res) {
                resp_hdr.status = -1;
            } else {
                auto tree_res = cdp_bridge_.get_axtree(cdp_nodes_, cdp_tree_buf_);
                if (tree_res) {
                    tree_text_len = *tree_res;
                    resp_hdr.node_count = static_cast<uint32_t>(cdp_nodes_.size());
                    resp_hdr.payload_len = static_cast<uint32_t>(tree_text_len);
                }
                resp_hdr.status = 0;
            }
            break;
        }
        case wire::CdpActionType::GetTree: {
            auto tree_res = cdp_bridge_.get_axtree(cdp_nodes_, cdp_tree_buf_);
            if (!tree_res) {
                resp_hdr.status = -1;
            } else {
                tree_text_len = *tree_res;
                resp_hdr.node_count = static_cast<uint32_t>(cdp_nodes_.size());
                resp_hdr.payload_len = static_cast<uint32_t>(tree_text_len);
                resp_hdr.status = 0;
            }
            break;
        }
        case wire::CdpActionType::Click: {
            auto click_res = cdp_bridge_.click(req.target_id);
            if (!click_res) {
                resp_hdr.status = -1;
            } else {
                auto tree_res = cdp_bridge_.get_axtree(cdp_nodes_, cdp_tree_buf_);
                if (tree_res) {
                    tree_text_len = *tree_res;
                    resp_hdr.node_count = static_cast<uint32_t>(cdp_nodes_.size());
                    resp_hdr.payload_len = static_cast<uint32_t>(tree_text_len);
                }
                resp_hdr.status = 0;
            }
            break;
        }
        case wire::CdpActionType::Type: {
            auto type_res = cdp_bridge_.type_text(req.target_id, p1);
            if (!type_res) {
                resp_hdr.status = -1;
            } else {
                resp_hdr.status = 0;
            }
            break;
        }
        case wire::CdpActionType::Scroll: {
            auto scroll_res = cdp_bridge_.scroll(req.scroll_delta);
            if (!scroll_res) {
                resp_hdr.status = -1;
            } else {
                resp_hdr.status = 0;
            }
            break;
        }
        default:
            resp_hdr.status = -2;
            break;
        }

        uint32_t total_payload = static_cast<uint32_t>(wire::CDP_RESP_HEADER_SIZE + tree_text_len);
        wire::FrameHeader out_hdr{
            .magic = wire::WIRE_MAGIC,
            .opcode = static_cast<uint16_t>(wire::Opcode::CdpActionResponse),
            .request_id = header.request_id,
            .payload_len = total_payload,
        };

        std::array<uint8_t, wire::HEADER_SIZE + wire::CDP_RESP_HEADER_SIZE> hdr_buf{};
        auto enc1 = wire::encode_header(out_hdr, std::span(hdr_buf.data(), wire::HEADER_SIZE));
        auto enc2 = wire::encode_cdp_response(
            resp_hdr, std::span(hdr_buf.data() + wire::HEADER_SIZE, wire::CDP_RESP_HEADER_SIZE));

        if (enc1 && enc2) {
            if (!write_all(slot->fd, hdr_buf.data(), hdr_buf.size())) {
                (void)reactor_.remove(slot->fd);
                slot->reset();
                return;
            }
            if (tree_text_len > 0) {
                if (!write_all(slot->fd, cdp_tree_buf_.data(), tree_text_len)) {
                    (void)reactor_.remove(slot->fd);
                    slot->reset();
                    return;
                }
            }
        }
    }

    /// Handles a ReadFile request and streams file bytes.
    void handle_read_file(ClientSlot* slot, const wire::FrameHeader& header) noexcept {
        if (header.payload_len < wire::READ_FILE_REQ_HEADER_SIZE) {
            return;
        }

        auto payload = slot->rx_buffer.as_span().subspan(wire::HEADER_SIZE, header.payload_len);
        auto req_res = wire::decode_read_file_request(payload);
        if (!req_res) {
            return;
        }

        const auto& req = *req_res;
        if (header.payload_len < wire::READ_FILE_REQ_HEADER_SIZE + req.path_len) {
            return;
        }

        const char* path_data =
            reinterpret_cast<const char*>(payload.data() + wire::READ_FILE_REQ_HEADER_SIZE);
        std::string_view path(path_data, req.path_len);

        wire::ReadFileResponseHeader resp_hdr{};
        size_t bytes_to_send = 0;

        if (path.empty() || path.size() >= 4096) {
            resp_hdr.status = -EINVAL;
        } else {
            std::array<char, 4096> path_buf{};
            std::memcpy(path_buf.data(), path.data(), path.size());
            path_buf[path.size()] = '\0';

            int fd = open(path_buf.data(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                resp_hdr.status = -errno;
            } else {
                struct stat st{};
                if (fstat(fd, &st) == 0) {
                    resp_hdr.total_size = static_cast<uint32_t>(st.st_size);
                }

                if (req.offset > 0) {
                    if (lseek(fd, static_cast<off_t>(req.offset), SEEK_SET) < 0) {
                        resp_hdr.status = -errno;
                    }
                }

                if (resp_hdr.status == 0) {
                    size_t max_read = ast_work_buf_.size();
                    if (req.max_bytes > 0 && req.max_bytes < max_read) {
                        max_read = req.max_bytes;
                    }

                    ssize_t n = read(fd, ast_work_buf_.data(), max_read);
                    if (n < 0) {
                        resp_hdr.status = -errno;
                    } else {
                        bytes_to_send = static_cast<size_t>(n);
                        resp_hdr.status = 0;
                        resp_hdr.content_len = static_cast<uint32_t>(bytes_to_send);
                    }
                }
                close(fd);
            }
        }

        uint32_t total_payload =
            static_cast<uint32_t>(wire::READ_FILE_RESP_HEADER_SIZE + bytes_to_send);
        wire::FrameHeader out_hdr{
            .magic = wire::WIRE_MAGIC,
            .opcode = static_cast<uint16_t>(wire::Opcode::ReadFileResponse),
            .request_id = header.request_id,
            .payload_len = total_payload,
        };

        std::array<uint8_t, wire::HEADER_SIZE + wire::READ_FILE_RESP_HEADER_SIZE> hdr_buf{};
        auto enc1 = wire::encode_header(out_hdr, std::span(hdr_buf.data(), wire::HEADER_SIZE));
        auto enc2 =
            wire::encode_read_file_response(resp_hdr, std::span(hdr_buf.data() + wire::HEADER_SIZE,
                                                                wire::READ_FILE_RESP_HEADER_SIZE));

        if (enc1 && enc2) {
            if (!write_all(slot->fd, hdr_buf.data(), hdr_buf.size())) {
                (void)reactor_.remove(slot->fd);
                slot->reset();
                return;
            }
            if (bytes_to_send > 0) {
                if (!write_all(slot->fd, ast_work_buf_.data(), bytes_to_send)) {
                    (void)reactor_.remove(slot->fd);
                    slot->reset();
                    return;
                }
            }
        }
    }

    /// Handles a WriteFile request and writes content to disk.
    void handle_write_file(ClientSlot* slot, const wire::FrameHeader& header) noexcept {
        if (header.payload_len < wire::WRITE_FILE_REQ_HEADER_SIZE) {
            return;
        }

        auto payload = slot->rx_buffer.as_span().subspan(wire::HEADER_SIZE, header.payload_len);
        auto req_res = wire::decode_write_file_request(payload);
        if (!req_res) {
            return;
        }

        const auto& req = *req_res;
        size_t needed = wire::WRITE_FILE_REQ_HEADER_SIZE + req.path_len + req.content_len;
        if (header.payload_len < needed) {
            return;
        }

        const char* path_data =
            reinterpret_cast<const char*>(payload.data() + wire::WRITE_FILE_REQ_HEADER_SIZE);
        std::string_view path(path_data, req.path_len);

        const uint8_t* content_data =
            payload.data() + wire::WRITE_FILE_REQ_HEADER_SIZE + req.path_len;

        wire::WriteFileResponseHeader resp_hdr{};

        if (path.empty() || path.size() >= 4096) {
            resp_hdr.status = -EINVAL;
        } else {
            std::array<char, 4096> path_buf{};
            std::memcpy(path_buf.data(), path.data(), path.size());
            path_buf[path.size()] = '\0';

            // Ensure parent directories exist
            for (size_t i = 1; i < path.size(); ++i) {
                if (path_buf[i] == '/') {
                    path_buf[i] = '\0';
                    (void)mkdir(path_buf.data(), 0755);
                    path_buf[i] = '/';
                }
            }

            int flags = O_WRONLY | O_CREAT | O_CLOEXEC;
            if ((req.flags & 1) != 0) {
                flags |= O_APPEND;
            } else {
                flags |= O_TRUNC;
            }

            mode_t mode = req.mode != 0 ? static_cast<mode_t>(req.mode) : 0644;
            int fd = open(path_buf.data(), flags, mode);
            if (fd < 0) {
                resp_hdr.status = -errno;
            } else {
                if (req.content_len > 0) {
                    if (!write_all(fd, content_data, req.content_len)) {
                        resp_hdr.status = -errno;
                    } else {
                        resp_hdr.status = 0;
                        resp_hdr.bytes_written = req.content_len;
                    }
                } else {
                    resp_hdr.status = 0;
                    resp_hdr.bytes_written = 0;
                }
                close(fd);
            }
        }

        wire::FrameHeader out_hdr{
            .magic = wire::WIRE_MAGIC,
            .opcode = static_cast<uint16_t>(wire::Opcode::WriteFileResponse),
            .request_id = header.request_id,
            .payload_len = wire::WRITE_FILE_RESP_HEADER_SIZE,
        };

        std::array<uint8_t, wire::HEADER_SIZE + wire::WRITE_FILE_RESP_HEADER_SIZE> hdr_buf{};
        auto enc1 = wire::encode_header(out_hdr, std::span(hdr_buf.data(), wire::HEADER_SIZE));
        auto enc2 = wire::encode_write_file_response(
            resp_hdr,
            std::span(hdr_buf.data() + wire::HEADER_SIZE, wire::WRITE_FILE_RESP_HEADER_SIZE));

        if (enc1 && enc2) {
            if (!write_all(slot->fd, hdr_buf.data(), hdr_buf.size())) {
                (void)reactor_.remove(slot->fd);
                slot->reset();
            }
        }
    }

    /// Closes all active client connections and shuts down subordinate processes.
    void cleanup() noexcept {
        for (auto& slot : clients_) {
            if (slot.fd >= 0) {
                (void)reactor_.remove(slot.fd);
                slot.reset();
            }
        }
        cdp_bridge_.stop();
    }
};

} // namespace axiom::server
