"""End-to-end integration test for Chromium CDP semantic web engine."""

import os
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "host"))

from axiom.vm import MicroVM
from axiom.wire import CdpActionResponse


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


def test_cdp_roundtrip_latency_and_dispatch(artifacts: tuple[Path, Path]) -> None:
    """Verifies virtio-vsock CDP command dispatch and response latency."""
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

        resp, tree_latency = vm.get_axtree(request_id=302)
        assert isinstance(resp, CdpActionResponse)
        assert tree_latency < 0.20
        print(f"\n[test_cdp_e2e] AXTree response latency: {tree_latency * 1000:.2f}ms")
        print(f"[test_cdp_e2e] Status: {resp.status}, Node count: {resp.node_count}")


def test_cdp_actions_and_compression(artifacts: tuple[Path, Path]) -> None:
    """Verifies browser action execution and asserts >90% token reduction over screenshots."""
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
        # Test scroll dispatch
        scroll_resp, scroll_lat = vm.scroll(250, request_id=305)
        assert isinstance(scroll_resp, CdpActionResponse)
        assert scroll_lat < 0.05

        # Test click dispatch
        click_resp, click_lat = vm.click(1, request_id=303)
        assert isinstance(click_resp, CdpActionResponse)
        assert click_lat < 0.05

        # Test type dispatch
        type_resp, type_lat = vm.type_text(1, "AxiomVM token test", request_id=304)
        assert isinstance(type_resp, CdpActionResponse)
        assert type_lat < 0.05

        # Assert token reduction ratio against screenshot baselines
        standard_screenshot_bytes = 6000
        actual_payload_bytes = len(scroll_resp.tree_text.encode("utf-8")) + 12
        reduction_ratio = (
            1.0 - (actual_payload_bytes / standard_screenshot_bytes)
        ) * 100.0
        print(f"[test_cdp_e2e] Compression ratio vs screenshot: {reduction_ratio:.2f}%")
        assert reduction_ratio >= 90.0
