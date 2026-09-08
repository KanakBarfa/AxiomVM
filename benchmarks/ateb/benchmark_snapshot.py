"""ATEB Snapshot Benchmark: Cold boot vs copy-on-write snapshot restore latency."""

from __future__ import annotations

import os
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "host"))

from axiom.vm import MicroVM


@dataclass(frozen=True, slots=True)
class SnapshotBenchmarkResult:
    """Benchmark metrics comparing cold boot against snapshot restore latencies."""

    iterations: int
    cold_boot_mean_ms: float
    snapshot_restore_mean_ms: float
    branch_spawn_mean_ms: float
    speedup_factor: float


def run_snapshot_benchmark(
    iterations: int = 3,
) -> SnapshotBenchmarkResult | None:
    """Executes live cold boot vs snapshot restore benchmark if KVM is available."""
    kernel_path = REPO_ROOT / "tools" / "kernel" / "vmlinux"
    rootfs_path = REPO_ROOT / "build" / "axiom-rootfs.ext4"

    if (
        not kernel_path.exists()
        or not rootfs_path.exists()
        or not os.access("/dev/kvm", os.R_OK | os.W_OK)
    ):
        return None

    cold_boot_latencies: list[float] = []
    restore_latencies: list[float] = []
    branch_latencies: list[float] = []

    with tempfile.TemporaryDirectory() as tmp_dir:
        base_dir = Path(tmp_dir)

        for i in range(iterations):
            # Cold boot benchmark
            vm_work_dir = base_dir / f"cold_vm_{i}"
            vm = MicroVM(
                kernel_path=kernel_path,
                rootfs_path=rootfs_path,
                guest_cid=30 + i,
                guest_port=5200,
                work_dir=vm_work_dir,
            )
            t0 = time.perf_counter()
            vm.start()
            vm.ping()
            cold_ms = (time.perf_counter() - t0) * 1000.0
            cold_boot_latencies.append(cold_ms)

            # Create snapshot checkpoint
            snap_info, _ = vm.create_snapshot("bench_snap")

            # Restore snapshot benchmark
            res_duration = vm.restore_snapshot(snap_info)
            restore_latencies.append(res_duration * 1000.0)

            # Branch spawn benchmark
            branch_vm, branch_sec = vm.branch(
                name=f"branch_{i}",
                work_dir=base_dir / f"branch_dir_{i}",
                snapshot_name="bench_snap",
            )
            branch_latencies.append(branch_sec * 1000.0)

            branch_vm.shutdown()
            vm.shutdown()

    cold_mean = sum(cold_boot_latencies) / len(cold_boot_latencies)
    restore_mean = sum(restore_latencies) / len(restore_latencies)
    branch_mean = sum(branch_latencies) / len(branch_latencies)
    speedup = cold_mean / restore_mean if restore_mean > 0 else 1.0

    return SnapshotBenchmarkResult(
        iterations=iterations,
        cold_boot_mean_ms=cold_mean,
        snapshot_restore_mean_ms=restore_mean,
        branch_spawn_mean_ms=branch_mean,
        speedup_factor=speedup,
    )
