#pragma once

#include <cstdint>
#include <fcntl.h>
#include <linux/vm_sockets.h>
#include <sys/socket.h>
#include <unistd.h>

#include "axiom/common.hpp"

namespace axiom::vsock {

inline constexpr uint32_t DEFAULT_GUEST_PORT = 5200;

/// Sets a file descriptor to non-blocking and close-on-exec mode.
[[nodiscard]] inline auto set_nonblocking_cloexec(int fd) noexcept -> Result<void> {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return std::unexpected(SystemError::IoError);
    }
    int fd_flags = fcntl(fd, F_GETFD, 0);
    if (fd_flags < 0 || fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0) {
        return std::unexpected(SystemError::IoError);
    }
    return {};
}

inline constexpr int DEFAULT_BACKLOG = 16;

/// Creates, binds, and listens on an AF_VSOCK streaming socket.
[[nodiscard]] inline auto create_listener(uint32_t port = DEFAULT_GUEST_PORT,
                                          uint32_t cid = VMADDR_CID_ANY,
                                          int backlog = DEFAULT_BACKLOG) noexcept -> Result<int> {
    int listen_fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        return std::unexpected(SystemError::VsockError);
    }

    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        close(listen_fd);
        return std::unexpected(SystemError::VsockError);
    }

    sockaddr_vm addr{};
    addr.svm_family = AF_VSOCK;
    addr.svm_port = port;
    addr.svm_cid = cid;

    if (bind(listen_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(listen_fd);
        return std::unexpected(SystemError::VsockError);
    }

    if (listen(listen_fd, backlog) != 0) {
        close(listen_fd);
        return std::unexpected(SystemError::VsockError);
    }

    return listen_fd;
}

/// Accepts a pending connection from an AF_VSOCK listener in non-blocking mode.
[[nodiscard]] inline auto accept_connection(int listen_fd) noexcept -> Result<int> {
    sockaddr_vm client_addr{};
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept4(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len,
                            SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client_fd < 0) {
        return std::unexpected(SystemError::VsockError);
    }
    return client_fd;
}

} // namespace axiom::vsock
