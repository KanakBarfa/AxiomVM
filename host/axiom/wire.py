"""TDS-Wire protocol codec for host orchestration."""

import struct
from dataclasses import dataclass
from enum import IntEnum

WIRE_MAGIC = 0xAA55
HEADER_SIZE = 12
HEADER_FORMAT = "<HHII"


class Opcode(IntEnum):
    """Operation codes supported by TDS-Wire protocol."""

    PING = 0x0001
    PONG = 0x0002
    EXEC = 0x0010
    EXEC_OUTPUT = 0x0011
    AST_SYMBOLS = 0x0020
    AST_SLICE = 0x0021
    AST_PATCH = 0x0022
    AST_SYMBOLS_RESPONSE = 0x0023
    AST_SLICE_RESPONSE = 0x0024
    AST_PATCH_RESPONSE = 0x0025
    CDP_ACTION = 0x0030
    CDP_ACTION_RESPONSE = 0x0031
    SHUTDOWN = 0x00FF


@dataclass(frozen=True, slots=True)
class FrameHeader:
    """Wire protocol header structure matching C++ memory topology."""

    magic: int
    opcode: int
    request_id: int
    payload_len: int


def encode_frame(opcode: int, request_id: int, payload: bytes = b"") -> bytes:
    """Encodes a TDS-Wire frame with header and optional payload bytes."""
    header = struct.pack(HEADER_FORMAT, WIRE_MAGIC, opcode, request_id, len(payload))
    return header + payload


def decode_header(buffer: bytes) -> FrameHeader:
    """Decodes a frame header from raw buffer bytes."""
    if len(buffer) < HEADER_SIZE:
        raise ValueError("Buffer underflow decoding frame header")
    magic, opcode, request_id, payload_len = struct.unpack(
        HEADER_FORMAT, buffer[:HEADER_SIZE]
    )
    if magic != WIRE_MAGIC:
        raise ValueError(f"Invalid magic 0x{magic:04X}, expected 0x{WIRE_MAGIC:04X}")
    return FrameHeader(
        magic=magic, opcode=opcode, request_id=request_id, payload_len=payload_len
    )


EXEC_FLAG_STRIP_ANSI = 1 << 0
EXEC_FLAG_CAPTURE_DIFF = 1 << 1

EXEC_RESP_TRUNCATED = 1 << 0
EXEC_RESP_TIMED_OUT = 1 << 1

EXEC_REQ_FORMAT = "<IIHH"
EXEC_REQ_HEADER_SIZE = 12

EXEC_RESP_FORMAT = "<iIQII"
EXEC_RESP_HEADER_SIZE = 24


@dataclass(frozen=True, slots=True)
class ExecRequest:
    """Request parameters for command execution inside appliance."""

    command: str
    cwd: str = ""
    timeout_ms: int = 5000
    flags: int = EXEC_FLAG_STRIP_ANSI | EXEC_FLAG_CAPTURE_DIFF


@dataclass(frozen=True, slots=True)
class ExecResponse:
    """Execution response returned by appliance daemon."""

    exit_code: int
    flags: int
    cpu_cycles: int
    output: str
    diff: str


def encode_exec_request(req: ExecRequest) -> bytes:
    """Serializes an ExecRequest into binary payload bytes."""
    cmd_bytes = req.command.encode("utf-8")
    cwd_bytes = req.cwd.encode("utf-8")
    hdr = struct.pack(
        EXEC_REQ_FORMAT,
        req.flags,
        req.timeout_ms,
        len(cmd_bytes),
        len(cwd_bytes),
    )
    return hdr + cmd_bytes + cwd_bytes


