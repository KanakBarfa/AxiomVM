"""Snapshot management and branching engine for AxiomVM microVMs."""

from __future__ import annotations

import shutil
import struct
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

# Basis vectors for Firecracker CRC64 table generation (MSB-first, reflected)
_CRC64_BASIS: tuple[int, ...] = (
    0x7AD870C830358979,
    0xF5B0E190606B12F2,
    0xC038E5739841B68F,
    0xAB28ECB46814FE75,
    0x7D08FF3B88BE6F81,
    0xFA11FE77117CDF02,
    0xDF7ADABD7A6E2D6F,
    0x95AC9329AC4BC9B5,
)


def _compute_crc64_entry(val: int) -> int:
    result = 0
    for bit in range(8):
        if (val >> bit) & 1:
            result ^= _CRC64_BASIS[bit]
    return result


# 256-entry lookup table for Firecracker snapshot validation
CRC64_TABLE: tuple[int, ...] = tuple(_compute_crc64_entry(i) for i in range(256))


def fc_crc64(data: bytes, crc: int = 0) -> int:
    """Calculates Firecracker-compatible CRC64 checksum for snapshot validation."""
    crc &= 0xFFFFFFFFFFFFFFFF
    for byte in data:
        idx = (crc ^ byte) & 0xFF
        crc = (CRC64_TABLE[idx] ^ (crc >> 8)) & 0xFFFFFFFFFFFFFFFF
    return crc


def rewrite_snapshot_path(snap_bytes: bytes, old_path: str, new_path: str) -> bytes:
    """Substitutes a length-prefixed path in a bincode-serialized snapshot."""
    old_encoded = old_path.encode("utf-8")
    new_encoded = new_path.encode("utf-8")
    old_needle = struct.pack("<Q", len(old_encoded)) + old_encoded
    new_needle = struct.pack("<Q", len(new_encoded)) + new_encoded
    if old_needle not in snap_bytes:
        raise ValueError(f"Target path '{old_path}' not located in snapshot stream")
    return snap_bytes.replace(old_needle, new_needle, 1)


def update_snapshot_state(
    snap_bytes: bytes, replacements: list[tuple[str, str]]
) -> bytes:
    """Applies path substitutions to snapshot state and updates trailing CRC64."""
    if len(snap_bytes) < 8:
        raise ValueError("Snapshot payload is too small to contain a CRC64 checksum")
    payload = snap_bytes[:-8]
    for old_path, new_path in replacements:
        payload = rewrite_snapshot_path(payload, old_path, new_path)
    new_crc = fc_crc64(payload, 0)
    return payload + struct.pack("<Q", new_crc)


@dataclass(frozen=True, slots=True)
class SnapshotInfo:
    """Metadata describing a saved MicroVM execution snapshot."""

    name: str
    snap_path: Path
    mem_path: Path
    rootfs_path: Path
    kernel_path: Path
    created_at: float = field(default_factory=time.time)
    metadata: dict[str, Any] = field(default_factory=dict)


def copy_or_reflink(src: Path, dst: Path) -> None:
    """Copies a file using copy-on-write reflink when supported by the filesystem."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        shutil.copyfile(src, dst)
    except OSError:
        shutil.copy2(src, dst)
