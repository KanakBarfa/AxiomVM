"""Model Context Protocol (MCP) tool server for AxiomVM appliance."""

from __future__ import annotations

import os
from pathlib import Path

from mcp.server.mcpserver import MCPServer

from axiom.assets import resolve_assets
from axiom.snapshot import SnapshotInfo
from axiom.vm import MicroVM
from axiom.wire import CdpActionRequest, CdpActionType

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_KERNEL = REPO_ROOT / "tools" / "kernel" / "vmlinux"
DEFAULT_ROOTFS = REPO_ROOT / "build" / "axiom-rootfs.ext4"

server = MCPServer(name="AxiomVM")

_active_vm: MicroVM | None = None
_branches: dict[str, MicroVM] = {}
_snapshots: dict[str, SnapshotInfo] = {}


def get_or_create_vm() -> MicroVM:
    """Retrieves active MicroVM instance or boots a new appliance on demand."""
    global _active_vm
    if _active_vm is not None and _active_vm.process is not None:
        return _active_vm

    kernel_path, rootfs_path = resolve_assets(auto_download=True)
    if kernel_path is None:
        kernel_path = Path(os.environ.get("AXIOM_KERNEL_PATH", str(DEFAULT_KERNEL)))
    if rootfs_path is None:
        rootfs_path = Path(os.environ.get("AXIOM_ROOTFS_PATH", str(DEFAULT_ROOTFS)))
    work_dir = Path(os.environ.get("AXIOM_WORK_DIR", "/tmp/axiom_mcp_primary"))

    _active_vm = MicroVM(
        kernel_path=kernel_path,
        rootfs_path=rootfs_path,
        guest_cid=3,
        guest_port=5200,
        work_dir=work_dir,
    )
    _active_vm.start()
    return _active_vm


@server.tool()
def axiom_exec(
    command: str,
    working_dir: str = "/tmp",
    strip_ansi: bool = True,
    capture_diff: bool = True,
) -> str:
    """Executes a command inside the microVM with ANSI stripping and file diffs."""
    vm = get_or_create_vm()
    full_cmd = f"cd {working_dir} && {command}" if working_dir != "/tmp" else command
    resp, latency = vm.exec(
        full_cmd,
        strip_ansi=strip_ansi,
        capture_diff=capture_diff,
    )

    diff_section = f"\n[File Diffs]\n{resp.diff}" if resp.diff else ""
    return (
        f"[Exit Code: {resp.exit_code}] [Latency: {latency * 1000:.2f}ms]\n"
        f"{resp.output}{diff_section}"
    )


@server.tool()
def axiom_ast_symbols(file_path: str) -> str:
    """Extracts token-dense function, struct, and symbol outlines using Tree-sitter."""
    vm = get_or_create_vm()
    resp, latency = vm.get_symbols(file_path)
    return (
        f"Found {resp.symbol_count} symbols in {file_path} ({latency * 1000:.2f}ms):\n"
        f"{resp.symbols_text}"
    )


@server.tool()
def axiom_ast_slice(file_path: str, symbol_name: str) -> str:
    """Retrieves target symbol implementation directly from guest AST engine."""
    vm = get_or_create_vm()
    resp, latency = vm.get_slice(file_path, symbol_name)
    return (
        f"Symbol '{symbol_name}' [L{resp.start_line}-L{resp.end_line}] "
        f"({latency * 1000:.2f}ms):\n{resp.content}"
    )


@server.tool()
def axiom_ast_patch(
    file_path: str,
    symbol_name: str,
    replacement_code: str,
) -> str:
    """Atomically patches target symbol implementation on guest filesystem."""
    vm = get_or_create_vm()
    resp, latency = vm.patch_symbol(file_path, symbol_name, replacement_code)
    return (
        f"Patched '{symbol_name}' in {file_path} ({latency * 1000:.2f}ms): "
        f"old lines [L{resp.old_start_line}-L{resp.old_end_line}], "
        f"new lines [L{resp.old_start_line}-L{resp.new_end_line}]."
    )