def decode_exec_response(payload: bytes) -> ExecResponse:
    """Parses binary response payload into an ExecResponse dataclass."""
    if len(payload) < EXEC_RESP_HEADER_SIZE:
        raise ValueError("Payload underflow decoding ExecResponse")
    exit_code, flags, cpu_cycles, output_len, diff_len = struct.unpack(
        EXEC_RESP_FORMAT, payload[:EXEC_RESP_HEADER_SIZE]
    )
    data = payload[EXEC_RESP_HEADER_SIZE:]
    if len(data) < output_len + diff_len:
        raise ValueError("Payload underflow for output and diff segments")
    output_bytes = data[:output_len]
    diff_bytes = data[output_len : output_len + diff_len]
    return ExecResponse(
        exit_code=exit_code,
        flags=flags,
        cpu_cycles=cpu_cycles,
        output=output_bytes.decode("utf-8", errors="replace"),
        diff=diff_bytes.decode("utf-8", errors="replace"),
    )


AST_SYMBOLS_REQ_FORMAT = "<I"
AST_SYMBOLS_REQ_HEADER_SIZE = 4
AST_SYMBOLS_RESP_FORMAT = "<iII"
AST_SYMBOLS_RESP_HEADER_SIZE = 12

AST_SLICE_REQ_FORMAT = "<HH"
AST_SLICE_REQ_HEADER_SIZE = 4
AST_SLICE_RESP_FORMAT = "<iIII"
AST_SLICE_RESP_HEADER_SIZE = 16

AST_PATCH_REQ_FORMAT = "<HHI"
AST_PATCH_REQ_HEADER_SIZE = 8
AST_PATCH_RESP_FORMAT = "<iIII"
AST_PATCH_RESP_HEADER_SIZE = 16


@dataclass(frozen=True, slots=True)
class AstSymbolsRequest:
    """Request parameters for querying symbol declarations in a file."""

    path: str


@dataclass(frozen=True, slots=True)
class AstSymbolsResponse:
    """Structured symbol outline returned from AST analysis."""

    status: int
    symbol_count: int
    symbols_text: str


def encode_ast_symbols_request(req: AstSymbolsRequest) -> bytes:
    """Serializes an AstSymbolsRequest into binary payload bytes."""
    path_bytes = req.path.encode("utf-8")
    hdr = struct.pack(AST_SYMBOLS_REQ_FORMAT, len(path_bytes))
    return hdr + path_bytes


def decode_ast_symbols_response(payload: bytes) -> AstSymbolsResponse:
    """Parses binary response payload into an AstSymbolsResponse dataclass."""
    if len(payload) < AST_SYMBOLS_RESP_HEADER_SIZE:
        raise ValueError("Payload underflow decoding AstSymbolsResponse")
    status, symbol_count, payload_len = struct.unpack(
        AST_SYMBOLS_RESP_FORMAT, payload[:AST_SYMBOLS_RESP_HEADER_SIZE]
    )
    data = payload[AST_SYMBOLS_RESP_HEADER_SIZE:]
    if len(data) < payload_len:
        raise ValueError("Payload underflow for symbols text")
    symbols_text = data[:payload_len].decode("utf-8", errors="replace")
    return AstSymbolsResponse(
        status=status, symbol_count=symbol_count, symbols_text=symbols_text
    )


@dataclass(frozen=True, slots=True)
class AstSliceRequest:
    """Request parameters for extracting the source slice of a symbol."""

    path: str
    symbol: str


@dataclass(frozen=True, slots=True)
class AstSliceResponse:
    """Symbol slice content and line boundaries returned from AST analysis."""

    status: int
    start_line: int
    end_line: int
    content: str


def encode_ast_slice_request(req: AstSliceRequest) -> bytes:
    """Serializes an AstSliceRequest into binary payload bytes."""
    path_bytes = req.path.encode("utf-8")
    sym_bytes = req.symbol.encode("utf-8")
    hdr = struct.pack(AST_SLICE_REQ_FORMAT, len(path_bytes), len(sym_bytes))
    return hdr + path_bytes + sym_bytes


def decode_ast_slice_response(payload: bytes) -> AstSliceResponse:
    """Parses binary response payload into an AstSliceResponse dataclass."""
    if len(payload) < AST_SLICE_RESP_HEADER_SIZE:
        raise ValueError("Payload underflow decoding AstSliceResponse")
    status, start_line, end_line, content_len = struct.unpack(
        AST_SLICE_RESP_FORMAT, payload[:AST_SLICE_RESP_HEADER_SIZE]
    )
    data = payload[AST_SLICE_RESP_HEADER_SIZE:]
    if len(data) < content_len:
        raise ValueError("Payload underflow for slice content")
    content = data[:content_len].decode("utf-8", errors="replace")
    return AstSliceResponse(
        status=status, start_line=start_line, end_line=end_line, content=content
    )


