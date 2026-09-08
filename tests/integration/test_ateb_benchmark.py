"""Integration test verifying Axiom Token Efficiency Benchmark (ATEB) executions."""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "host"))

from benchmarks.ateb.benchmark_ast import run_ast_benchmark
from benchmarks.ateb.benchmark_browser import run_browser_benchmark
from benchmarks.ateb.benchmark_exec import run_exec_benchmark
from benchmarks.ateb.benchmark_snapshot import run_snapshot_benchmark


def test_ateb_ast_efficiency() -> None:
    """Verifies AST slicing achieves substantial token reduction."""
    result = run_ast_benchmark()
    assert result.raw_file_tokens > result.axiom_total_tokens
    assert result.reduction_percent >= 40.0


def test_ateb_exec_efficiency() -> None:
    """Verifies command execution and diff capture achieves token reduction."""
    results = run_exec_benchmark()
    assert len(results) >= 2
    for r in results:
        assert r.raw_shell_tokens > r.axiom_exec_tokens
        assert r.reduction_percent > 15.0


def test_ateb_browser_efficiency() -> None:
    """Verifies AXTree achieves high token savings over vision and DOM."""
    result = run_browser_benchmark()
    assert result.vision_savings_percent >= 60.0
    assert result.dom_savings_percent >= 60.0


def test_ateb_snapshot_latency() -> None:
    """Verifies snapshot restore latency is sub-15ms when KVM is present."""
    kernel_path = REPO_ROOT / "tools" / "kernel" / "vmlinux"
    rootfs_path = REPO_ROOT / "build" / "axiom-rootfs.ext4"

    if not kernel_path.exists() or not rootfs_path.exists():
        pytest.skip("Kernel or rootfs artifacts missing for live snapshot benchmark")
    if not os.access("/dev/kvm", os.R_OK | os.W_OK):
        pytest.skip("Read/write access to /dev/kvm required for snapshot benchmark")

    result = run_snapshot_benchmark(iterations=1)
    assert result is not None
    assert result.snapshot_restore_mean_ms < 15.0
    assert result.speedup_factor > 1.0
