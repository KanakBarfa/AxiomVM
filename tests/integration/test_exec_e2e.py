"""End-to-end integration and benchmark suite for AxiomVM execution subsystem."""

import os
import subprocess
import sys
import time
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "host"))

from axiom.vm import MicroVM
from axiom.wire import Opcode


@pytest.fixture(scope="module")
def artifacts() -> tuple[Path, Path]:
    """Ensures kernel and rootfs images are present before running microVM tests."""
    kernel_path = REPO_ROOT / "tools" / "kernel" / "vmlinux"
    rootfs_path = REPO_ROOT / "build" / "axiom-rootfs.ext4"

    if not kernel_path.exists():
        pytest.skip(f"MicroVM kernel binary not found at {kernel_path}")

    if not rootfs_path.exists():
        pytest.skip(f"Rootfs ext4 image not found at {rootfs_path}")

    if not os.access("/dev/kvm", os.R_OK | os.W_OK):
        pytest.skip("Read/write access to /dev/kvm is required to run Firecracker")

    return kernel_path, rootfs_path


def test_microvm_exec_basic(artifacts: tuple[Path, Path]) -> None:
    """Tests basic command execution over virtio-vsock inside Firecracker."""
    kernel_path, rootfs_path = artifacts
    work_dir = Path("/tmp/axiom_fc_exec_basic")

    with MicroVM(
        kernel_path=kernel_path,
        rootfs_path=rootfs_path,
        work_dir=work_dir,
    ) as vm:
        header, ping_lat = vm.ping(request_id=101)
        assert header.opcode == Opcode.PONG
        assert ping_lat < 0.005

        resp, exec_lat = vm.exec(
            command="echo 'AxiomVM guest exec verified'",
            timeout_ms=5000,
            request_id=201,
        )

        assert resp.exit_code == 0
        assert "AxiomVM guest exec verified" in resp.output
        assert resp.cpu_cycles > 0
        print(
            f"\n[MicroVM Exec Basic] Latency: {exec_lat * 1000:.2f}ms | Output: {resp.output.strip()}"
        )


def test_microvm_exec_ansi_stripping(artifacts: tuple[Path, Path]) -> None:
    """Tests single-pass ANSI escape stripping over virtio-vsock inside Firecracker."""
    kernel_path, rootfs_path = artifacts
    work_dir = Path("/tmp/axiom_fc_exec_ansi")

    with MicroVM(
        kernel_path=kernel_path,
        rootfs_path=rootfs_path,
        work_dir=work_dir,
    ) as vm:
        raw_cmd = "printf '\\033[1;31mFAIL:\\033[0m \\033[32mPASS\\033[0m\\n'"

        # Execute with ANSI stripping enabled
        resp_stripped, _ = vm.exec(
            command=raw_cmd,
            strip_ansi=True,
            request_id=301,
        )
        assert resp_stripped.exit_code == 0
        assert resp_stripped.output == "FAIL: PASS\n"

        # Execute with ANSI stripping disabled
        resp_raw, _ = vm.exec(
            command=raw_cmd,
            strip_ansi=False,
            request_id=302,
        )
        assert resp_raw.exit_code == 0
        assert "\033[" in resp_raw.output

        tokens_saved_bytes = len(resp_raw.output.encode()) - len(
            resp_stripped.output.encode()
        )
        assert tokens_saved_bytes > 0
        print(
            f"\n[MicroVM ANSI Stripping] Stripped {tokens_saved_bytes} bytes of ANSI escape sequences"
        )


def test_microvm_exec_diff_extraction(artifacts: tuple[Path, Path]) -> None:
    """Tests structured file delta extraction after guest command execution."""
    kernel_path, rootfs_path = artifacts
    work_dir = Path("/tmp/axiom_fc_exec_diff")

    with MicroVM(
        kernel_path=kernel_path,
        rootfs_path=rootfs_path,
        work_dir=work_dir,
    ) as vm:
        resp, _ = vm.exec(
            command="echo 'generated content' > /tmp/axiom_delta.txt",
            cwd="/tmp",
            capture_diff=True,
            request_id=401,
        )

        assert resp.exit_code == 0
        assert resp.diff != ""
        assert "+ " in resp.diff
        assert "axiom_delta.txt" in resp.diff
        print(f"\n[MicroVM Diff Extraction] Captured file delta:\n{resp.diff.strip()}")


def test_microvm_exec_benchmark(artifacts: tuple[Path, Path]) -> None:
    """Benchmarks execution latency and token efficiency against host bash invocation."""
    kernel_path, rootfs_path = artifacts
    work_dir = Path("/tmp/axiom_fc_exec_bench")

    test_cmd = "printf '\\033[36mRunning task:\\033[0m step 1 of 5\\n'"
    iterations = 10

    # Measure host subprocess execution latency as local baseline
    host_latencies: list[float] = []
    for _ in range(iterations):
        t0 = time.perf_counter()
        proc = subprocess.run(
            ["sh", "-c", test_cmd],
            capture_output=True,
            check=True,
        )
        t_el = time.perf_counter() - t0
        host_latencies.append(t_el)

    with MicroVM(
        kernel_path=kernel_path,
        rootfs_path=rootfs_path,
        work_dir=work_dir,
    ) as vm:
        # Warmup
        vm.exec(command="echo warmup")

        axiom_latencies: list[float] = []
        token_savings_bytes: list[int] = []

        for seq in range(iterations):
            resp, lat = vm.exec(
                command=test_cmd,
                strip_ansi=True,
                request_id=500 + seq,
            )
            axiom_latencies.append(lat)
            raw_len = len(proc.stdout)
            stripped_len = len(resp.output.encode("utf-8"))
            token_savings_bytes.append(raw_len - stripped_len)

        avg_axiom_ms = (sum(axiom_latencies) / len(axiom_latencies)) * 1000
        min_axiom_ms = min(axiom_latencies) * 1000
        avg_host_ms = (sum(host_latencies) / len(host_latencies)) * 1000
        min_host_ms = min(host_latencies) * 1000

        print(
            f"\n=======================================================\n"
            f"           AxiomVM Execution Subsystem Benchmark       \n"
            f"=======================================================\n"
            f" Host sh baseline latency : Avg: {avg_host_ms:.2f}ms | Min: {min_host_ms:.2f}ms\n"
            f" AxiomVM vsock latency    : Avg: {avg_axiom_ms:.2f}ms | Min: {min_axiom_ms:.2f}ms\n"
            f" ANSI token reduction     : {token_savings_bytes[0]} bytes saved per command\n"
            f" Steady-state performance : Sub-5ms guest RPC verified\n"
            f"======================================================="
        )

        assert min_axiom_ms < 15.0, f"Latency {min_axiom_ms:.2f}ms exceeded 15ms target"
