#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <sys/epoll.h>
#include <unistd.h>
#include <utility>

#include "axiom/common.hpp"

namespace axiom {

/// RAII wrapper around Linux epoll file descriptor.
class Reactor {
public:
    /// Creates an uninitialized reactor handle.
    constexpr Reactor() noexcept : epoll_fd_{-1} {}

    /// Takes ownership of an existing epoll file descriptor.
    explicit constexpr Reactor(int epoll_fd) noexcept : epoll_fd_{epoll_fd} {}

    ~Reactor() noexcept {
        if (epoll_fd_ >= 0) {
            close(epoll_fd_);
            epoll_fd_ = -1;
        }
    }

    Reactor(const Reactor&) = delete;
    auto operator=(const Reactor&) -> Reactor& = delete;

    /// Transfers ownership of the reactor descriptor.
    constexpr Reactor(Reactor&& other) noexcept : epoll_fd_{other.epoll_fd_} {
        other.epoll_fd_ = -1;
    }

    /// Move-assigns an epoll reactor instance.
    auto operator=(Reactor&& other) noexcept -> Reactor& {
        if (this != &other) {
            if (epoll_fd_ >= 0) {
                close(epoll_fd_);
            }
            epoll_fd_ = other.epoll_fd_;
            other.epoll_fd_ = -1;
        }
        return *this;
    }

    /// Creates a new epoll reactor with close-on-exec enabled.
    [[nodiscard]] static auto create() noexcept -> Result<Reactor> {
        int fd = epoll_create1(EPOLL_CLOEXEC);
        if (fd < 0) {
            return std::unexpected(SystemError::EpollError);
        }
        return Reactor{fd};
    }

    /// Registers a file descriptor with epoll interest list.
    [[nodiscard]] auto add(int fd, uint32_t events) noexcept -> Result<void> {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
            return std::unexpected(SystemError::EpollError);
        }
        return {};
    }

    /// Modifies registered events for a file descriptor.
    [[nodiscard]] auto modify(int fd, uint32_t events) noexcept -> Result<void> {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) != 0) {
            return std::unexpected(SystemError::EpollError);
        }
        return {};
    }

    /// Removes a file descriptor from epoll monitoring.
    [[nodiscard]] auto remove(int fd) noexcept -> Result<void> {
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) != 0) {
            return std::unexpected(SystemError::EpollError);
        }
        return {};
    }

    /// Polls registered file descriptors for active events up to span capacity.
    [[nodiscard]] Result<size_t> wait(std::span<epoll_event> events, int timeout_ms) noexcept {
        if (events.empty()) {
            return 0;
        }
        int nfds =
            epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()), timeout_ms);
        if (nfds < 0) {
            return std::unexpected(SystemError::EpollError);
        }
        return static_cast<size_t>(nfds);
    }

    /// Returns the underlying file descriptor.
    [[nodiscard]] constexpr auto fd() const noexcept -> int { return epoll_fd_; }

    /// Checks if the reactor contains a valid open descriptor.
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool { return epoll_fd_ >= 0; }

private:
    int epoll_fd_{-1};
};

} // namespace axiom
