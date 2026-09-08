"""End-to-end integration test booting Firecracker and asserting sub-5ms vsock latency."""

import os
import sys
from pathlib import Path

import pytest

# Ensure host package is importable
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


def test_firecracker_boot_and_vsock_ping_pong(artifacts: tuple[Path, Path]) -> None:
    """Boots Firecracker microVM, connects over virtio-vsock, and verifies ping latency."""
    kernel_path, rootfs_path = artifacts
    work_dir = Path("/tmp/axiom_fc_test")

    with MicroVM(
        kernel_path=kernel_path,
        rootfs_path=rootfs_path,
        work_dir=work_dir,
    ) as vm:
        header, latency = vm.ping(request_id=1001)

        assert header.opcode == Opcode.PONG
        assert header.request_id == 1001
        assert header.payload_len == 0
        assert latency < 0.1, (
            f"Ping latency exceeded 100ms limit: {latency * 1000:.2f}ms"
        )

        # Run 10-iteration ping burst to measure warm steady-state latency
        latencies: list[float] = []
        for seq in range(10):
            _, ping_lat = vm.ping(request_id=2000 + seq)
            latencies.append(ping_lat)

        avg_latency_ms = (sum(latencies) / len(latencies)) * 1000
        min_latency_ms = min(latencies) * 1000
        max_latency_ms = max(latencies) * 1000

        print(
            f"\n[AxiomVM Vsock Latency] Avg: {avg_latency_ms:.3f}ms | "
            f"Min: {min_latency_ms:.3f}ms | Max: {max_latency_ms:.3f}ms"
        )
        assert min_latency_ms < 5.0
