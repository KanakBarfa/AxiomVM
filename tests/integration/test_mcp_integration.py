"""Integration tests for AxiomVM Model Context Protocol (MCP) server."""

from __future__ import annotations

import asyncio
import os
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

pytest.importorskip("mcp", reason="mcp library not installed")

from mcp.types import CallToolResult, TextContent

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "host"))

from axiom.mcp_server import server
from axiom.snapshot import SnapshotInfo
from axiom.wire import (
    AstPatchResponse,
    AstSliceResponse,
    AstSymbolsResponse,
    CdpActionResponse,
    ExecResponse,
    ReadFileResponse,
    WriteFileResponse,
)


def extract_text(res: object) -> str:
    """Extracts first text segment from a successful CallToolResult."""
    assert isinstance(res, CallToolResult)
    assert not res.is_error
    assert len(res.content) > 0
    item = res.content[0]
    assert isinstance(item, TextContent)
    return item.text


def test_mcp_tool_registration() -> None:
    """Verifies all expected tools are properly exposed on the MCP server."""

    async def run() -> None:
        tools = await server.list_tools()
        tool_names = {tool.name for tool in tools}

        expected_tools = {
            "axiom_exec",
            "axiom_ast_symbols",
            "axiom_ast_slice",
            "axiom_ast_patch",
            "axiom_read_file",
            "axiom_write_file",
            "axiom_browser_navigate",
            "axiom_browser_action",
            "axiom_snapshot",
            "axiom_branch",
        }

        assert expected_tools.issubset(tool_names)

    asyncio.run(run())


def test_mcp_exec_tool_invocation() -> None:
    """Tests execution tool dispatch through MCP server."""

    async def run() -> None:
        with patch("axiom.mcp_server.get_or_create_vm") as mock_get_vm:
            mock_vm = MagicMock()
            mock_vm.exec.return_value = (
                ExecResponse(
                    exit_code=0,
                    flags=0,
                    cpu_cycles=45000,
                    output="hello from microvm\n",
                    diff="",
                ),
                0.0012,
            )
            mock_get_vm.return_value = mock_vm

            res = await server.call_tool(
                "axiom_exec", {"command": "echo 'hello from microvm'"}
            )
            text = extract_text(res)
            assert "[Exit Code: 0]" in text
            assert "hello from microvm" in text

    asyncio.run(run())


def test_mcp_ast_tools_invocation() -> None:
    """Tests AST analysis and patching tools through MCP server."""

    async def run() -> None:
        with patch("axiom.mcp_server.get_or_create_vm") as mock_get_vm:
            mock_vm = MagicMock()
            mock_vm.get_symbols.return_value = (
                AstSymbolsResponse(
                    status=0,
                    symbol_count=2,
                    symbols_text="[function] add [L1-L3]\n[struct] Point [L5-L8]",
                ),
                0.0008,
            )
            mock_vm.get_slice.return_value = (
                AstSliceResponse(
                    status=0,
                    start_line=1,
                    end_line=3,
                    content="int add(int a, int b) { return a + b; }",
                ),
                0.0006,
            )
            mock_vm.patch_symbol.return_value = (
                AstPatchResponse(
                    status=0,
                    old_start_line=1,
                    old_end_line=3,
                    new_end_line=4,
                ),
                0.0015,
            )
            mock_get_vm.return_value = mock_vm

            sym_res = await server.call_tool(
                "axiom_ast_symbols", {"file_path": "main.cpp"}
            )
            assert "Found 2 symbols" in extract_text(sym_res)

            slice_res = await server.call_tool(
                "axiom_ast_slice", {"file_path": "main.cpp", "symbol_name": "add"}
            )
            assert "Symbol 'add'" in extract_text(slice_res)

            patch_res = await server.call_tool(
                "axiom_ast_patch",
                {
                    "file_path": "main.cpp",
                    "symbol_name": "add",
                    "replacement_code": "int add(int a, int b) { return a + b + 1; }",
                },
            )
            assert "Patched 'add'" in extract_text(patch_res)

    asyncio.run(run())


def test_mcp_file_tools_invocation() -> None:
    """Tests file read and write tools through MCP server."""

    async def run() -> None:
        with patch("axiom.mcp_server.get_or_create_vm") as mock_get_vm:
            mock_vm = MagicMock()
            mock_vm.read_file.return_value = (
                ReadFileResponse(
                    status=0,
                    total_size=12,
                    content=b"hello world\n",
                ),
                0.0005,
            )
            mock_vm.write_file.return_value = (
                WriteFileResponse(
                    status=0,
                    bytes_written=12,
                ),
                0.0006,
            )
            mock_get_vm.return_value = mock_vm

            read_res = await server.call_tool(
                "axiom_read_file", {"file_path": "/tmp/test.txt"}
            )
            text = extract_text(read_res)
            assert "hello world" in text
            assert "/tmp/test.txt" in text

            write_res = await server.call_tool(
                "axiom_write_file",
                {"file_path": "/tmp/test.txt", "content": "hello world\n"},
            )
            write_text = extract_text(write_res)
            assert "Successfully wrote 12 bytes" in write_text

    asyncio.run(run())


