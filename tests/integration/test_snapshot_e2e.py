"""End-to-end integration test for MicroVM snapshotting, restore, and branching."""

from __future__ import annotations

import os
import sys
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
        pytest.skip(f"MicroVM rootfs image not found at {rootfs_path}")
    if not os.access("/dev/kvm", os.R_OK | os.W_OK):
        pytest.skip("Read/write access to /dev/kvm is required to run Firecracker")

    return kernel_path, rootfs_path


def test_snapshot_create_and_concurrent_branch(artifacts: tuple[Path, Path]) -> None:
    """Verifies snapshot creation, sub-25ms restore, and concurrent branch execution."""
    kernel_path, rootfs_path = artifacts
    primary_work_dir = Path("/tmp/axiom_test_primary_snap")

    vm = MicroVM(
        kernel_path=kernel_path,
        rootfs_path=rootfs_path,
        guest_cid=3,
        guest_port=5200,
        vcpu_count=1,
        mem_size_mib=128,
        work_dir=primary_work_dir,
    )

    with vm:
        hdr, lat = vm.ping(request_id=1)
        assert hdr.opcode == Opcode.PONG
        assert hdr.request_id == 1
        assert lat < 0.05

        # Execute a command to create state inside the guest
        exec_resp, exec_lat = vm.exec(
            "echo snapshot_checkpoint_ok > /tmp/snap_marker.txt",
            request_id=10,
        )
        assert exec_resp.exit_code == 0
        assert exec_lat < 0.1

        # Create snapshot
        snap_info, snap_duration = vm.create_snapshot(name="checkpoint_alpha")
        print(f"[test_snapshot_e2e] Snapshot created in {snap_duration * 1000:.2f}ms")
        assert snap_info.snap_path.exists()
        assert snap_info.mem_path.exists()
        assert snap_info.snap_path.stat().st_size > 1000
        assert snap_info.mem_path.stat().st_size == 128 * 1024 * 1024

        # Verify primary VM is still alive and responsive
        hdr_after, lat_after = vm.ping(request_id=2)
        assert hdr_after.opcode == Opcode.PONG
        assert hdr_after.request_id == 2
        assert lat_after < 0.05

        # Branch into a concurrent MicroVM from the snapshot
        branch_work_dir = Path("/tmp/axiom_test_branch_snap")
        branch_vm, restore_latency = vm.branch(
            name="branch_beta",
            work_dir=branch_work_dir,
            snapshot_name="checkpoint_alpha",
        )
        print(
            f"[test_snapshot_e2e] Branch restored in {restore_latency * 1000:.2f}ms "
            f"(Target: < 25ms)"
        )

        try:
            # Invariant: sub-25ms snapshot restore
            assert restore_latency < 0.025, (
                f"Restore latency {restore_latency * 1000:.2f}ms exceeded 25ms limit"
            )

            # Verify branch VM responsiveness over its independent vsock
            b_hdr, b_lat = branch_vm.ping(request_id=100)
            assert b_hdr.opcode == Opcode.PONG
            assert b_hdr.request_id == 100
            assert b_lat < 0.05

            # Verify guest state persisted in the branch
            b_exec, _ = branch_vm.exec("cat /tmp/snap_marker.txt", request_id=101)
            assert b_exec.exit_code == 0
            assert "snapshot_checkpoint_ok" in b_exec.output

            # Verify write isolation in the branch
            b_iso, _ = branch_vm.exec(
                "echo branch_exclusive > /tmp/branch_only.txt",
                request_id=102,
            )
            assert b_iso.exit_code == 0

            # Verify primary VM is unaffected by branch writes
            p_check, _ = vm.exec("cat /tmp/branch_only.txt", request_id=20)
            assert p_check.exit_code != 0

        finally:
            branch_vm.shutdown()


def test_snapshot_in_place_restore(artifacts: tuple[Path, Path]) -> None:
    """Verifies in-place microVM state rollback from an execution snapshot in < 25ms."""
    kernel_path, rootfs_path = artifacts
    work_dir = Path("/tmp/axiom_test_inplace_snap")

    vm = MicroVM(
        kernel_path=kernel_path,
        rootfs_path=rootfs_path,
        guest_cid=3,
        guest_port=5200,
        vcpu_count=1,
        mem_size_mib=128,
        work_dir=work_dir,
    )

    with vm:
        vm.exec("echo baseline_state > /tmp/state.txt", request_id=10)
        snap, _ = vm.create_snapshot(name="initial_state")

        # Mutate state
        vm.exec("echo corrupted_state > /tmp/state.txt", request_id=11)
        check1, _ = vm.exec("cat /tmp/state.txt", request_id=12)
        assert "corrupted_state" in check1.output

        # Restore from snapshot
        restore_lat = vm.restore_snapshot(snap)
        print(
            f"[test_snapshot_e2e] In-place restore latency: {restore_lat * 1000:.2f}ms"
        )
        assert restore_lat < 0.025

        # Verify state is restored to baseline
        check2, _ = vm.exec("cat /tmp/state.txt", request_id=13)
        assert "baseline_state" in check2.output
