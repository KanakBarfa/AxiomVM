"""End-to-end test verifying virtio-vsock AST RPCs inside Firecracker MicroVM."""

import os
import sys
from pathlib import Path

import pytest

# Ensure host package is importable
REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "host"))

from axiom.vm import MicroVM


@pytest.fixture(scope="module")
def artifacts() -> tuple[Path, Path]:
    """Ensures kernel and rootfs images are present before running microVM tests."""
    kernel_path = REPO_ROOT / "tools" / "kernel" / "vmlinux"
    rootfs_path = REPO_ROOT / "build" / "axiom-rootfs.ext4"

    if not kernel_path.exists():
        pytest.skip(f"MicroVM kernel binary not found at {kernel_path}")
    if not rootfs_path.exists():
        pytest.skip(f"MicroVM rootfs image not found at {rootfs_path}")
    if not os.access("/dev/kvm", os.R_OK | os.W_OK):
        pytest.skip("Read/write access to /dev/kvm is required to run Firecracker")

    return kernel_path, rootfs_path


def test_ast_engine_e2e(artifacts: tuple[Path, Path]) -> None:
    kernel_path, rootfs_path = artifacts

    vm = MicroVM(
        kernel_path=kernel_path,
        rootfs_path=rootfs_path,
        guest_cid=3,
        guest_port=5200,
        vcpu_count=1,
        mem_size_mib=128,
    )

    with vm:
        header, latency = vm.ping(request_id=1)
        assert header.request_id == 1
        assert latency < 0.05

        # 1. Stage Rust source file inside appliance
        rust_code = (
            "pub struct Calculator {\n"
            "    offset: i32,\n"
            "}\n\n"
            "impl Calculator {\n"
            "    pub fn new(offset: i32) -> Self {\n"
            "        Calculator { offset }\n"
            "    }\n\n"
            "    pub fn compute(&self, val: i32) -> i32 {\n"
            "        val + self.offset\n"
            "    }\n"
            "}\n\n"
            "fn helper() -> bool {\n"
            "    true\n"
            "}\n"
        )
        cmd_stage_rust = f"cat << 'EOF' > /tmp/calc.rs\n{rust_code}EOF\n"
        resp, _ = vm.exec(cmd_stage_rust)
        assert resp.exit_code == 0

        # 2. Stage C++ source file inside appliance
        cpp_code = (
            "#include <string>\n\n"
            "class NetworkService {\n"
            "public:\n"
            "    bool connect(const std::string& addr) {\n"
            "        return true;\n"
            "    }\n"
            "    void disconnect();\n"
            "};\n\n"
            "int run_service() {\n"
            "    return 0;\n"
            "}\n"
        )
        cmd_stage_cpp = f"cat << 'EOF' > /tmp/net.cpp\n{cpp_code}EOF\n"
        resp, _ = vm.exec(cmd_stage_cpp)
        assert resp.exit_code == 0

        # 3. Test AstSymbols query over vsock
        sym_resp, sym_lat = vm.get_symbols("/tmp/calc.rs")
        assert sym_resp.status == 0
        assert sym_resp.symbol_count >= 4
        assert "struct Calculator" in sym_resp.symbols_text
        assert "method Calculator::new" in sym_resp.symbols_text
        assert "method Calculator::compute" in sym_resp.symbols_text
        assert "function helper" in sym_resp.symbols_text
        assert sym_lat < 0.05

        # 4. Test AstSlice query over vsock
        slice_resp, slice_lat = vm.get_slice("/tmp/calc.rs", "Calculator::compute")
        assert slice_resp.status == 0
        assert slice_resp.start_line == 10
        assert slice_resp.end_line == 12
        assert "val + self.offset" in slice_resp.content
        assert slice_lat < 0.05

        # 5. Test AstPatch query over vsock
        replacement = (
            "    pub fn compute(&self, val: i32) -> i32 {\n"
            "        val + self.offset + 1000\n"
            "    }"
        )
        patch_resp, patch_lat = vm.patch_symbol(
            "/tmp/calc.rs", "Calculator::compute", replacement
        )
        assert patch_resp.status == 0
        assert patch_resp.old_start_line == 10
        assert patch_resp.old_end_line == 12
        assert patch_lat < 0.05

        # 6. Verify patched content is active in guest filesystem
        cat_resp, _ = vm.exec("cat /tmp/calc.rs")
        assert cat_resp.exit_code == 0
        assert "val + self.offset + 1000" in cat_resp.output

        # 7. Test C++ symbol extraction and slicing
        cpp_sym_resp, _ = vm.get_symbols("/tmp/net.cpp")
        assert cpp_sym_resp.status == 0
        assert "class NetworkService" in cpp_sym_resp.symbols_text
        assert "method NetworkService::connect" in cpp_sym_resp.symbols_text

        cpp_slice_resp, _ = vm.get_slice("/tmp/net.cpp", "NetworkService::connect")
        assert cpp_slice_resp.status == 0
        assert "return true;" in cpp_slice_resp.content

        # 8. Token reduction verification against raw cat/grep
        full_source_bytes = len(rust_code) + len(cpp_code)
        extracted_bytes = len(sym_resp.symbols_text) + len(slice_resp.content)
        savings = (full_source_bytes - extracted_bytes) / full_source_bytes
        assert savings > 0.40

        # 9. Error case validation
        err_sym_resp, _ = vm.get_symbols("/tmp/nonexistent.rs")
        assert err_sym_resp.status != 0

        err_slice_resp, _ = vm.get_slice("/tmp/calc.rs", "NonExistentSymbol")
        assert err_slice_resp.status != 0

        # 10. Test Python AST symbol extraction and slicing
        py_code = (
            "class TaskRunner:\n"
            "    def __init__(self, name: str):\n"
            "        self.name = name\n\n"
            "    def run_task(self, count: int) -> int:\n"
            "        return count * 2\n\n"
            "def top_level_fn() -> str:\n"
            "    return 'done'\n"
        )
        vm.write_file("/tmp/task.py", py_code)
        py_sym_resp, _ = vm.get_symbols("/tmp/task.py")
        assert py_sym_resp.status == 0
        assert "class TaskRunner" in py_sym_resp.symbols_text
        assert "method TaskRunner::run_task" in py_sym_resp.symbols_text
        assert "function top_level_fn" in py_sym_resp.symbols_text

        py_slice_resp, _ = vm.get_slice("/tmp/task.py", "TaskRunner.run_task")
        assert py_slice_resp.status == 0
        assert "return count * 2" in py_slice_resp.content

        # 11. Test TypeScript AST symbol extraction and slicing
        ts_code = (
            "interface User {\n"
            "    id: number;\n"
            "}\n\n"
            "class AuthService {\n"
            "    login(token: string): boolean {\n"
            "        return token.length > 0;\n"
            "    }\n"
            "}\n\n"
            "function computeHash(data: string): string {\n"
            "    return data;\n"
            "}\n"
        )
        vm.write_file("/tmp/service.ts", ts_code)
        ts_sym_resp, _ = vm.get_symbols("/tmp/service.ts")
        assert ts_sym_resp.status == 0
        assert "class AuthService" in ts_sym_resp.symbols_text
        assert "method AuthService::login" in ts_sym_resp.symbols_text
        assert "function computeHash" in ts_sym_resp.symbols_text

        ts_slice_resp, _ = vm.get_slice("/tmp/service.ts", "AuthService.login")
        assert ts_slice_resp.status == 0
        assert "return token.length > 0;" in ts_slice_resp.content
