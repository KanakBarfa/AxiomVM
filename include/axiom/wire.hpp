#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "axiom/common.hpp"
#include "axiom/fixed_vector.hpp"

namespace axiom::wire {

inline constexpr uint16_t WIRE_MAGIC = 0xAA55;
inline constexpr size_t HEADER_SIZE = 12;

/// Opcodes supported by the TDS-Wire protocol across virtio-vsock.
// NOLINTNEXTLINE(performance-enum-size)
enum class Opcode : uint16_t {
    Ping = 0x0001,
    Pong = 0x0002,
    Exec = 0x0010,
    ExecOutput = 0x0011,
    AstSymbols = 0x0020,
    AstSlice = 0x0021,
    AstPatch = 0x0022,
    AstSymbolsResponse = 0x0023,
    AstSliceResponse = 0x0024,
    AstPatchResponse = 0x0025,
    CdpAction = 0x0030,
    CdpActionResponse = 0x0031,
    ReadFile = 0x0040,
    ReadFileResponse = 0x0041,
    WriteFile = 0x0042,
    WriteFileResponse = 0x0043,
    Shutdown = 0x00FF,
};

/// Wire protocol frame header packed to exactly 12 bytes.
struct [[gnu::packed]] FrameHeader {
    uint16_t magic{WIRE_MAGIC};
    uint16_t opcode{0};
    uint32_t request_id{0};
    uint32_t payload_len{0};
};

static_assert(sizeof(FrameHeader) == HEADER_SIZE, "FrameHeader must be 12 bytes");

/// Validates and parses a frame header from raw buffer bytes.
[[nodiscard]] constexpr auto decode_header(std::span<const uint8_t> buffer) -> Result<FrameHeader> {
    if (buffer.size() < sizeof(FrameHeader)) {
        return std::unexpected(SystemError::BufferUnderflow);
    }
    FrameHeader header{};
    std::memcpy(&header, buffer.data(), sizeof(FrameHeader));
    if (header.magic != WIRE_MAGIC) {
        return std::unexpected(SystemError::ProtocolError);
    }
    return header;
}

/// Serializes a frame header into a target buffer.
[[nodiscard]] constexpr Result<size_t> encode_header(const FrameHeader& hdr,
                                                     std::span<uint8_t> buf) noexcept {
    if (buf.size() < sizeof(FrameHeader)) {
        return std::unexpected(SystemError::BufferOverflow);
    }
    std::memcpy(buf.data(), &hdr, sizeof(FrameHeader));
    return sizeof(FrameHeader);
}

inline constexpr uint32_t EXEC_FLAG_STRIP_ANSI = 1 << 0;
inline constexpr uint32_t EXEC_FLAG_CAPTURE_DIFF = 1 << 1;

inline constexpr uint32_t EXEC_RESP_TRUNCATED = 1 << 0;
inline constexpr uint32_t EXEC_RESP_TIMED_OUT = 1 << 1;

inline constexpr size_t EXEC_REQ_HEADER_SIZE = 12;
inline constexpr size_t EXEC_RESP_HEADER_SIZE = 24;

/// Request payload header for Opcode::Exec.
struct [[gnu::packed]] ExecRequestHeader {
    uint32_t flags{0};
    uint32_t timeout_ms{5000};
    uint16_t cmd_len{0};
    uint16_t cwd_len{0};
};

static_assert(sizeof(ExecRequestHeader) == EXEC_REQ_HEADER_SIZE,
              "ExecRequestHeader must be 12 bytes");

/// Response payload header for Opcode::ExecOutput.
struct [[gnu::packed]] ExecResponseHeader {
    int32_t exit_code{0};
    uint32_t flags{0};
    uint64_t cpu_cycles{0};
    uint32_t output_len{0};
    uint32_t diff_len{0};
};

static_assert(sizeof(ExecResponseHeader) == EXEC_RESP_HEADER_SIZE,
              "ExecResponseHeader must be 24 bytes");

/// Decodes an ExecRequestHeader from a raw buffer.
[[nodiscard]] constexpr auto decode_exec_request(std::span<const uint8_t> buffer)
    -> Result<ExecRequestHeader> {
    if (buffer.size() < sizeof(ExecRequestHeader)) {
        return std::unexpected(SystemError::BufferUnderflow);
    }
    ExecRequestHeader req{};
    std::memcpy(&req, buffer.data(), sizeof(ExecRequestHeader));
    return req;
}

/// Serializes an ExecRequestHeader into a target buffer.
[[nodiscard]] constexpr auto encode_exec_request(const ExecRequestHeader& req,
                                                 std::span<uint8_t> buf) noexcept
    -> Result<size_t> {
    if (buf.size() < sizeof(ExecRequestHeader)) {
        return std::unexpected(SystemError::BufferOverflow);
    }
    std::memcpy(buf.data(), &req, sizeof(ExecRequestHeader));
    return sizeof(ExecRequestHeader);
}

/// Decodes an ExecResponseHeader from a raw buffer.
[[nodiscard]] constexpr auto decode_exec_response(std::span<const uint8_t> buffer)
    -> Result<ExecResponseHeader> {
    if (buffer.size() < sizeof(ExecResponseHeader)) {
        return std::unexpected(SystemError::BufferUnderflow);
    }
    ExecResponseHeader resp{};
    std::memcpy(&resp, buffer.data(), sizeof(ExecResponseHeader));
    return resp;
}

/// Serializes an ExecResponseHeader into a target buffer.
[[nodiscard]] constexpr auto encode_exec_response(const ExecResponseHeader& resp,
                                                  std::span<uint8_t> buf) noexcept
    -> Result<size_t> {
    if (buf.size() < sizeof(ExecResponseHeader)) {
        return std::unexpected(SystemError::BufferOverflow);
    }
    std::memcpy(buf.data(), &resp, sizeof(ExecResponseHeader));
    return sizeof(ExecResponseHeader);
}

inline constexpr size_t AST_SYMBOLS_REQ_HEADER_SIZE = 4;
inline constexpr size_t AST_SYMBOLS_RESP_HEADER_SIZE = 12;

/// Request payload header for Opcode::AstSymbols.
struct [[gnu::packed]] AstSymbolsRequestHeader {
    uint32_t path_len{0};
};

static_assert(sizeof(AstSymbolsRequestHeader) == AST_SYMBOLS_REQ_HEADER_SIZE,
              "AstSymbolsRequestHeader must be 4 bytes");

/// Response payload header for Opcode::AstSymbolsResponse.
struct [[gnu::packed]] AstSymbolsResponseHeader {
    int32_t status{0};
    uint32_t symbol_count{0};
    uint32_t payload_len{0};
};

static_assert(sizeof(AstSymbolsResponseHeader) == AST_SYMBOLS_RESP_HEADER_SIZE,
              "AstSymbolsResponseHeader must be 12 bytes");

/// Decodes an AstSymbolsRequestHeader from a raw buffer.
[[nodiscard]] constexpr auto decode_ast_symbols_request(std::span<const uint8_t> buffer)
    -> Result<AstSymbolsRequestHeader> {
    if (buffer.size() < sizeof(AstSymbolsRequestHeader)) {
        return std::unexpected(SystemError::BufferUnderflow);
    }
    AstSymbolsRequestHeader req{};
    std::memcpy(&req, buffer.data(), sizeof(AstSymbolsRequestHeader));
    return req;
}

/// Serializes an AstSymbolsRequestHeader into a target buffer.
[[nodiscard]] constexpr auto encode_ast_symbols_request(const AstSymbolsRequestHeader& req,
                                                        std::span<uint8_t> buf) noexcept
    -> Result<size_t> {
    if (buf.size() < sizeof(AstSymbolsRequestHeader)) {
        return std::unexpected(SystemError::BufferOverflow);
    }
    std::memcpy(buf.data(), &req, sizeof(AstSymbolsRequestHeader));
    return sizeof(AstSymbolsRequestHeader);
}

/// Decodes an AstSymbolsResponseHeader from a raw buffer.
[[nodiscard]] constexpr auto decode_ast_symbols_response(std::span<const uint8_t> buffer)
    -> Result<AstSymbolsResponseHeader> {
    if (buffer.size() < sizeof(AstSymbolsResponseHeader)) {
        return std::unexpected(SystemError::BufferUnderflow);
    }
    AstSymbolsResponseHeader resp{};
    std::memcpy(&resp, buffer.data(), sizeof(AstSymbolsResponseHeader));
    return resp;
}

/// Serializes an AstSymbolsResponseHeader into a target buffer.
[[nodiscard]] constexpr auto encode_ast_symbols_response(const AstSymbolsResponseHeader& resp,
                                                         std::span<uint8_t> buf) noexcept
    -> Result<size_t> {
    if (buf.size() < sizeof(AstSymbolsResponseHeader)) {
        return std::unexpected(SystemError::BufferOverflow);
    }
    std::memcpy(buf.data(), &resp, sizeof(AstSymbolsResponseHeader));
    return sizeof(AstSymbolsResponseHeader);
}

inline constexpr size_t AST_SLICE_REQ_HEADER_SIZE = 4;
inline constexpr size_t AST_SLICE_RESP_HEADER_SIZE = 16;

/// Request payload header for Opcode::AstSlice.
struct [[gnu::packed]] AstSliceRequestHeader {
    uint16_t path_len{0};
    uint16_t symbol_len{0};
};

static_assert(sizeof(AstSliceRequestHeader) == AST_SLICE_REQ_HEADER_SIZE,
              "AstSliceRequestHeader must be 4 bytes");

/// Response payload header for Opcode::AstSliceResponse.
struct [[gnu::packed]] AstSliceResponseHeader {
    int32_t status{0};
    uint32_t start_line{0};
    uint32_t end_line{0};
    uint32_t content_len{0};
};

static_assert(sizeof(AstSliceResponseHeader) == AST_SLICE_RESP_HEADER_SIZE,
              "AstSliceResponseHeader must be 16 bytes");

/// Decodes an AstSliceRequestHeader from a raw buffer.
[[nodiscard]] constexpr auto decode_ast_slice_request(std::span<const uint8_t> buffer)
    -> Result<AstSliceRequestHeader> {
    if (buffer.size() < sizeof(AstSliceRequestHeader)) {
        return std::unexpected(SystemError::BufferUnderflow);
    }
    AstSliceRequestHeader req{};
    std::memcpy(&req, buffer.data(), sizeof(AstSliceRequestHeader));
    return req;
}

/// Serializes an AstSliceRequestHeader into a target buffer.
[[nodiscard]] constexpr auto encode_ast_slice_request(const AstSliceRequestHeader& req,
                                                      std::span<uint8_t> buf) noexcept
    -> Result<size_t> {
    if (buf.size() < sizeof(AstSliceRequestHeader)) {
        return std::unexpected(SystemError::BufferOverflow);
    }
    std::memcpy(buf.data(), &req, sizeof(AstSliceRequestHeader));
    return sizeof(AstSliceRequestHeader);
}

/// Decodes an AstSliceResponseHeader from a raw buffer.
[[nodiscard]] constexpr auto decode_ast_slice_response(std::span<const uint8_t> buffer)
    -> Result<AstSliceResponseHeader> {
    if (buffer.size() < sizeof(AstSliceResponseHeader)) {
        return std::unexpected(SystemError::BufferUnderflow);
    }
    AstSliceResponseHeader resp{};
    std::memcpy(&resp, buffer.data(), sizeof(AstSliceResponseHeader));
    return resp;
}

/// Serializes an AstSliceResponseHeader into a target buffer.
[[nodiscard]] constexpr auto encode_ast_slice_response(const AstSliceResponseHeader& resp,
                                                       std::span<uint8_t> buf) noexcept
    -> Result<size_t> {
    if (buf.size() < sizeof(AstSliceResponseHeader)) {
        return std::unexpected(SystemError::BufferOverflow);
    }
    std::memcpy(buf.data(), &resp, sizeof(AstSliceResponseHeader));
    return sizeof(AstSliceResponseHeader);
}

inline constexpr size_t AST_PATCH_REQ_HEADER_SIZE = 8;
inline constexpr size_t AST_PATCH_RESP_HEADER_SIZE = 16;

/// Request payload header for Opcode::AstPatch.
struct [[gnu::packed]] AstPatchRequestHeader {
    uint16_t path_len{0};
    uint16_t symbol_len{0};
    uint32_t replacement_len{0};
};

static_assert(sizeof(AstPatchRequestHeader) == AST_PATCH_REQ_HEADER_SIZE,
              "AstPatchRequestHeader must be 8 bytes");

/// Response payload header for Opcode::AstPatchResponse.
struct [[gnu::packed]] AstPatchResponseHeader {
    int32_t status{0};
    uint32_t old_start_line{0};
    uint32_t old_end_line{0};
    uint32_t new_end_line{0};
};

static_assert(sizeof(AstPatchResponseHeader) == AST_PATCH_RESP_HEADER_SIZE,
              "AstPatchResponseHeader must be 16 bytes");

/// Decodes an AstPatchRequestHeader from a raw buffer.
[[nodiscard]] constexpr auto decode_ast_patch_request(std::span<const uint8_t> buffer)
    -> Result<AstPatchRequestHeader> {
    if (buffer.size() < sizeof(AstPatchRequestHeader)) {
        return std::unexpected(SystemError::BufferUnderflow);
    }
    AstPatchRequestHeader req{};
    std::memcpy(&req, buffer.data(), sizeof(AstPatchRequestHeader));
    return req;
}

/// Serializes an AstPatchRequestHeader into a target buffer.
[[nodiscard]] constexpr auto encode_ast_patch_request(const AstPatchRequestHeader& req,
                                                      std::span<uint8_t> buf) noexcept
    -> Result<size_t> {
    if (buf.size() < sizeof(AstPatchRequestHeader)) {
        return std::unexpected(SystemError::BufferOverflow);
    }
    std::memcpy(buf.data(), &req, sizeof(AstPatchRequestHeader));
    return sizeof(AstPatchRequestHeader);
}

/// Decodes an AstPatchResponseHeader from a raw buffer.
[[nodiscard]] constexpr auto decode_ast_patch_response(std::span<const uint8_t> buffer)
    -> Result<AstPatchResponseHeader> {
    if (buffer.size() < sizeof(AstPatchResponseHeader)) {
        return std::unexpected(SystemError::BufferUnderflow);
    }
    AstPatchResponseHeader resp{};
    std::memcpy(&resp, buffer.data(), sizeof(AstPatchResponseHeader));
    return resp;
}

/// Serializes an AstPatchResponseHeader into a target buffer.
[[nodiscard]] constexpr auto encode_ast_patch_response(const AstPatchResponseHeader& resp,
                                                       std::span<uint8_t> buf) noexcept
    -> Result<size_t> {
    if (buf.size() < sizeof(AstPatchResponseHeader)) {
        return std::unexpected(SystemError::BufferOverflow);
    }
    std::memcpy(buf.data(), &resp, sizeof(AstPatchResponseHeader));
    return sizeof(AstPatchResponseHeader);
}

/// Supported browser action types for CDP interaction.
enum class CdpActionType : uint8_t {
    Navigate = 1,
    GetTree = 2,
    Click = 3,
    Type = 4,
    Scroll = 5,
};

inline constexpr size_t CDP_REQ_HEADER_SIZE = 12;
inline constexpr size_t CDP_RESP_HEADER_SIZE = 12;

/// Request payload header for Opcode::CdpAction.
struct [[gnu::packed]] CdpActionRequestHeader {
    uint8_t action{0};
    uint8_t flags{0};
    uint16_t target_id{0};
    int32_t scroll_delta{0};
    uint16_t payload1_len{0};
    uint16_t payload2_len{0};
};

static_assert(sizeof(CdpActionRequestHeader) == CDP_REQ_HEADER_SIZE,
              "CdpActionRequestHeader must be 12 bytes");

/// Response payload header for Opcode::CdpActionResponse.
struct [[gnu::packed]] CdpActionResponseHeader {
    int32_t status{0};
    uint32_t node_count{0};
    uint32_t payload_len{0};
};

static_assert(sizeof(CdpActionResponseHeader) == CDP_RESP_HEADER_SIZE,
              "CdpActionResponseHeader must be 12 bytes");

/// Decodes a CdpActionRequestHeader from a raw buffer.
[[nodiscard]] constexpr auto decode_cdp_request(std::span<const uint8_t> buffer)
    -> Result<CdpActionRequestHeader> {
    if (buffer.size() < sizeof(CdpActionRequestHeader)) {
        return std::unexpected(SystemError::BufferUnderflow);
    }
    CdpActionRequestHeader req{};
    std::memcpy(&req, buffer.data(), sizeof(CdpActionRequestHeader));
    return req;
}

/// Serializes a CdpActionRequestHeader into a target buffer.
[[nodiscard]] constexpr auto encode_cdp_request(const CdpActionRequestHeader& req,
                                                std::span<uint8_t> buf) noexcept -> Result<size_t> {
    if (buf.size() < sizeof(CdpActionRequestHeader)) {
        return std::unexpected(SystemError::BufferOverflow);
    }
    std::memcpy(buf.data(), &req, sizeof(CdpActionRequestHeader));
    return sizeof(CdpActionRequestHeader);
}

/// Decodes a CdpActionResponseHeader from a raw buffer.
[[nodiscard]] constexpr auto decode_cdp_response(std::span<const uint8_t> buffer)
    -> Result<CdpActionResponseHeader> {
    if (buffer.size() < sizeof(CdpActionResponseHeader)) {
        return std::unexpected(SystemError::BufferUnderflow);
    }
    CdpActionResponseHeader resp{};
    std::memcpy(&resp, buffer.data(), sizeof(CdpActionResponseHeader));
    return resp;
}

/// Serializes a CdpActionResponseHeader into a target buffer.
[[nodiscard]] constexpr auto encode_cdp_response(const CdpActionResponseHeader& resp,
                                                 std::span<uint8_t> buf) noexcept
    -> Result<size_t> {
    if (buf.size() < sizeof(CdpActionResponseHeader)) {
        return std::unexpected(SystemError::BufferOverflow);
    }
    std::memcpy(buf.data(), &resp, sizeof(CdpActionResponseHeader));
    return sizeof(CdpActionResponseHeader);
}

inline constexpr size_t READ_FILE_REQ_HEADER_SIZE = 12;
inline constexpr size_t READ_FILE_RESP_HEADER_SIZE = 12;

/// Request payload header for Opcode::ReadFile.
struct [[gnu::packed]] ReadFileRequestHeader {
    uint32_t offset{0};
    uint32_t max_bytes{0};
    uint16_t path_len{0};
    uint16_t reserved{0};
};

static_assert(sizeof(ReadFileRequestHeader) == READ_FILE_REQ_HEADER_SIZE,
              "ReadFileRequestHeader must be 12 bytes");

/// Response payload header for Opcode::ReadFileResponse.
struct [[gnu::packed]] ReadFileResponseHeader {
    int32_t status{0};
    uint32_t total_size{0};
    uint32_t content_len{0};
};

static_assert(sizeof(ReadFileResponseHeader) == READ_FILE_RESP_HEADER_SIZE,
              "ReadFileResponseHeader must be 12 bytes");

inline constexpr size_t WRITE_FILE_REQ_HEADER_SIZE = 16;
inline constexpr size_t WRITE_FILE_RESP_HEADER_SIZE = 8;

/// Request payload header for Opcode::WriteFile.
struct [[gnu::packed]] WriteFileRequestHeader {
    uint32_t flags{0};
    uint32_t mode{0644};
    uint16_t path_len{0};
    uint16_t reserved{0};
    uint32_t content_len{0};
};

static_assert(sizeof(WriteFileRequestHeader) == WRITE_FILE_REQ_HEADER_SIZE,
              "WriteFileRequestHeader must be 16 bytes");

/// Response payload header for Opcode::WriteFileResponse.
struct [[gnu::packed]] WriteFileResponseHeader {
    int32_t status{0};
    uint32_t bytes_written{0};
};

static_assert(sizeof(WriteFileResponseHeader) == WRITE_FILE_RESP_HEADER_SIZE,
              "WriteFileResponseHeader must be 8 bytes");

/// Decodes a ReadFileRequestHeader from a raw buffer.
[[nodiscard]] constexpr auto decode_read_file_request(std::span<const uint8_t> buffer)
    -> Result<ReadFileRequestHeader> {
    if (buffer.size() < sizeof(ReadFileRequestHeader)) {
        return std::unexpected(SystemError::BufferUnderflow);
    }
    ReadFileRequestHeader req{};
    std::memcpy(&req, buffer.data(), sizeof(ReadFileRequestHeader));
    return req;
}

/// Serializes a ReadFileResponseHeader into a target buffer.
[[nodiscard]] constexpr auto encode_read_file_response(const ReadFileResponseHeader& resp,
                                                       std::span<uint8_t> buf) noexcept
    -> Result<size_t> {
    if (buf.size() < sizeof(ReadFileResponseHeader)) {
        return std::unexpected(SystemError::BufferOverflow);
    }
    std::memcpy(buf.data(), &resp, sizeof(ReadFileResponseHeader));
    return sizeof(ReadFileResponseHeader);
}

/// Decodes a WriteFileRequestHeader from a raw buffer.
[[nodiscard]] constexpr auto decode_write_file_request(std::span<const uint8_t> buffer)
    -> Result<WriteFileRequestHeader> {
    if (buffer.size() < sizeof(WriteFileRequestHeader)) {
        return std::unexpected(SystemError::BufferUnderflow);
    }
    WriteFileRequestHeader req{};
    std::memcpy(&req, buffer.data(), sizeof(WriteFileRequestHeader));
    return req;
}

/// Serializes a WriteFileResponseHeader into a target buffer.
[[nodiscard]] constexpr auto encode_write_file_response(const WriteFileResponseHeader& resp,
                                                        std::span<uint8_t> buf) noexcept
    -> Result<size_t> {
    if (buf.size() < sizeof(WriteFileResponseHeader)) {
        return std::unexpected(SystemError::BufferOverflow);
    }
    std::memcpy(buf.data(), &resp, sizeof(WriteFileResponseHeader));
    return sizeof(WriteFileResponseHeader);
}

} // namespace axiom::wire