@server.tool()
def axiom_read_file(
    file_path: str,
    offset: int = 0,
    max_bytes: int = 65536,
) -> str:
    """Reads file content directly from microVM guest filesystem over virtio-vsock."""
    vm = get_or_create_vm()
    resp, latency = vm.read_file(file_path, offset=offset, max_bytes=max_bytes)
    if resp.status != 0:
        return (
            f"Error reading {file_path} (status {resp.status}, {latency * 1000:.2f}ms)"
        )
    try:
        content_text = resp.content.decode("utf-8")
    except UnicodeDecodeError:
        content_text = resp.content.hex()
    return (
        f"File {file_path} [{len(resp.content)}/{resp.total_size} B] "
        f"({latency * 1000:.2f}ms):\n{content_text}"
    )


@server.tool()
def axiom_write_file(
    file_path: str,
    content: str,
    append: bool = False,
) -> str:
    """Writes text content to a file on microVM guest filesystem over virtio-vsock."""
    vm = get_or_create_vm()
    resp, latency = vm.write_file(file_path, content, append=append)
    if resp.status != 0:
        return (
            f"Error writing to {file_path} (status {resp.status}, {latency * 1000:.2f}ms)"
        )
    return (
        f"Successfully wrote {resp.bytes_written} bytes to {file_path} "
        f"({latency * 1000:.2f}ms)"
    )


@server.tool()
def axiom_browser_navigate(url: str) -> str:
    """Navigates headless Chromium and returns token-dense semantic accessibility tree."""
    vm = get_or_create_vm()
    resp, latency = vm.navigate(url)
    return (
        f"Navigated to {url} [Nodes: {resp.node_count}] ({latency * 1000:.2f}ms):\n"
        f"{resp.tree_text}"
    )


@server.tool()
def axiom_browser_action(
    action: str,
    target_id: int = 0,
    text: str = "",
    scroll_delta: int = 0,
) -> str:
    """Executes browser interactions (click, type, scroll) using semantic element IDs."""
    vm = get_or_create_vm()
    act_lower = action.lower().strip()
    if act_lower == "click":
        resp, latency = vm.click(target_id)
    elif act_lower == "type":
        resp, latency = vm.type_text(target_id, text)
    elif act_lower == "scroll":
        resp, latency = vm.scroll(scroll_delta)
    elif act_lower in ("tree", "get_tree"):
        resp, latency = vm.get_axtree()
    else:
        req = CdpActionRequest(
            action=int(CdpActionType.GET_TREE),
            target_id=target_id,
            scroll_delta=scroll_delta,
            payload1=text,
        )
        resp, latency = vm.cdp_action(req)

    return (
        f"Action '{action}' executed ({latency * 1000:.2f}ms) [Status: {resp.status}]:\n"
        f"{resp.tree_text}"
    )


@server.tool()
def axiom_snapshot(name: str = "checkpoint") -> str:
    """Creates a sub-100ms copy-on-write memory snapshot checkpoint."""
    vm = get_or_create_vm()
    snap_info, duration = vm.create_snapshot(name=name)
    _snapshots[name] = snap_info
    return (
        f"Snapshot '{name}' created in {duration * 1000:.2f}ms.\n"
        f"State: {snap_info.snap_path} ({snap_info.snap_path.stat().st_size} bytes)\n"
        f"Memory: {snap_info.mem_path} ({snap_info.mem_path.stat().st_size} bytes)"
    )


@server.tool()
def axiom_branch(name: str = "branch", snapshot_name: str = "checkpoint") -> str:
    """Spawns an isolated microVM execution branch from snapshot in < 25ms."""
    vm = get_or_create_vm()
    branch_vm, latency = vm.branch(name=name, snapshot_name=snapshot_name)
    _branches[name] = branch_vm
    return (
        f"Branch '{name}' spawned from '{snapshot_name}' in {latency * 1000:.2f}ms.\n"
        f"Work dir: {branch_vm.work_dir}\n"
        f"Vsock CID: {branch_vm.guest_cid}, Port: {branch_vm.guest_port}"
    )


def main() -> None:
    """Runs the AxiomVM MCP server over standard I/O."""
    server.run(transport="stdio")


if __name__ == "__main__":
    main()
