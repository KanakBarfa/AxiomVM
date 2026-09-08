#pragma once

#include <cstddef>
#include <cstdint>
#include <signal.h>
#include <span>
#include <sys/signalfd.h>
#include <unistd.h>
#include <utility>

#include "axiom/common.hpp"

namespace axiom {

/// RAII wrapper around Linux signalfd for asynchronous signal dispatch.
class SignalFd {
public:
    /// Constructs an uninitialized signalfd handle.
    constexpr SignalFd() noexcept : fd_{-1} {}

    /// Takes ownership of a signalfd descriptor.
    explicit constexpr SignalFd(int fd) noexcept : fd_{fd} {}

    ~SignalFd() noexcept {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    SignalFd(const SignalFd&) = delete;
    auto operator=(const SignalFd&) -> SignalFd& = delete;

    /// Transfers ownership of the signalfd descriptor.
    constexpr SignalFd(SignalFd&& other) noexcept : fd_{other.fd_} { other.fd_ = -1; }

    /// Move-assigns a signalfd instance.
    auto operator=(SignalFd&& other) noexcept -> SignalFd& {
        if (this != &other) {
            if (fd_ >= 0) {
                close(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    /// Creates and masks signals for a non-blocking signalfd handle.
    [[nodiscard]] static auto create(std::span<const int> signals) noexcept -> Result<SignalFd> {
        sigset_t mask;
        sigemptyset(&mask);
        for (int sig : signals) {
            sigaddset(&mask, sig);
        }

        if (sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
            return std::unexpected(SystemError::SignalfdError);
        }

        int fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (fd < 0) {
            return std::unexpected(SystemError::SignalfdError);
        }

        return SignalFd{fd};
    }

    /// Reads pending signal information into the destination span without blocking.
    [[nodiscard]] auto read_signals(std::span<signalfd_siginfo> info) noexcept -> Result<size_t> {
        if (info.empty()) {
            return 0;
        }

        size_t bytes_to_read = info.size() * sizeof(signalfd_siginfo);
        ssize_t bytes_read = read(fd_, info.data(), bytes_to_read);
        if (bytes_read < 0) {
            return std::unexpected(SystemError::SignalfdError);
        }

        return static_cast<size_t>(bytes_read) / sizeof(signalfd_siginfo);
    }

    /// Releases ownership of the file descriptor without closing it.
    [[nodiscard]] constexpr auto release() noexcept -> int {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

    /// Returns the underlying file descriptor.
    [[nodiscard]] constexpr auto fd() const noexcept -> int { return fd_; }

    /// Checks if the handle contains an open descriptor.
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool { return fd_ >= 0; }

private:
    int fd_{-1};
};

} // namespace axiom
