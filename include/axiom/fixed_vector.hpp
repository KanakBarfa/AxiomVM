#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <type_traits>

namespace axiom {

/// Fixed capacity contiguous container backed by std::array without heap allocations.
template <typename T, size_t Capacity> class FixedVector {
public:
    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using reference = value_type&;
    using const_reference = const value_type&;
    using pointer = value_type*;
    using const_pointer = const value_type*;
    using iterator = pointer;
    using const_iterator = const_pointer;

    constexpr FixedVector() noexcept = default;

    /// Appends an element to the end if capacity permits.
    constexpr auto push_back(const T& value) noexcept -> bool {
        if (size_ >= Capacity) {
            return false;
        }
        data_[size_++] = value;
        return true;
    }

    /// Emplaces an element to the end if capacity permits.
    template <typename... Args> constexpr auto emplace_back(Args&&... args) noexcept -> bool {
        if (size_ >= Capacity) {
            return false;
        }
        data_[size_++] = T(static_cast<Args&&>(args)...);
        return true;
    }

    /// Removes the last element if not empty.
    constexpr auto pop_back() noexcept -> bool {
        if (size_ == 0) {
            return false;
        }
        --size_;
        return true;
    }

    /// Clears the container by resetting size to zero.
    constexpr void clear() noexcept { size_ = 0; }

    /// Returns the number of elements currently stored.
    [[nodiscard]] constexpr auto size() const noexcept -> size_type { return size_; }

    /// Returns the maximum capacity of the container.
    [[nodiscard]] constexpr auto capacity() const noexcept -> size_type { return Capacity; }

    /// Checks if the container is empty.
    [[nodiscard]] constexpr auto empty() const noexcept -> bool { return size_ == 0; }

    /// Checks if the container has reached maximum capacity.
    [[nodiscard]] constexpr auto full() const noexcept -> bool { return size_ >= Capacity; }

    /// Returns a direct pointer to the underlying array.
    [[nodiscard]] constexpr auto data() noexcept -> pointer { return data_.data(); }

    /// Returns a const pointer to the underlying array.
    [[nodiscard]] constexpr auto data() const noexcept -> const_pointer { return data_.data(); }

    /// Returns a contiguous span view of the active elements.
    [[nodiscard]] constexpr auto as_span() noexcept -> std::span<T> {
        return {data_.data(), size_};
    }

    /// Returns a const contiguous span view of the active elements.
    [[nodiscard]] constexpr auto as_span() const noexcept -> std::span<const T> {
        return {data_.data(), size_};
    }

    /// Element access with bounds check clamping.
    [[nodiscard]] constexpr auto operator[](size_type index) noexcept -> reference {
        return data_[index];
    }

    /// Const element access.
    [[nodiscard]] constexpr auto operator[](size_type index) const noexcept -> const_reference {
        return data_[index];
    }

    [[nodiscard]] constexpr auto begin() noexcept -> iterator { return data_.data(); }

    [[nodiscard]] constexpr auto end() noexcept -> iterator { return data_.data() + size_; }

    [[nodiscard]] constexpr auto begin() const noexcept -> const_iterator { return data_.data(); }

    [[nodiscard]] constexpr auto end() const noexcept -> const_iterator {
        return data_.data() + size_;
    }

private:
    std::array<T, Capacity> data_{};
    size_type size_{0};
};

} // namespace axiom
