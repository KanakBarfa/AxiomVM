#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace axiom::ansi {

/// State enum for ANSI escape sequence finite state machine.
enum class State : uint8_t {
    Ground = 0,
    Escape,
    CsiParam,
    CsiInterm,
    Osc,
    OscEsc,
    EscInterm,
};

/// Single-pass zero-allocation finite state machine for stripping ANSI sequences.
class AnsiFilter {
public:
    constexpr AnsiFilter() noexcept = default;

    /// Filters ANSI escape sequences from input span into output span.
    constexpr auto filter(std::span<const uint8_t> input, std::span<uint8_t> output) noexcept
        -> size_t {
        size_t out_idx = 0;
        for (uint8_t b : input) {
            process_byte(b, output, out_idx);
        }
        return out_idx;
    }

    /// Filters ANSI escape sequences from string_view into char output span.
    constexpr auto filter(std::string_view input, std::span<char> output) noexcept -> size_t {
        std::span<const uint8_t> in_span(reinterpret_cast<const uint8_t*>(input.data()),
                                         input.size());
        std::span<uint8_t> out_span(reinterpret_cast<uint8_t*>(output.data()), output.size());
        return filter(in_span, out_span);
    }

    /// Resets the filter back to ground state.
    constexpr void reset() noexcept { state_ = State::Ground; }

    /// Returns current internal state of parser.
    [[nodiscard]] constexpr auto state() const noexcept -> State { return state_; }

private:
    State state_{State::Ground};

    constexpr void process_byte(uint8_t b, std::span<uint8_t> out, size_t& out_idx) noexcept {
        switch (state_) {
        case State::Ground:
            if (b == 0x1B) {
                state_ = State::Escape;
                return;
            }
            if (b == '\r') {
                return;
            }
            if (b == '\n' || b == '\t' || b >= 0x20) {
                if (out_idx < out.size()) {
                    out[out_idx++] = b;
                }
            }
            return;

        case State::Escape:
            if (b == '[') {
                state_ = State::CsiParam;
            } else if (b == ']') {
                state_ = State::Osc;
            } else if (b >= 0x20 && b <= 0x2F) {
                state_ = State::EscInterm;
            } else {
                state_ = State::Ground;
            }
            return;

        case State::CsiParam:
            if (b >= 0x30 && b <= 0x3F) {
                return;
            }
            if (b >= 0x20 && b <= 0x2F) {
                state_ = State::CsiInterm;
                return;
            }
            if (b >= 0x40 && b <= 0x7E) {
                state_ = State::Ground;
                return;
            }
            if (b == 0x1B) {
                state_ = State::Escape;
                return;
            }
            state_ = State::Ground;
            return;

        case State::CsiInterm:
            if (b >= 0x20 && b <= 0x2F) {
                return;
            }
            if (b >= 0x40 && b <= 0x7E) {
                state_ = State::Ground;
                return;
            }
            if (b == 0x1B) {
                state_ = State::Escape;
                return;
            }
            state_ = State::Ground;
            return;

        case State::Osc:
            if (b == 0x07) {
                state_ = State::Ground;
            } else if (b == 0x1B) {
                state_ = State::OscEsc;
            }
            return;

        case State::OscEsc:
            if (b == '\\') {
                state_ = State::Ground;
            } else if (b == '[') {
                state_ = State::CsiParam;
            } else if (b != 0x1B) {
                state_ = State::Escape;
            }
            return;

        case State::EscInterm:
            state_ = State::Ground;
            return;
        }
    }
};

/// Strips ANSI sequences in a single pass into destination buffer.
[[nodiscard]] inline auto strip_ansi(std::string_view input, std::span<char> dest) noexcept
    -> size_t {
    AnsiFilter filter;
    return filter.filter(input, dest);
}

} // namespace axiom::ansi
