"""Firecracker microVM controller and virtio-vsock client."""

from __future__ import annotations

import json
import os
import socket
import subprocess
import time
from pathlib import Path
from typing import Any, BinaryIO, Self

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
    decode_ast_patch_response,
    decode_ast_slice_response,
    decode_ast_symbols_response,
    decode_cdp_response,
    decode_exec_response,
    decode_header,
    encode_ast_patch_request,
    encode_ast_slice_request,
    encode_ast_symbols_request,
    encode_cdp_request,
    encode_exec_request,
    encode_frame,
)


class MicroVMError(RuntimeError):
    """Raised when microVM lifecycle operations fail."""


class MicroVM:
    """Controls a Firecracker microVM instance over Unix domain sockets."""

    def __init__(
        self,
        kernel_path: str | Path,
        rootfs_path: str | Path,
        firecracker_bin: str | Path = "/usr/local/bin/firecracker",
        vcpu_count: int = 1,
        mem_size_mib: int = 128,
        guest_cid: int = 3,
        guest_port: int = 5200,
        work_dir: str | Path = "/tmp/axiom_vm",
    ) -> None:
        """Initializes MicroVM configuration parameters."""
        self.kernel_path = Path(kernel_path).resolve()
        self.rootfs_path = Path(rootfs_path).resolve()
        self.firecracker_bin = str(firecracker_bin)
        self.vcpu_count = vcpu_count
        self.mem_size_mib = mem_size_mib
        self.guest_cid = guest_cid
        self.guest_port = guest_port
        self.work_dir = Path(work_dir)

        self.api_sock = self.work_dir / "firecracker.sock"
        self.vsock_path = self.work_dir / "vsock.sock"
        self.log_path = self.work_dir / "firecracker.log"

        self.process: subprocess.Popen[bytes] | None = None
        self.vsock_conn: socket.socket | None = None
        self.log_file: BinaryIO | None = None

    def start(self) -> None:
        """Boots the microVM instance and configures kernel, drives, and vsock."""
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
        request_id: int = 100,
    ) -> tuple[ExecResponse, float]:
        """Executes a command inside the appliance over vsock and returns response and latency."""
        if not self.vsock_conn:
            self.connect_vsock()

        assert self.vsock_conn is not None

        flags = 0
        if strip_ansi:
            flags |= EXEC_FLAG_STRIP_ANSI
        if capture_diff:
            flags |= EXEC_FLAG_CAPTURE_DIFF

        req = ExecRequest(
            command=command,
            cwd=cwd,
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
        return exec_resp, latency

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