@dataclass(frozen=True, slots=True)
class AstPatchRequest:
    """Request parameters for patching a symbol with replacement code."""

    path: str
    symbol: str
    replacement: str


@dataclass(frozen=True, slots=True)
class AstPatchResponse:
    """Result of structural symbol replacement operation."""

    status: int
    old_start_line: int
    old_end_line: int
    new_end_line: int


def encode_ast_patch_request(req: AstPatchRequest) -> bytes:
    """Serializes an AstPatchRequest into binary payload bytes."""
    path_bytes = req.path.encode("utf-8")
    sym_bytes = req.symbol.encode("utf-8")
    rep_bytes = req.replacement.encode("utf-8")
    hdr = struct.pack(
        AST_PATCH_REQ_FORMAT, len(path_bytes), len(sym_bytes), len(rep_bytes)
    )
    return hdr + path_bytes + sym_bytes + rep_bytes


def decode_ast_patch_response(payload: bytes) -> AstPatchResponse:
    """Parses binary response payload into an AstPatchResponse dataclass."""
    if len(payload) < AST_PATCH_RESP_HEADER_SIZE:
        raise ValueError("Payload underflow decoding AstPatchResponse")
    status, old_start_line, old_end_line, new_end_line = struct.unpack(
        AST_PATCH_RESP_FORMAT, payload[:AST_PATCH_RESP_HEADER_SIZE]
    )
    return AstPatchResponse(
        status=status,
        old_start_line=old_start_line,
        old_end_line=old_end_line,
        new_end_line=new_end_line,
    )


class CdpActionType(IntEnum):
    """Supported browser action types for CDP interaction."""

    NAVIGATE = 1
    GET_TREE = 2
    CLICK = 3
    TYPE = 4
    SCROLL = 5


CDP_REQ_FORMAT = "<BBHiHH"
CDP_REQ_HEADER_SIZE = 12
CDP_RESP_FORMAT = "<iII"
CDP_RESP_HEADER_SIZE = 12


@dataclass(frozen=True, slots=True)
class CdpActionRequest:
    """Request parameters for browser CDP actions."""

    action: int
    target_id: int = 0
    scroll_delta: int = 0
    payload1: str = ""
    payload2: str = ""
    flags: int = 0


@dataclass(frozen=True, slots=True)
class CdpActionResponse:
    """Result of browser CDP action returned by appliance daemon."""

    status: int
    node_count: int
    tree_text: str


def encode_cdp_request(req: CdpActionRequest) -> bytes:
    """Serializes a CdpActionRequest into binary payload bytes."""
    p1_bytes = req.payload1.encode("utf-8")
    p2_bytes = req.payload2.encode("utf-8")
    hdr = struct.pack(
        CDP_REQ_FORMAT,
        req.action,
        req.flags,
        req.target_id,
        req.scroll_delta,
        len(p1_bytes),
        len(p2_bytes),
    )
    return hdr + p1_bytes + p2_bytes


def decode_cdp_response(payload: bytes) -> CdpActionResponse:
    """Parses binary response payload into a CdpActionResponse dataclass."""
    if len(payload) < CDP_RESP_HEADER_SIZE:
        raise ValueError("Payload underflow decoding CdpActionResponse")
    status, node_count, payload_len = struct.unpack(
        CDP_RESP_FORMAT, payload[:CDP_RESP_HEADER_SIZE]
    )
    data = payload[CDP_RESP_HEADER_SIZE:]
    if len(data) < payload_len:
        raise ValueError("Payload underflow for tree text")
    tree_text = data[:payload_len].decode("utf-8", errors="replace")
    return CdpActionResponse(
        status=status,
        node_count=node_count,
        tree_text=tree_text,
    )