def test_mcp_browser_tools_invocation() -> None:
    """Tests browser navigation and action tools through MCP server."""

    async def run() -> None:
        with patch("axiom.mcp_server.get_or_create_vm") as mock_get_vm:
            mock_vm = MagicMock()
            mock_vm.navigate.return_value = (
                CdpActionResponse(
                    status=0,
                    node_count=1,
                    tree_text='[1] RootWebArea "Dashboard"',
                ),
                0.012,
            )
            mock_vm.click.return_value = (
                CdpActionResponse(
                    status=0,
                    node_count=1,
                    tree_text='[1] RootWebArea "Dashboard - Clicked"',
                ),
                0.005,
            )
            mock_get_vm.return_value = mock_vm

            nav_res = await server.call_tool(
                "axiom_browser_navigate", {"url": "http://127.0.0.1:8080"}
            )
            assert "RootWebArea" in extract_text(nav_res)

            act_res = await server.call_tool(
                "axiom_browser_action", {"action": "click", "target_id": 1}
            )
            assert "Dashboard - Clicked" in extract_text(act_res)

    asyncio.run(run())


def test_mcp_snapshot_and_branch_tools(tmp_path: Path) -> None:
    """Tests snapshot and branch tools through MCP server."""

    async def run() -> None:
        snap_file = tmp_path / "snap"
        mem_file = tmp_path / "mem"
        snap_file.write_bytes(b"dummy snapshot")
        mem_file.write_bytes(b"dummy memory")

        dummy_snap_info = SnapshotInfo(
            name="test_snap",
            snap_path=snap_file,
            mem_path=mem_file,
            rootfs_path=tmp_path / "rootfs.ext4",
            kernel_path=tmp_path / "vmlinux",
        )

        with patch("axiom.mcp_server.get_or_create_vm") as mock_get_vm:
            mock_vm = MagicMock()
            mock_vm.create_snapshot.return_value = (dummy_snap_info, 0.045)

            branch_mock = MagicMock()
            branch_mock.work_dir = tmp_path / "branch_dir"
            branch_mock.guest_cid = 4
            branch_mock.guest_port = 5200
            mock_vm.branch.return_value = (branch_mock, 0.004)

            mock_get_vm.return_value = mock_vm

            snap_res = await server.call_tool("axiom_snapshot", {"name": "test_snap"})
            assert "Snapshot 'test_snap' created" in extract_text(snap_res)

            branch_res = await server.call_tool(
                "axiom_branch", {"name": "branch_1", "snapshot_name": "test_snap"}
            )
            assert "Branch 'branch_1' spawned" in extract_text(branch_res)

    asyncio.run(run())


def test_mcp_live_e2e_exec(tmp_path: Path) -> None:
    """Tests live microVM exec via MCP tools when virtualization is present."""
    kernel_path = REPO_ROOT / "tools" / "kernel" / "vmlinux"
    rootfs_path = REPO_ROOT / "build" / "axiom-rootfs.ext4"

    if not kernel_path.exists() or not rootfs_path.exists():
        pytest.skip("Kernel or rootfs artifacts missing for live E2E MCP test")
    if not os.access("/dev/kvm", os.R_OK | os.W_OK):
        pytest.skip("Read/write access to /dev/kvm required for live E2E MCP test")

    import axiom.mcp_server as mcp_mod
    from axiom.vm import MicroVM

    async def run() -> None:
        live_vm = MicroVM(
            kernel_path=kernel_path,
            rootfs_path=rootfs_path,
            guest_cid=3,
            guest_port=5200,
            work_dir=tmp_path / "live_mcp_vm",
        )
        live_vm.start()
        try:
            old_vm = mcp_mod._active_vm
            mcp_mod._active_vm = live_vm
            try:
                res = await server.call_tool(
                    "axiom_exec", {"command": "echo 'LIVE_MCP_OK'"}
                )
                assert "LIVE_MCP_OK" in extract_text(res)
            finally:
                mcp_mod._active_vm = old_vm
        finally:
            live_vm.shutdown()

    asyncio.run(run())
