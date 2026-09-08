#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace axiom::exec {

/// Fixed-capacity circular ring buffer that preserves trailing output upon overflow.
template <size_t Capacity> class RingBuffer {
public:
    static_assert(Capacity > 0, "Capacity must be greater than 0");

    constexpr RingBuffer() noexcept = default;

    /// Writes a data slice into the ring buffer, overwriting oldest bytes when full.
    auto write(std::span<const uint8_t> data) noexcept -> size_t {
        for (uint8_t byte : data) {
            push_byte(byte);
        }
        return data.size();
    }

    /// Linearizes the stored contents in chronological order into destination span.
    auto linearize(std::span<uint8_t> dest) const noexcept -> size_t {
        size_t to_copy = std::min(dest.size(), size_);
        for (size_t i = 0; i < to_copy; ++i) {
            dest[i] = storage_[(head_ + i) % Capacity];
        }
        return to_copy;
    }

    /// Linearizes the stored contents into a char span.
    auto linearize(std::span<char> dest) const noexcept -> size_t {
        return linearize(std::span<uint8_t>(reinterpret_cast<uint8_t*>(dest.data()), dest.size()));
    }

    /// Returns current number of valid bytes stored in the ring buffer.
    [[nodiscard]] constexpr auto size() const noexcept -> size_t { return size_; }

    /// Returns maximum capacity of the ring buffer.
    [[nodiscard]] constexpr auto capacity() const noexcept -> size_t { return Capacity; }

    /// Returns total number of bytes written since last clear.
    [[nodiscard]] constexpr auto total_written() const noexcept -> size_t { return total_written_; }

    /// Returns whether any bytes have been dropped due to capacity overflow.
    [[nodiscard]] constexpr auto is_truncated() const noexcept -> bool {
        return total_written_ > Capacity;
    }

    /// Returns number of bytes dropped due to capacity overflow.
    [[nodiscard]] constexpr auto dropped_bytes() const noexcept -> size_t {
        return is_truncated() ? (total_written_ - Capacity) : 0;
    }

    /// Resets the ring buffer state to empty.
    constexpr void clear() noexcept {
        head_ = 0;
        tail_ = 0;
        size_ = 0;
        total_written_ = 0;
    }

private:
    std::array<uint8_t, Capacity> storage_{};
    size_t head_{0};
    size_t tail_{0};
    size_t size_{0};
    size_t total_written_{0};

    constexpr void push_byte(uint8_t b) noexcept {
        storage_[tail_] = b;
        tail_ = (tail_ + 1) % Capacity;
        if (size_ < Capacity) {
            ++size_;
        } else {
            head_ = (head_ + 1) % Capacity;
        }
        ++total_written_;
    }
};

} // namespace axiom::exec
