"""Firecracker microVM controller and virtio-vsock client."""

from __future__ import annotations

import contextlib
import json
import os
import socket
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, BinaryIO, Self

from axiom.assets import resolve_assets
from axiom.snapshot import SnapshotInfo, copy_or_reflink, update_snapshot_state
from axiom.wire import (
    EXEC_FLAG_CAPTURE_DIFF,
    EXEC_FLAG_STRIP_ANSI,
    HEADER_SIZE,
    AstPatchRequest,
    AstPatchResponse,
    AstSliceRequest,
    AstSliceResponse,
    AstSymbolsRequest,
    AstSymbolsResponse,
    CdpActionRequest,
    CdpActionResponse,
    CdpActionType,
    ExecRequest,
    ExecResponse,
    Opcode,
    ReadFileRequest,
    ReadFileResponse,
    WriteFileRequest,
    WriteFileResponse,
    decode_ast_patch_response,
    decode_ast_slice_response,
    decode_ast_symbols_response,
    decode_cdp_response,
    decode_exec_response,
    decode_header,
    decode_read_file_response,
    decode_write_file_response,
    encode_ast_patch_request,
    encode_ast_slice_request,
    encode_ast_symbols_request,
    encode_cdp_request,
    encode_exec_request,
    encode_frame,
    encode_read_file_request,
    encode_write_file_request,
)


@dataclass(frozen=True, slots=True)
class MountSpec:
    """Represents a host to guest directory mapping."""

    host_path: Path
    guest_path: str


class MicroVMError(RuntimeError):
    """Raised when microVM lifecycle operations fail."""


