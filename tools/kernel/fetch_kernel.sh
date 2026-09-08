#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DEST="${SCRIPT_DIR}/vmlinux"
KERNEL_URL="https://s3.amazonaws.com/spec.ccfc.min/firecracker-ci/v1.10/x86_64/vmlinux-6.1.102"

if [[ -f "${KERNEL_DEST}" ]]; then
    echo "[kernel] Kernel already present at ${KERNEL_DEST}"
    exit 0
fi

echo "[kernel] Downloading Linux 6.1 LTS microVM kernel from ${KERNEL_URL}..."
curl -fsSL "${KERNEL_URL}" -o "${KERNEL_DEST}"
chmod +x "${KERNEL_DEST}"
echo "[kernel] Successfully downloaded kernel to ${KERNEL_DEST}"
