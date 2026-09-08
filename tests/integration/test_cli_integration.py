"""Integration tests for the unified AxiomVM host CLI."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent.parent


def test_cli_help() -> None:
    """Verifies that axiom --help displays the usage banner and subcommands."""
    proc = subprocess.run(
        [sys.executable, "-m", "axiom.cli", "--help"],
        cwd=REPO_ROOT,
        env={**os.environ, "PYTHONPATH": str(REPO_ROOT / "host")},
        capture_output=True,
        text=True,
    )
    assert proc.returncode == 0
    assert "AxiomVM: The Token-First MicroVM Appliance for AI Agents" in proc.stdout
    assert "check" in proc.stdout
    assert "info" in proc.stdout
    assert "mcp" in proc.stdout
    assert "run" in proc.stdout
    assert "exec" in proc.stdout


def test_cli_info() -> None:
    """Verifies that axiom info outputs version and configuration paths."""
    proc = subprocess.run(
        [sys.executable, "-m", "axiom.cli", "info"],
        cwd=REPO_ROOT,
        env={**os.environ, "PYTHONPATH": str(REPO_ROOT / "host")},
        capture_output=True,
        text=True,
    )
    assert proc.returncode == 0
    assert "Version: 0.1.0" in proc.stdout
    assert "Default Kernel:" in proc.stdout
    assert "Default Rootfs:" in proc.stdout


def test_cli_check_prerequisites() -> None:
    """Verifies that axiom check validates host prerequisites cleanly."""
    proc = subprocess.run(
        [sys.executable, "-m", "axiom.cli", "check"],
        cwd=REPO_ROOT,
        env={**os.environ, "PYTHONPATH": str(REPO_ROOT / "host")},
        capture_output=True,
        text=True,
    )
    assert "Checking AxiomVM host prerequisites..." in proc.stdout
    assert "[OK] Operating system: Linux" in proc.stdout
    if os.access("/dev/kvm", os.R_OK | os.W_OK):
        assert "[OK] /dev/kvm: Accessible (read/write)" in proc.stdout


def test_cli_exec_e2e() -> None:
    """Verifies that axiom exec runs a command end-to-end inside Firecracker."""
    kernel_path = REPO_ROOT / "tools" / "kernel" / "vmlinux"
    rootfs_path = REPO_ROOT / "build" / "axiom-rootfs.ext4"

    if not kernel_path.exists() or not rootfs_path.exists():
        pytest.skip("Appliance kernel or rootfs missing")

    if not os.access("/dev/kvm", os.R_OK | os.W_OK):
        pytest.skip("/dev/kvm not accessible")

    work_dir = Path("/tmp/axiom_test_cli_exec")
    proc = subprocess.run(
        [
            sys.executable,
            "-m",
            "axiom.cli",
            "exec",
            "echo CLI_INTEGRATION_SUCCESS",
            "--work-dir",
            str(work_dir),
            "--cid",
            "25",
            "--port",
            "5200",
        ],
        cwd=REPO_ROOT,
        env={**os.environ, "PYTHONPATH": str(REPO_ROOT / "host")},
        capture_output=True,
        text=True,
    )
    assert proc.returncode == 0
    assert "CLI_INTEGRATION_SUCCESS" in proc.stdout