class MicroVM:
    """Controls a Firecracker microVM instance over Unix domain sockets."""

    def __init__(
        self,
        kernel_path: str | Path | None = None,
        rootfs_path: str | Path | None = None,
        firecracker_bin: str | Path = "/usr/local/bin/firecracker",
        vcpu_count: int = 1,
        mem_size_mib: int = 128,
        guest_cid: int = 3,
        guest_port: int = 5200,
        work_dir: str | Path = "/tmp/axiom_vm",
        mounts: list[str] | list[MountSpec] | None = None,
    ) -> None:
        """Initializes MicroVM configuration parameters."""
        if kernel_path is None or rootfs_path is None:
            resolved_k, resolved_r = resolve_assets()
            if kernel_path is None:
                if resolved_k is None:
                    raise MicroVMError(
                        "Kernel path not specified and could not be resolved"
                    )
                kernel_path = resolved_k
            if rootfs_path is None:
                if resolved_r is None:
                    raise MicroVMError(
                        "Rootfs path not specified and could not be resolved"
                    )
                rootfs_path = resolved_r

        self.kernel_path = Path(kernel_path).resolve()
        self.rootfs_path = Path(rootfs_path).resolve()
        self.firecracker_bin = str(firecracker_bin)
        self.vcpu_count = vcpu_count
        self.mem_size_mib = mem_size_mib
        self.guest_cid = guest_cid
        self.guest_port = guest_port
        self.work_dir = Path(work_dir)
        self.boot_duration: float = 0.0

        self.mounts: list[MountSpec] = []
        if mounts:
            for m in mounts:
                if isinstance(m, MountSpec):
                    self.mounts.append(m)
                elif isinstance(m, str):
                    if ":" in m:
                        hp, gp = m.split(":", 1)
                    else:
                        hp, gp = m, "/workspace"
                    self.mounts.append(
                        MountSpec(
                            host_path=Path(hp).resolve(), guest_path=gp.rstrip("/")
                        )
                    )

        self.api_sock = self.work_dir / "firecracker.sock"
        self.vsock_path = self.work_dir / "vsock.sock"
        self.log_path = self.work_dir / "firecracker.log"

        self.process: subprocess.Popen[bytes] | None = None
        self.vsock_conn: socket.socket | None = None
        self.log_file: BinaryIO | None = None

    def start(self) -> None:
        """Boots the microVM instance and configures kernel, drives, and vsock."""
        t_start = time.perf_counter()
        self.work_dir.mkdir(parents=True, exist_ok=True)
        self._cleanup_sockets()

        self.log_file = self.log_path.open("wb")
        self.process = subprocess.Popen(
            [
                self.firecracker_bin,
                "--api-sock",
                str(self.api_sock),
                "--id",
                "axiom-microvm",
            ],
            stdout=self.log_file,
            stderr=subprocess.STDOUT,
        )

        self._wait_for_api_socket()

        self._api_put(
            "/machine-config",
            {
                "vcpu_count": self.vcpu_count,
                "mem_size_mib": self.mem_size_mib,
            },
        )

        self._api_put(
            "/boot-source",
            {
                "kernel_image_path": str(self.kernel_path),
                "boot_args": "console=ttyS0 reboot=k panic=1 pci=off init=/init quiet",
            },
        )

        self._api_put(
            "/drives/rootfs",
            {
                "drive_id": "rootfs",
                "path_on_host": str(self.rootfs_path),
                "is_root_device": True,
                "is_read_only": False,
            },
        )

        self._api_put(
            "/vsock",
            {
                "guest_cid": self.guest_cid,
                "uds_path": str(self.vsock_path),
            },
        )

        self._api_put(
            "/actions",
            {
                "action_type": "InstanceStart",
            },
        )
        self.boot_duration = time.perf_counter() - t_start

    def connect_vsock(self, timeout_sec: float = 5.0) -> socket.socket:
        """Establishes stream connection to guest appliance daemon over virtio-vsock."""
        deadline = time.time() + timeout_sec
        last_err: Exception | None = None

        while time.time() < deadline:
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.settimeout(1.0)
                sock.connect(str(self.vsock_path))

                handshake = f"CONNECT {self.guest_port}\n".encode()
                sock.sendall(handshake)

                response = b""
                while not response.endswith(b"\n"):
                    chunk = sock.recv(128)
                    if not chunk:
                        break
                    response += chunk

                if response.startswith(b"OK"):
                    sock.settimeout(5.0)
                    self.vsock_conn = sock
                    return sock

                sock.close()
            except (TimeoutError, ConnectionRefusedError, FileNotFoundError) as exc:
                last_err = exc
                time.sleep(0.05)

        raise MicroVMError(
            f"Failed to connect to guest vsock port {self.guest_port}: {last_err}"
        )

    def ping(self, request_id: int = 1) -> tuple[Any, float]:
        """Transmits Ping frame over vsock and returns decoded header and round-trip latency."""
        if not self.vsock_conn:
            self.connect_vsock()

        assert self.vsock_conn is not None
        frame = encode_frame(Opcode.PING, request_id)

        t_start = time.perf_counter()
        self.vsock_conn.sendall(frame)

        resp = b""
        while len(resp) < HEADER_SIZE:
            chunk = self.vsock_conn.recv(HEADER_SIZE - len(resp))
            if not chunk:
                raise MicroVMError("Socket closed while awaiting Ping response")
            resp += chunk

        latency = time.perf_counter() - t_start
        header = decode_header(resp)
        return header, latency

    def exec(
        self,
        command: str,
        cwd: str = "",
        timeout_ms: int = 5000,
        strip_ansi: bool = True,
        capture_diff: bool = True,
        sync_mounts: bool = True,
        request_id: int = 100,
    ) -> tuple[ExecResponse, float]:
        """Executes a command inside the appliance over vsock and returns response and latency."""
        if not self.vsock_conn:
            self.connect_vsock()

        assert self.vsock_conn is not None

        if sync_mounts and self.mounts:
            self.sync_mounts_to_guest()

        effective_cwd = cwd
        if not effective_cwd and self.mounts:
            effective_cwd = self.mounts[0].guest_path

        flags = 0
        if strip_ansi:
            flags |= EXEC_FLAG_STRIP_ANSI
        if capture_diff:
            flags |= EXEC_FLAG_CAPTURE_DIFF

        req = ExecRequest(
            command=command,
            cwd=effective_cwd,
            timeout_ms=timeout_ms,
            flags=flags,
        )
        payload = encode_exec_request(req)
        frame = encode_frame(Opcode.EXEC, request_id, payload)

        t_start = time.perf_counter()
        self.vsock_conn.sendall(frame)

        resp_header_bytes = b""
        while len(resp_header_bytes) < HEADER_SIZE:
            chunk = self.vsock_conn.recv(HEADER_SIZE - len(resp_header_bytes))
            if not chunk:
                raise MicroVMError("Socket closed while awaiting Exec response header")
            resp_header_bytes += chunk

        header = decode_header(resp_header_bytes)
        if header.opcode != Opcode.EXEC_OUTPUT:
            raise MicroVMError(
                f"Unexpected opcode in response: {header.opcode:04X}, expected {Opcode.EXEC_OUTPUT:04X}"
            )

        resp_payload = b""
        while len(resp_payload) < header.payload_len:
            chunk = self.vsock_conn.recv(header.payload_len - len(resp_payload))
            if not chunk:
                raise MicroVMError("Socket closed while awaiting Exec response payload")
            resp_payload += chunk

        latency = time.perf_counter() - t_start
        exec_resp = decode_exec_response(resp_payload)

        if sync_mounts and self.mounts and capture_diff and exec_resp.diff:
            self.sync_mounts_from_guest(exec_resp.diff)

        return exec_resp, latency

    def read_file(
        self,
        path: str,
        offset: int = 0,
        max_bytes: int = 65536,
        request_id: int = 210,
    ) -> tuple[ReadFileResponse, float]:
        """Reads file bytes directly from appliance over virtio-vsock."""
        if not self.vsock_conn:
            self.connect_vsock()

        assert self.vsock_conn is not None

        req = ReadFileRequest(path=path, offset=offset, max_bytes=max_bytes)
        payload = encode_read_file_request(req)
        frame = encode_frame(Opcode.READ_FILE, request_id, payload)

        t_start = time.perf_counter()
        self.vsock_conn.sendall(frame)

        resp_header_bytes = b""
        while len(resp_header_bytes) < HEADER_SIZE:
            chunk = self.vsock_conn.recv(HEADER_SIZE - len(resp_header_bytes))
            if not chunk:
                raise MicroVMError(
                    "Socket closed while awaiting ReadFile response header"
                )
            resp_header_bytes += chunk

        header = decode_header(resp_header_bytes)
        if header.opcode != Opcode.READ_FILE_RESPONSE:
            raise MicroVMError(
                f"Unexpected opcode in response: {header.opcode:04X}, expected {Opcode.READ_FILE_RESPONSE:04X}"
            )

        resp_payload = b""
        while len(resp_payload) < header.payload_len:
            chunk = self.vsock_conn.recv(header.payload_len - len(resp_payload))
            if not chunk:
                raise MicroVMError(
                    "Socket closed while awaiting ReadFile response payload"
                )
            resp_payload += chunk

        latency = time.perf_counter() - t_start
        resp = decode_read_file_response(resp_payload)
        return resp, latency

    def write_file(
        self,
        path: str,
        content: bytes | str,
        append: bool = False,
        mode: int = 0o644,
        request_id: int = 211,
    ) -> tuple[WriteFileResponse, float]:
        """Writes binary or text content to guest filesystem over virtio-vsock."""
        if not self.vsock_conn:
            self.connect_vsock()

        assert self.vsock_conn is not None

        content_bytes = content.encode("utf-8") if isinstance(content, str) else content
        req = WriteFileRequest(
            path=path, content=content_bytes, append=append, mode=mode
        )
        payload = encode_write_file_request(req)
        frame = encode_frame(Opcode.WRITE_FILE, request_id, payload)

        t_start = time.perf_counter()
        self.vsock_conn.sendall(frame)

        resp_header_bytes = b""
        while len(resp_header_bytes) < HEADER_SIZE:
            chunk = self.vsock_conn.recv(HEADER_SIZE - len(resp_header_bytes))
            if not chunk:
                raise MicroVMError(
                    "Socket closed while awaiting WriteFile response header"
                )
            resp_header_bytes += chunk

        header = decode_header(resp_header_bytes)
        if header.opcode != Opcode.WRITE_FILE_RESPONSE:
            raise MicroVMError(
                f"Unexpected opcode in response: {header.opcode:04X}, expected {Opcode.WRITE_FILE_RESPONSE:04X}"
            )

        resp_payload = b""
        while len(resp_payload) < header.payload_len:
            chunk = self.vsock_conn.recv(header.payload_len - len(resp_payload))
            if not chunk:
                raise MicroVMError(
                    "Socket closed while awaiting WriteFile response payload"
                )
            resp_payload += chunk

        latency = time.perf_counter() - t_start
        resp = decode_write_file_response(resp_payload)
        return resp, latency

    def read_file_all(self, path: str) -> bytes:
        """Reads complete file content from appliance across chunk boundaries."""
        resp, _ = self.read_file(path, offset=0, max_bytes=0)
        if resp.status != 0:
            raise MicroVMError(f"Failed to read file '{path}': status {resp.status}")
        data = bytearray(resp.content)
        while len(data) < resp.total_size:
            chunk_resp, _ = self.read_file(path, offset=len(data), max_bytes=0)
            if chunk_resp.status != 0 or not chunk_resp.content:
                break
            data.extend(chunk_resp.content)
        return bytes(data)

    def write_file_all(
        self,
        path: str,
        content: bytes | str,
        mode: int = 0o644,
        chunk_size: int = 32768,
    ) -> int:
        """Writes arbitrary-length content to guest file using chunked appends."""
        content_bytes = content.encode("utf-8") if isinstance(content, str) else content
        if not content_bytes:
            resp, _ = self.write_file(path, b"", append=False, mode=mode)
            return resp.bytes_written

        total_written = 0
        for i in range(0, len(content_bytes), chunk_size):
            chunk = content_bytes[i : i + chunk_size]
            append = i > 0
            resp, _ = self.write_file(path, chunk, append=append, mode=mode)
            if resp.status != 0:
                raise MicroVMError(
                    f"Failed writing chunk to '{path}': status {resp.status}"
                )
            total_written += resp.bytes_written
        return total_written

    def sync_mounts_to_guest(self) -> int:
        """Synchronizes all configured host mount directories into the guest microVM."""
        total_files = 0
        for m in self.mounts:
            if not m.host_path.is_dir():
                continue
            for root, dirs, files in os.walk(m.host_path):
                dirs[:] = [
                    d
                    for d in dirs
                    if d not in {".git", ".venv", "__pycache__", "build", "target"}
                ]
                for file_name in files:
                    local_file = Path(root) / file_name
                    rel_path = local_file.relative_to(m.host_path)
                    guest_file = f"{m.guest_path}/{rel_path.as_posix()}"
                    try:
                        content = local_file.read_bytes()
                        self.write_file_all(guest_file, content)
                        total_files += 1
                    except OSError:
                        pass
        return total_files

    def sync_mounts_from_guest(self, diff_text: str | None = None) -> int:
        """Synchronizes modified or created files from guest back to host directories."""
        if not self.mounts:
            return 0
        total_synced = 0
        if diff_text:
            for line in diff_text.splitlines():
                line = line.strip()
                if not line:
                    continue
                tokens = line.split()
                if len(tokens) < 2:
                    continue
                action, guest_file = tokens[0], tokens[1]
                for m in self.mounts:
                    if guest_file == m.guest_path or guest_file.startswith(
                        m.guest_path + "/"
                    ):
                        rel_path = guest_file[len(m.guest_path) :].lstrip("/")
                        target_file = m.host_path / rel_path
                        if action in ("+", "M"):
                            with contextlib.suppress(Exception):
                                data = self.read_file_all(guest_file)
                                target_file.parent.mkdir(parents=True, exist_ok=True)
                                target_file.write_bytes(data)
                                total_synced += 1
                        elif action == "-":
                            with contextlib.suppress(OSError):
                                target_file.unlink(missing_ok=True)
                                total_synced += 1
        return total_synced

    def get_symbols(
        self,
        path: str,
        request_id: int = 200,
    ) -> tuple[AstSymbolsResponse, float]:
        """Queries high-level symbol declarations from a file over virtio-vsock."""
        if not self.vsock_conn:
            self.connect_vsock()

        assert self.vsock_conn is not None

        req = AstSymbolsRequest(path=path)
        payload = encode_ast_symbols_request(req)
        frame = encode_frame(Opcode.AST_SYMBOLS, request_id, payload)

        t_start = time.perf_counter()
        self.vsock_conn.sendall(frame)

        resp_header_bytes = b""
        while len(resp_header_bytes) < HEADER_SIZE:
            chunk = self.vsock_conn.recv(HEADER_SIZE - len(resp_header_bytes))
            if not chunk:
                raise MicroVMError(
                    "Socket closed while awaiting AstSymbols response header"
                )
            resp_header_bytes += chunk

        header = decode_header(resp_header_bytes)
        if header.opcode != Opcode.AST_SYMBOLS_RESPONSE:
            raise MicroVMError(
                f"Unexpected opcode in response: {header.opcode:04X}, expected {Opcode.AST_SYMBOLS_RESPONSE:04X}"
            )

        resp_payload = b""
        while len(resp_payload) < header.payload_len:
            chunk = self.vsock_conn.recv(header.payload_len - len(resp_payload))
            if not chunk:
                raise MicroVMError(
                    "Socket closed while awaiting AstSymbols response payload"
                )
            resp_payload += chunk

        latency = time.perf_counter() - t_start
        resp = decode_ast_symbols_response(resp_payload)
        return resp, latency

    def get_slice(
        self,
        path: str,
        symbol: str,
        request_id: int = 201,
    ) -> tuple[AstSliceResponse, float]:
        """Extracts source lines for a target symbol in a file over virtio-vsock."""
        if not self.vsock_conn:
            self.connect_vsock()

        assert self.vsock_conn is not None

        req = AstSliceRequest(path=path, symbol=symbol)
        payload = encode_ast_slice_request(req)
        frame = encode_frame(Opcode.AST_SLICE, request_id, payload)

        t_start = time.perf_counter()
        self.vsock_conn.sendall(frame)

        resp_header_bytes = b""
        while len(resp_header_bytes) < HEADER_SIZE:
            chunk = self.vsock_conn.recv(HEADER_SIZE - len(resp_header_bytes))
            if not chunk:
                raise MicroVMError(
                    "Socket closed while awaiting AstSlice response header"
                )
            resp_header_bytes += chunk

        header = decode_header(resp_header_bytes)
        if header.opcode != Opcode.AST_SLICE_RESPONSE:
            raise MicroVMError(
                f"Unexpected opcode in response: {header.opcode:04X}, expected {Opcode.AST_SLICE_RESPONSE:04X}"
            )

        resp_payload = b""
        while len(resp_payload) < header.payload_len:
            chunk = self.vsock_conn.recv(header.payload_len - len(resp_payload))
            if not chunk:
                raise MicroVMError(
                    "Socket closed while awaiting AstSlice response payload"
                )
            resp_payload += chunk

        latency = time.perf_counter() - t_start
        resp = decode_ast_slice_response(resp_payload)
        return resp, latency

    def patch_symbol(
        self,
        path: str,
        symbol: str,
        replacement: str,
        request_id: int = 202,
    ) -> tuple[AstPatchResponse, float]:
        """Applies a structural replacement to a symbol in a file over virtio-vsock."""
        if not self.vsock_conn:
            self.connect_vsock()

        assert self.vsock_conn is not None

        req = AstPatchRequest(path=path, symbol=symbol, replacement=replacement)
        payload = encode_ast_patch_request(req)
        frame = encode_frame(Opcode.AST_PATCH, request_id, payload)

        t_start = time.perf_counter()
        self.vsock_conn.sendall(frame)

        resp_header_bytes = b""
        while len(resp_header_bytes) < HEADER_SIZE:
            chunk = self.vsock_conn.recv(HEADER_SIZE - len(resp_header_bytes))
            if not chunk:
                raise MicroVMError(
                    "Socket closed while awaiting AstPatch response header"
                )
            resp_header_bytes += chunk

        header = decode_header(resp_header_bytes)
        if header.opcode != Opcode.AST_PATCH_RESPONSE:
            raise MicroVMError(
                f"Unexpected opcode in response: {header.opcode:04X}, expected {Opcode.AST_PATCH_RESPONSE:04X}"
            )

        resp_payload = b""
        while len(resp_payload) < header.payload_len:
            chunk = self.vsock_conn.recv(header.payload_len - len(resp_payload))
            if not chunk:
                raise MicroVMError(
                    "Socket closed while awaiting AstPatch response payload"
                )
            resp_payload += chunk

        latency = time.perf_counter() - t_start
        resp = decode_ast_patch_response(resp_payload)
        return resp, latency

    def cdp_action(
        self,
        req: CdpActionRequest,
        request_id: int = 300,
    ) -> tuple[CdpActionResponse, float]:
        """Dispatches a browser CDP action request over virtio-vsock."""
        if not self.vsock_conn:
            self.connect_vsock()

        assert self.vsock_conn is not None

        payload = encode_cdp_request(req)
        frame = encode_frame(Opcode.CDP_ACTION, request_id, payload)

        t_start = time.perf_counter()
        self.vsock_conn.sendall(frame)

        resp_header_bytes = b""
        while len(resp_header_bytes) < HEADER_SIZE:
            chunk = self.vsock_conn.recv(HEADER_SIZE - len(resp_header_bytes))
            if not chunk:
                raise MicroVMError("Socket closed while awaiting CDP response header")
            resp_header_bytes += chunk

        header = decode_header(resp_header_bytes)
        if header.opcode != Opcode.CDP_ACTION_RESPONSE:
            raise MicroVMError(
                f"Unexpected opcode in response: {header.opcode:04X}, expected {Opcode.CDP_ACTION_RESPONSE:04X}"
            )

        resp_payload = b""
        while len(resp_payload) < header.payload_len:
            chunk = self.vsock_conn.recv(header.payload_len - len(resp_payload))
            if not chunk:
                raise MicroVMError("Socket closed while awaiting CDP response payload")
            resp_payload += chunk

        latency = time.perf_counter() - t_start
        resp = decode_cdp_response(resp_payload)
        return resp, latency

    def navigate(
        self,
        url: str,
        request_id: int = 301,
    ) -> tuple[CdpActionResponse, float]:
        """Navigates headless Chromium to target URL and returns semantic AXTree."""
        req = CdpActionRequest(action=int(CdpActionType.NAVIGATE), payload1=url)
        return self.cdp_action(req, request_id=request_id)

    def get_axtree(
        self,
        request_id: int = 302,
    ) -> tuple[CdpActionResponse, float]:
        """Fetches the current filtered semantic AXTree from headless Chromium."""
        req = CdpActionRequest(action=int(CdpActionType.GET_TREE))
        return self.cdp_action(req, request_id=request_id)

    def click(
        self,
        target_id: int,
        request_id: int = 303,
    ) -> tuple[CdpActionResponse, float]:
        """Triggers a click action on the specified target handle."""
        req = CdpActionRequest(action=int(CdpActionType.CLICK), target_id=target_id)
        return self.cdp_action(req, request_id=request_id)

    def type_text(
        self,
        target_id: int,
        text: str,
        request_id: int = 304,
    ) -> tuple[CdpActionResponse, float]:
        """Types text into the specified target element handle."""
        req = CdpActionRequest(
            action=int(CdpActionType.TYPE),
            target_id=target_id,
            payload1=text,
        )
        return self.cdp_action(req, request_id=request_id)

    def scroll(
        self,
        delta_y: int,
        request_id: int = 305,
    ) -> tuple[CdpActionResponse, float]:
        """Scrolls the current viewport by delta Y pixels."""
        req = CdpActionRequest(
            action=int(CdpActionType.SCROLL),
            scroll_delta=delta_y,
        )
        return self.cdp_action(req, request_id=request_id)

    def pause(self) -> float:
        """Suspends the microVM execution via Firecracker PATCH /vm."""
        t_start = time.perf_counter()
        self._api_patch("/vm", {"state": "Paused"})
        return time.perf_counter() - t_start

    def resume(self) -> float:
        """Resumes microVM execution and reconnects the vsock stream."""
        t_start = time.perf_counter()
        self._api_patch("/vm", {"state": "Resumed"})
        if self.vsock_conn:
            try:
                self.vsock_conn.close()
            except OSError:
                pass
            self.vsock_conn = None
        self.connect_vsock()
        return time.perf_counter() - t_start

    def create_snapshot(
        self,
        name: str = "checkpoint",
        snapshot_dir: Path | str | None = None,
    ) -> tuple[SnapshotInfo, float]:
        """Captures a complete execution snapshot (state + memory)."""
        snap_dir = (
            Path(snapshot_dir) if snapshot_dir else (self.work_dir / "snapshots" / name)
        )
        snap_dir.mkdir(parents=True, exist_ok=True)

        snap_path = snap_dir / f"{name}.snap"
        mem_path = snap_dir / f"{name}.mem"
        snap_path.unlink(missing_ok=True)
        mem_path.unlink(missing_ok=True)

        t_start = time.perf_counter()
        self.pause()
        try:
            self._api_put(
                "/snapshot/create",
                {
                    "snapshot_type": "Full",
                    "snapshot_path": str(snap_path),
                    "mem_file_path": str(mem_path),
                },
            )
        finally:
            self.resume()

        duration = time.perf_counter() - t_start
        info = SnapshotInfo(
            name=name,
            snap_path=snap_path,
            mem_path=mem_path,
            rootfs_path=self.rootfs_path,
            kernel_path=self.kernel_path,
            metadata={"vsock_path": str(self.vsock_path)},
        )
        return info, duration

    @classmethod
    def load_snapshot(
        cls,
        snapshot: SnapshotInfo,
        work_dir: Path | str,
        firecracker_bin: str | Path = "/usr/local/bin/firecracker",
        guest_cid: int = 3,
        guest_port: int = 5200,
        vcpu_count: int = 1,
        mem_size_mib: int = 128,
        isolated_rootfs: bool = True,
    ) -> tuple[MicroVM, float]:
        """Restores a microVM from a snapshot into a new isolated instance in < 25ms."""
        w_dir = Path(work_dir).resolve()
        w_dir.mkdir(parents=True, exist_ok=True)

        branch_mem = w_dir / f"{snapshot.name}_mem.bin"
        copy_or_reflink(snapshot.mem_path, branch_mem)

        if isolated_rootfs:
            branch_rootfs = w_dir / "rootfs.ext4"
            copy_or_reflink(snapshot.rootfs_path, branch_rootfs)
        else:
            branch_rootfs = snapshot.rootfs_path

        vm = cls(
            kernel_path=snapshot.kernel_path,
            rootfs_path=branch_rootfs,
            firecracker_bin=firecracker_bin,
            vcpu_count=vcpu_count,
            mem_size_mib=mem_size_mib,
            guest_cid=guest_cid,
            guest_port=guest_port,
            work_dir=w_dir,
        )
        vm._cleanup_sockets()

        with snapshot.snap_path.open("rb") as f:
            snap_data = f.read()

        replacements = [
            (str(snapshot.rootfs_path), str(branch_rootfs)),
        ]
        old_vsock = snapshot.metadata.get("vsock_path")
        if old_vsock:
            replacements.append((old_vsock, str(vm.vsock_path)))
        else:
            for token in snap_data.split(b"\x00"):
                if b"vsock.sock" in token:
                    old_path = token.decode("utf-8", errors="ignore")
                    replacements.append((old_path, str(vm.vsock_path)))
                    break

        branch_snap = w_dir / f"{snapshot.name}_branch.snap"
        updated_data = update_snapshot_state(snap_data, replacements)
        branch_snap.write_bytes(updated_data)

        vm.log_file = vm.log_path.open("wb")
        clean_id = f"axiom-{snapshot.name.replace('_', '-')}-{int(time.time() * 1000) % 100000}"
        vm.process = subprocess.Popen(
            [
                vm.firecracker_bin,
                "--api-sock",
                str(vm.api_sock),
                "--id",
                clean_id,
            ],
            stdout=vm.log_file,
            stderr=subprocess.STDOUT,
        )

        vm._wait_for_api_socket()

        t_load_start = time.perf_counter()
        vm._api_put(
            "/snapshot/load",
            {
                "snapshot_path": str(branch_snap),
                "mem_backend": {
                    "backend_path": str(branch_mem),
                    "backend_type": "File",
                },
                "resume_vm": True,
            },
        )
        load_latency = time.perf_counter() - t_load_start

        vm.connect_vsock()
        return vm, load_latency

    def restore_snapshot(self, snapshot: SnapshotInfo) -> float:
        """Restores this microVM in-place from a saved execution snapshot."""
        self.shutdown()
        restored_vm, latency = self.load_snapshot(
            snapshot=snapshot,
            work_dir=self.work_dir,
            firecracker_bin=self.firecracker_bin,
            guest_cid=self.guest_cid,
            guest_port=self.guest_port,
            vcpu_count=self.vcpu_count,
            mem_size_mib=self.mem_size_mib,
            isolated_rootfs=False,
        )
        self.process = restored_vm.process
        self.vsock_conn = restored_vm.vsock_conn
        self.log_file = restored_vm.log_file
        return latency

    def branch(
        self,
        name: str = "branch",
        work_dir: Path | str | None = None,
        snapshot_name: str = "base",
    ) -> tuple[MicroVM, float]:
        """Creates an independent copy-on-write execution branch of this microVM."""
        snap_info, _ = self.create_snapshot(name=snapshot_name)
        target_work_dir = (
            Path(work_dir)
            if work_dir
            else (self.work_dir.parent / f"{self.work_dir.name}_{name}")
        )
        return self.load_snapshot(
            snapshot=snap_info,
            work_dir=target_work_dir,
            firecracker_bin=self.firecracker_bin,
            guest_cid=self.guest_cid + 1,
            guest_port=self.guest_port,
            vcpu_count=self.vcpu_count,
            mem_size_mib=self.mem_size_mib,
        )

    def shutdown(self) -> None:
        """Sends Shutdown frame to guest and gracefully shuts down Firecracker."""
        if self.vsock_conn:
            try:
                frame = encode_frame(Opcode.SHUTDOWN, 999)
                self.vsock_conn.sendall(frame)
                self.vsock_conn.close()
            except OSError:
                pass
            self.vsock_conn = None

        if self.process:
            try:
                self.process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                try:
                    self.process.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
            self.process = None

        if self.log_file:
            self.log_file.close()
            self.log_file = None

        self._cleanup_sockets()

    stop = shutdown

    def is_alive(self) -> bool:
        """Returns True if the underlying Firecracker process is currently running."""
        return self.process is not None and self.process.poll() is None

    def __enter__(self) -> Self:
        """Context manager entry point."""
        self.start()
        return self

    def __exit__(self, exc_type: object, exc_val: object, exc_tb: object) -> None:
        """Context manager exit point."""
        self.shutdown()

    def _cleanup_sockets(self) -> None:
        """Removes leftover Unix domain sockets."""
        for path in (self.api_sock, self.vsock_path):
            if path.exists():
                try:
                    os.unlink(path)
                except OSError:
                    pass

    def _wait_for_api_socket(self, timeout_sec: float = 2.0) -> None:
        """Waits until the Firecracker API Unix domain socket becomes available."""
        deadline = time.time() + timeout_sec
        while time.time() < deadline:
            if self.api_sock.exists():
                return
            time.sleep(0.01)
        raise MicroVMError("Firecracker API socket did not appear within timeout")

    def _api_put(self, path: str, body: dict[str, Any]) -> None:
        """Sends an HTTP PUT request to Firecracker API socket."""
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(2.0)
            sock.connect(str(self.api_sock))

            data = json.dumps(body)
            request = (
                f"PUT {path} HTTP/1.1\r\n"
                f"Host: localhost\r\n"
                f"Content-Type: application/json\r\n"
                f"Content-Length: {len(data)}\r\n\r\n"
                f"{data}"
            )
            sock.sendall(request.encode("utf-8"))

            response = sock.recv(4096).decode("utf-8", errors="replace")
            status_line = response.splitlines()[0] if response else ""
            tokens = status_line.split()
            status_code = (
                int(tokens[1]) if len(tokens) > 1 and tokens[1].isdigit() else 0
            )
            if status_code not in (200, 204):
                raise MicroVMError(
                    f"Firecracker API error for PUT {path}: {status_line}\nResponse: {response}"
                )

    def _api_patch(self, path: str, body: dict[str, Any]) -> None:
        """Sends an HTTP PATCH request to Firecracker API socket."""
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(2.0)
            sock.connect(str(self.api_sock))

            data = json.dumps(body)
            request = (
                f"PATCH {path} HTTP/1.1\r\n"
                f"Host: localhost\r\n"
                f"Content-Type: application/json\r\n"
                f"Content-Length: {len(data)}\r\n\r\n"
                f"{data}"
            )
            sock.sendall(request.encode("utf-8"))

            response = sock.recv(4096).decode("utf-8", errors="replace")
            status_line = response.splitlines()[0] if response else ""
            tokens = status_line.split()
            status_code = (
                int(tokens[1]) if len(tokens) > 1 and tokens[1].isdigit() else 0
            )
            if status_code not in (200, 204):
                raise MicroVMError(
                    f"Firecracker API error for PATCH {path}: {status_line}\nResponse: {response}"
                )
