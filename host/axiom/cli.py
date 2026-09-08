"""Command-line interface for AxiomVM appliance management."""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import time
from pathlib import Path

from axiom.assets import DEFAULT_CACHE_DIR, download_assets, resolve_assets
from axiom.mcp_server import main as run_mcp_server
from axiom.vm import MicroVM, MicroVMError

DEFAULT_KERNEL_PATH = (
    Path(__file__).resolve().parent.parent.parent / "tools" / "kernel" / "vmlinux"
)
DEFAULT_ROOTFS_PATH = (
    Path(__file__).resolve().parent.parent.parent / "build" / "axiom-rootfs.ext4"
)


def check_prerequisites() -> int:
    """Verifies host environment prerequisites for running Firecracker microVMs."""
    print("Checking AxiomVM host prerequisites...")
    all_ok = True

    # Check Linux OS
    if sys.platform != "linux":
        print("  [FAIL] Operating system: Linux required (found " + sys.platform + ")")
        all_ok = False
    else:
        print("  [OK] Operating system: Linux")

    # Check /dev/kvm
    kvm_path = Path("/dev/kvm")
    if not kvm_path.exists():
        print(
            "  [FAIL] /dev/kvm: Device not found (Hardware virtualization disabled or unavailable)"
        )
        all_ok = False
    elif not os.access(kvm_path, os.R_OK | os.W_OK):
        print("  [FAIL] /dev/kvm: Permission denied (Ensure user is in 'kvm' group)")
        all_ok = False
    else:
        print("  [OK] /dev/kvm: Accessible (read/write)")

    # Check /dev/vhost-vsock
    vsock_path = Path("/dev/vhost-vsock")
    if not vsock_path.exists():
        print(
            "  [WARN] /dev/vhost-vsock: Not loaded (Run 'sudo modprobe vhost_vsock' if needed)"
        )
    else:
        print("  [OK] /dev/vhost-vsock: Present")

    # Check firecracker binary
    fc_bin = shutil.which("firecracker") or "/usr/local/bin/firecracker"
    if not Path(fc_bin).is_file() or not os.access(fc_bin, os.X_OK):
        print(
            "  [FAIL] firecracker: Executable not found in PATH or /usr/local/bin/firecracker"
        )
        all_ok = False
    else:
        print("  [OK] firecracker: " + str(fc_bin))

    # Check kernel and rootfs assets
    res_k, res_r = resolve_assets()
    if not res_k:
        print(
            "  [WARN] Kernel image: Missing (Run 'axiom setup' to download prebuilt assets)"
        )
    else:
        print("  [OK] Kernel image: " + str(res_k))

    if not res_r:
        print(
            "  [WARN] Rootfs image: Missing (Run 'axiom setup' to download prebuilt assets)"
        )
    else:
        print("  [OK] Rootfs image: " + str(res_r))

    if all_ok:
        print("\nAll required host prerequisites satisfied.")
        return 0

    print("\nSome prerequisites are missing or unconfigured.")
    return 1


def show_info() -> int:
    """Displays configuration paths and version information."""
    res_k, res_r = resolve_assets()
    print("AxiomVM: The Token-First MicroVM Appliance for AI Agents")
    print("Version: 0.1.0")
    print("Default Kernel: " + str(res_k or DEFAULT_KERNEL_PATH))
    print("Default Rootfs: " + str(res_r or DEFAULT_ROOTFS_PATH))
    print("Default Cache: " + str(DEFAULT_CACHE_DIR))
    print("Vsock Default Port: 5200")
    return 0


def setup_assets(args: argparse.Namespace) -> int:
    """Downloads and caches release assets for AxiomVM."""
    cache_dir = Path(args.cache_dir) if args.cache_dir else None
    try:
        kernel, rootfs = download_assets(cache_dir=cache_dir, force=args.force)
        print("Kernel ready at: " + str(kernel))
        print("Rootfs ready at: " + str(rootfs))
        return 0
    except (OSError, RuntimeError, ValueError) as err:
        sys.stderr.write("Setup failed: " + str(err) + "\n")
        return 1


def run_appliance(args: argparse.Namespace) -> int:
    """Boots an AxiomVM microVM appliance instance."""
    kernel = Path(args.kernel) if args.kernel else None
    rootfs = Path(args.rootfs) if args.rootfs else None
    if kernel is None or rootfs is None:
        res_k, res_r = resolve_assets(auto_download=True)
        if kernel is None:
            kernel = res_k
        if rootfs is None:
            rootfs = res_r

    if kernel is None or not kernel.is_file():
        sys.stderr.write(
            "Error: Kernel image not found. Run 'axiom setup' or specify --kernel.\n"
        )
        return 1
    if rootfs is None or not rootfs.is_file():
        sys.stderr.write(
            "Error: Rootfs image not found. Run 'axiom setup' or specify --rootfs.\n"
        )
        return 1

    work_dir = Path(args.work_dir)
    print(
        "Booting AxiomVM microVM (CID: "
        + str(args.cid)
        + ", Port: "
        + str(args.port)
        + ")..."
    )
    try:
        vm = MicroVM(
            kernel_path=kernel,
            rootfs_path=rootfs,
            guest_cid=args.cid,
            guest_port=args.port,
            work_dir=work_dir,
            vcpu_count=args.vcpus,
            mem_size_mib=args.mem,
            mounts=args.mount,
        )
        vm.start()
        print("AxiomVM booted successfully in " + f"{vm.boot_duration * 1000:.2f}ms.")
        print(
            "Listening for virtio-vsock frames on CID "
            + str(args.cid)
            + ":"
            + str(args.port)
        )
        print("Press Ctrl+C to terminate appliance.")
        try:
            while vm.is_alive():
                if vm.process is not None:
                    vm.process.wait(timeout=1.0)
                else:
                    time.sleep(1.0)
        except (KeyboardInterrupt, TimeoutError):
            pass
        finally:
            print("Stopping appliance...")
            vm.stop()
            print("Appliance terminated cleanly.")
        return 0
    except MicroVMError as err:
        sys.stderr.write("MicroVM error: " + str(err) + "\n")
        return 1


