"""Automated asset cache and provisioning management for AxiomVM."""

from __future__ import annotations

import contextlib
import os
import shutil
import urllib.request
from pathlib import Path

DEFAULT_CACHE_DIR = Path.home() / ".axiom" / "cache"
DEFAULT_RELEASE_TAG = "v0.1.0"
RELEASE_BASE_URL = os.environ.get(
    "AXIOM_RELEASE_BASE_URL",
    f"https://github.com/KanakBarfa/AxiomVM/releases/download/{DEFAULT_RELEASE_TAG}",
)


def get_repo_root() -> Path:
    """Returns the root directory of the local AxiomVM repository if running from source."""
    return Path(__file__).resolve().parent.parent.parent


def download_file(url: str, dest: Path) -> None:
    """Downloads a remote file to the destination path with a temporary buffer."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    temp_dest = dest.with_suffix(".tmp")
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "AxiomVM-Downloader/0.1.0"},
    )
    try:
        with urllib.request.urlopen(req) as response, temp_dest.open("wb") as out_file:
            shutil.copyfileobj(response, out_file)
        temp_dest.replace(dest)
    finally:
        temp_dest.unlink(missing_ok=True)


def download_assets(
    cache_dir: Path | str | None = None, force: bool = False
) -> tuple[Path, Path]:
    """Downloads prebuilt vmlinux and axiom-rootfs.ext4 to the local cache directory."""
    c_dir = Path(cache_dir) if cache_dir else DEFAULT_CACHE_DIR
    c_dir.mkdir(parents=True, exist_ok=True)

    kernel_path = c_dir / "vmlinux"
    rootfs_path = c_dir / "axiom-rootfs.ext4"

    if force or not kernel_path.is_file():
        kernel_url = f"{RELEASE_BASE_URL}/vmlinux"
        print(f"Downloading prebuilt kernel from {kernel_url}...")
        download_file(kernel_url, kernel_path)
        kernel_path.chmod(0o755)

    if force or not rootfs_path.is_file():
        rootfs_url = f"{RELEASE_BASE_URL}/axiom-rootfs.ext4"
        print(f"Downloading prebuilt rootfs from {rootfs_url}...")
        download_file(rootfs_url, rootfs_path)

    return kernel_path, rootfs_path


def resolve_assets(
    cache_dir: Path | str | None = None, auto_download: bool = False
) -> tuple[Path | None, Path | None]:
    """Resolves kernel and rootfs paths from environment, repository, or cache."""
    # 1. Environment variables
    env_kernel = os.environ.get("AXIOM_KERNEL_PATH")
    env_rootfs = os.environ.get("AXIOM_ROOTFS_PATH")
    if env_kernel and env_rootfs:
        k, r = Path(env_kernel), Path(env_rootfs)
        if k.is_file() and r.is_file():
            return k, r

    # 2. Local repository paths
    repo_root = get_repo_root()
    repo_kernel = repo_root / "tools" / "kernel" / "vmlinux"
    repo_rootfs = repo_root / "build" / "axiom-rootfs.ext4"
    if repo_kernel.is_file() and repo_rootfs.is_file():
        return repo_kernel, repo_rootfs

    # 3. Cache directory
    c_dir = Path(cache_dir) if cache_dir else DEFAULT_CACHE_DIR
    cached_kernel = c_dir / "vmlinux"
    cached_rootfs = c_dir / "axiom-rootfs.ext4"
    if cached_kernel.is_file() and cached_rootfs.is_file():
        return cached_kernel, cached_rootfs

    # 4. Auto-download if enabled
    if auto_download:
        with contextlib.suppress(Exception):
            return download_assets(c_dir)

    return (
        cached_kernel if cached_kernel.is_file() else None,
        cached_rootfs if cached_rootfs.is_file() else None,
    )
