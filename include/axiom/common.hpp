#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

namespace axiom {

/// Represents system-level error codes across the appliance runtime.
enum class SystemError : uint8_t {
    Success = 0,
    InvalidArgument,
    BufferUnderflow,
    BufferOverflow,
    OutOfMemory,
    VsockError,
    EpollError,
    SignalfdError,
    ProcessSpawnError,
    ProcessWaitError,
    IoError,
    ProtocolError,
    MountError,
    NotFound,
    AstError,
};

/// Returns a human-readable description for a SystemError code.
[[nodiscard]] constexpr auto to_string_view(SystemError err) noexcept -> std::string_view {
    switch (err) {
    case SystemError::Success:
        return "Success";
    case SystemError::InvalidArgument:
        return "Invalid argument";
    case SystemError::BufferUnderflow:
        return "Buffer underflow";
    case SystemError::BufferOverflow:
        return "Buffer overflow";
    case SystemError::OutOfMemory:
        return "Out of memory";
    case SystemError::VsockError:
        return "Virtio vsock error";
    case SystemError::EpollError:
        return "Epoll error";
    case SystemError::SignalfdError:
        return "Signalfd error";
    case SystemError::ProcessSpawnError:
        return "Process spawn error";
    case SystemError::ProcessWaitError:
        return "Process wait error";
    case SystemError::IoError:
        return "IO error";
    case SystemError::ProtocolError:
        return "Protocol error";
    case SystemError::MountError:
        return "Mount error";
    case SystemError::NotFound:
        return "Not found";
    case SystemError::AstError:
        return "AST error";
    }
    return "Unknown error";
}

template <typename T> using Result = std::expected<T, SystemError>;

} // namespace axiom