def exec_command(args: argparse.Namespace) -> int:
    """Executes a single command inside a microVM instance and prints result."""
    kernel = Path(args.kernel) if args.kernel else None
    rootfs = Path(args.rootfs) if args.rootfs else None
    if kernel is None or rootfs is None:
        res_k, res_r = resolve_assets(auto_download=True)
        if kernel is None:
            kernel = res_k
        if rootfs is None:
            rootfs = res_r

    if kernel is None or not kernel.is_file() or rootfs is None or not rootfs.is_file():
        sys.stderr.write("Error: Kernel or rootfs image missing. Run 'axiom setup'.\n")
        return 1

    work_dir = Path(args.work_dir)
    try:
        vm = MicroVM(
            kernel_path=kernel,
            rootfs_path=rootfs,
            guest_cid=args.cid,
            guest_port=args.port,
            work_dir=work_dir,
            mounts=args.mount,
        )
        vm.start()
        resp, _latency = vm.exec(
            args.command,
            cwd=args.cwd or "",
            strip_ansi=not args.raw,
            capture_diff=args.diff,
        )
        sys.stdout.write(resp.output)
        if args.diff and resp.diff:
            print("\n--- File Diff ---")
            sys.stdout.write(resp.diff)
        vm.stop()
        return resp.exit_code
    except MicroVMError as err:
        sys.stderr.write("Execution failed: " + str(err) + "\n")
        return 1


def main() -> None:
    """Primary entrypoint for the axiom CLI utility."""
    parser = argparse.ArgumentParser(
        prog="axiom",
        description="AxiomVM: The Token-First MicroVM Appliance for AI Agents",
    )
    subparsers = parser.add_subparsers(dest="subcommand", help="Available subcommands")

    # check
    subparsers.add_parser("check", help="Check host virtualization and dependencies")

    # info
    subparsers.add_parser("info", help="Display AxiomVM configuration details")

    # setup
    setup_parser = subparsers.add_parser(
        "setup", help="Download and cache prebuilt VM assets"
    )
    setup_parser.add_argument(
        "--cache-dir", type=str, default=None, help="Custom cache directory"
    )
    setup_parser.add_argument(
        "--force", action="store_true", help="Force re-downloading assets"
    )

    # mcp
    subparsers.add_parser("mcp", help="Start the Model Context Protocol stdio server")

    # run
    run_parser = subparsers.add_parser(
        "run", help="Boot an interactive microVM appliance"
    )
    run_parser.add_argument(
        "--kernel", type=str, default=None, help="Path to vmlinux kernel"
    )
    run_parser.add_argument(
        "--rootfs", type=str, default=None, help="Path to rootfs.ext4"
    )
    run_parser.add_argument(
        "--work-dir", type=str, default="/tmp/axiom_run", help="Working directory"
    )
    run_parser.add_argument("--cid", type=int, default=3, help="Guest vsock CID")
    run_parser.add_argument("--port", type=int, default=5200, help="Guest vsock port")
    run_parser.add_argument("--vcpus", type=int, default=1, help="Number of vCPUs")
    run_parser.add_argument("--mem", type=int, default=128, help="Memory size in MiB")
    run_parser.add_argument(
        "--mount",
        action="append",
        default=None,
        help="Mount host directory into guest (host_path:guest_path)",
    )

    # exec
    exec_parser = subparsers.add_parser(
        "exec", help="Execute command inside a microVM instance"
    )
    exec_parser.add_argument("command", type=str, help="Command to execute")
    exec_parser.add_argument(
        "--kernel", type=str, default=None, help="Path to vmlinux kernel"
    )
    exec_parser.add_argument(
        "--rootfs", type=str, default=None, help="Path to rootfs.ext4"
    )
    exec_parser.add_argument(
        "--work-dir", type=str, default="/tmp/axiom_exec", help="Working directory"
    )
    exec_parser.add_argument("--cid", type=int, default=3, help="Guest vsock CID")
    exec_parser.add_argument("--port", type=int, default=5200, help="Guest vsock port")
    exec_parser.add_argument(
        "--cwd", type=str, default="", help="Working directory inside guest microVM"
    )
    exec_parser.add_argument(
        "--raw", action="store_true", help="Preserve raw ANSI escape codes"
    )
    exec_parser.add_argument(
        "--diff", action="store_true", default=True, help="Capture file diffs"
    )
    exec_parser.add_argument(
        "--mount",
        action="append",
        default=None,
        help="Mount host directory into guest (host_path:guest_path)",
    )

    args = parser.parse_args()

    if args.subcommand == "check":
        sys.exit(check_prerequisites())
    elif args.subcommand == "info":
        sys.exit(show_info())
    elif args.subcommand == "setup":
        sys.exit(setup_assets(args))
    elif args.subcommand == "mcp":
        run_mcp_server()
    elif args.subcommand == "run":
        sys.exit(run_appliance(args))
    elif args.subcommand == "exec":
        sys.exit(exec_command(args))
    else:
        parser.print_help()
        sys.exit(0)


if __name__ == "__main__":
    main()
