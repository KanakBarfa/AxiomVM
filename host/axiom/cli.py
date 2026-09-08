"""Command-line interface for AxiomVM appliance management."""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path

from axiom.mcp_server import main as run_mcp_server
from axiom.vm import MicroVM, MicroVMError

DEFAULT_KERNEL_PATH = Path(__file__).resolve().parent.parent.parent / "tools" / "kernel" / "vmlinux"
DEFAULT_ROOTFS_PATH = Path(__file__).resolve().parent.parent.parent / "build" / "axiom-rootfs.ext4"


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
        print("  [FAIL] /dev/kvm: Device not found (Hardware virtualization disabled or unavailable)")
        all_ok = False
    elif not os.access(kvm_path, os.R_OK | os.W_OK):
        print("  [FAIL] /dev/kvm: Permission denied (Ensure user is in 'kvm' group)")
        all_ok = False
    else:
        print("  [OK] /dev/kvm: Accessible (read/write)")

    # Check /dev/vhost-vsock
    vsock_path = Path("/dev/vhost-vsock")
    if not vsock_path.exists():
        print("  [WARN] /dev/vhost-vsock: Not loaded (Run 'sudo modprobe vhost_vsock' if needed)")
    else:
        print("  [OK] /dev/vhost-vsock: Present")

    # Check firecracker binary
    fc_bin = shutil.which("firecracker") or "/usr/local/bin/firecracker"
    if not Path(fc_bin).is_file() or not os.access(fc_bin, os.X_OK):
        print("  [FAIL] firecracker: Executable not found in PATH or /usr/local/bin/firecracker")
        all_ok = False
    else:
        print("  [OK] firecracker: " + str(fc_bin))

    # Check kernel asset
    kernel = Path(os.environ.get("AXIOM_KERNEL_PATH", str(DEFAULT_KERNEL_PATH)))
    if not kernel.is_file():
        print("  [WARN] Kernel image: Missing at " + str(kernel))
    else:
        print("  [OK] Kernel image: " + str(kernel))

    # Check rootfs asset
    rootfs = Path(os.environ.get("AXIOM_ROOTFS_PATH", str(DEFAULT_ROOTFS_PATH)))
    if not rootfs.is_file():
        print("  [WARN] Rootfs image: Missing at " + str(rootfs))
    else:
        print("  [OK] Rootfs image: " + str(rootfs))

    if all_ok:
        print("\nAll required host prerequisites satisfied.")
        return 0

    print("\nSome prerequisites are missing or unconfigured.")
    return 1


def show_info() -> int:
    """Displays configuration paths and version information."""
    print("AxiomVM: The Token-First MicroVM Appliance for AI Agents")
    print("Version: 0.1.0")
    print("Default Kernel: " + str(DEFAULT_KERNEL_PATH))
    print("Default Rootfs: " + str(DEFAULT_ROOTFS_PATH))
    print("Vsock Default Port: 5200")
    return 0


def run_appliance(args: argparse.Namespace) -> int:
    """Boots an AxiomVM microVM appliance instance."""
    kernel = Path(args.kernel or os.environ.get("AXIOM_KERNEL_PATH", str(DEFAULT_KERNEL_PATH)))
    rootfs = Path(args.rootfs or os.environ.get("AXIOM_ROOTFS_PATH", str(DEFAULT_ROOTFS_PATH)))
    work_dir = Path(args.work_dir)

    if not kernel.is_file():
        sys.stderr.write("Error: Kernel image not found at " + str(kernel) + "\n")
        return 1
    if not rootfs.is_file():
        sys.stderr.write("Error: Rootfs image not found at " + str(rootfs) + "\n")
        return 1

    print("Booting AxiomVM microVM (CID: " + str(args.cid) + ", Port: " + str(args.port) + ")...")
    try:
        vm = MicroVM(
            kernel_path=kernel,
            rootfs_path=rootfs,
            guest_cid=args.cid,
            guest_port=args.port,
            work_dir=work_dir,
            vcpu_count=args.vcpus,
            mem_size_mib=args.mem,
        )
        vm.start()
        print("AxiomVM booted successfully in " + f"{vm.boot_duration * 1000:.2f}ms.")
        print("Listening for virtio-vsock frames on CID " + str(args.cid) + ":" + str(args.port))
        print("Press Ctrl+C to terminate appliance.")
        try:
            while vm.is_alive():
                vm.process.wait(timeout=1.0)
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
    kernel = Path(args.kernel or os.environ.get("AXIOM_KERNEL_PATH", str(DEFAULT_KERNEL_PATH)))
    rootfs = Path(args.rootfs or os.environ.get("AXIOM_ROOTFS_PATH", str(DEFAULT_ROOTFS_PATH)))
    work_dir = Path(args.work_dir)

    if not kernel.is_file() or not rootfs.is_file():
        sys.stderr.write("Error: Kernel or rootfs image missing.\n")
        return 1

    try:
        vm = MicroVM(
            kernel_path=kernel,
            rootfs_path=rootfs,
            guest_cid=args.cid,
            guest_port=args.port,
            work_dir=work_dir,
        )
        vm.start()
        resp, latency = vm.exec(
            args.command,
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

    # mcp
    subparsers.add_parser("mcp", help="Start the Model Context Protocol stdio server")

    # run
    run_parser = subparsers.add_parser("run", help="Boot an interactive microVM appliance")
    run_parser.add_argument("--kernel", type=str, default=None, help="Path to vmlinux kernel")
    run_parser.add_argument("--rootfs", type=str, default=None, help="Path to rootfs.ext4")
    run_parser.add_argument("--work-dir", type=str, default="/tmp/axiom_run", help="Working directory")
    run_parser.add_argument("--cid", type=int, default=3, help="Guest vsock CID")
    run_parser.add_argument("--port", type=int, default=5200, help="Guest vsock port")
    run_parser.add_argument("--vcpus", type=int, default=1, help="Number of vCPUs")
    run_parser.add_argument("--mem", type=int, default=128, help="Memory size in MiB")

    # exec
    exec_parser = subparsers.add_parser("exec", help="Execute command inside a microVM instance")
    exec_parser.add_argument("command", type=str, help="Command to execute")
    exec_parser.add_argument("--kernel", type=str, default=None, help="Path to vmlinux kernel")
    exec_parser.add_argument("--rootfs", type=str, default=None, help="Path to rootfs.ext4")
    exec_parser.add_argument("--work-dir", type=str, default="/tmp/axiom_exec", help="Working directory")
    exec_parser.add_argument("--cid", type=int, default=3, help="Guest vsock CID")
    exec_parser.add_argument("--port", type=int, default=5200, help="Guest vsock port")
    exec_parser.add_argument("--raw", action="store_true", help="Preserve raw ANSI escape codes")
    exec_parser.add_argument("--diff", action="store_true", default=True, help="Capture file diffs")

    args = parser.parse_args()

    if args.subcommand == "check":
        sys.exit(check_prerequisites())
    elif args.subcommand == "info":
        sys.exit(show_info())
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
