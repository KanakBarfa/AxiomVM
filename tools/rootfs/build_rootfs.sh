#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
ROOTFS_OUTPUT="${1:-${REPO_ROOT}/build/axiom-rootfs.ext4}"
INITD_BIN="${AXIOM_INITD_BIN:-}"

mkdir -p "$(dirname "${ROOTFS_OUTPUT}")"

if [[ -z "${INITD_BIN}" || ! -f "${INITD_BIN}" || "${REPO_ROOT}/include/axiom/server.hpp" -nt "${INITD_BIN}" || "${REPO_ROOT}/include/axiom/ast.hpp" -nt "${INITD_BIN}" || "${REPO_ROOT}/include/axiom/cdp.hpp" -nt "${INITD_BIN}" ]]; then
    echo "[rootfs] Building static axiom-initd with musl toolchain via Docker..."
    docker run --rm -v "${REPO_ROOT}":/src -w /src alpine:3.21 sh -c \
        "apk add --no-cache build-base clang llvm cmake ninja musl-dev linux-headers file busybox-static tree-sitter-dev tree-sitter-static >/dev/null 2>&1 && \
         cmake -B build-musl -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DAXIOM_STATIC_BUILD=ON -DAXIOM_WARNINGS_AS_ERRORS=ON >/dev/null 2>&1 && \
         cmake --build build-musl --target axiom-initd -- -j \$(nproc) >/dev/null 2>&1 && \
         cp build-musl/bin/axiom-initd /src/build/axiom-initd-static && \
         cp /bin/busybox.static /src/build/busybox-static && \
         rm -rf build-musl"
    INITD_BIN="${REPO_ROOT}/build/axiom-initd-static"
fi

echo "[rootfs] Staging root filesystem hierarchy..."
STAGING_DIR="$(mktemp -d /tmp/axiom-staging-XXXXXX)"
trap 'rm -rf "${STAGING_DIR}"' EXIT

mkdir -p "${STAGING_DIR}"/{bin,sbin,proc,sys,dev,tmp,run,etc,usr/lib,usr/bin}
chmod 755 "${STAGING_DIR}"
chmod 1777 "${STAGING_DIR}/tmp"

cp "${INITD_BIN}" "${STAGING_DIR}/init"
chmod 755 "${STAGING_DIR}/init"
ln -sf /init "${STAGING_DIR}/sbin/init"

if [[ -f "${REPO_ROOT}/build/busybox-static" ]]; then
    cp "${REPO_ROOT}/build/busybox-static" "${STAGING_DIR}/bin/busybox"
    chmod 755 "${STAGING_DIR}/bin/busybox"
    for app in sh echo cat ls mkdir touch rm sleep printf grep head tail wc date uname env; do
        ln -sf busybox "${STAGING_DIR}/bin/${app}"
    done
fi

ROOTFS_SIZE="32M"
if [[ "${AXIOM_PACKAGE_CHROMIUM:-0}" == "1" || "${AXIOM_PACKAGE_CHROMIUM:-0}" == "ON" || "${AXIOM_PACKAGE_CHROMIUM:-0}" == "true" ]]; then
    ROOTFS_SIZE="512M"
    echo "[rootfs] Staging Chromium runtime into rootfs..."
    if docker image inspect axiom-test-env >/dev/null 2>&1; then
        docker run --rm -v "${STAGING_DIR}":/dest axiom-test-env sh -c \
            "cp -a /usr/lib/chromium /dest/usr/lib/ && cp -a /usr/bin/chromium* /dest/usr/bin/ 2>/dev/null || true"
    fi
fi

echo "[rootfs] Generating ext4 filesystem image: ${ROOTFS_OUTPUT} (${ROOTFS_SIZE})..."
rm -f "${ROOTFS_OUTPUT}"
truncate -s "${ROOTFS_SIZE}" "${ROOTFS_OUTPUT}"
mke2fs -q -F -t ext4 -d "${STAGING_DIR}" "${ROOTFS_OUTPUT}" "${ROOTFS_SIZE}"

echo "[rootfs] Successfully generated ext4 root filesystem at ${ROOTFS_OUTPUT}"
