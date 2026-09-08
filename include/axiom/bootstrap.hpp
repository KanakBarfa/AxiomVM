#pragma once

#include <cerrno>
#include <cstdio>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axiom/common.hpp"

namespace axiom::init {

/// Safely creates a directory if it does not already exist.
inline auto ensure_directory(const char* path, mode_t mode) noexcept -> Result<void> {
    if (mkdir(path, mode) != 0 && errno != EEXIST) {
        return std::unexpected(SystemError::IoError);
    }
    return {};
}

/// Safely mounts a virtual filesystem, ignoring EBUSY if already mounted.
inline auto mount_fs(const char* source, const char* target, const char* fstype,
                     unsigned long flags, const void* data) noexcept -> Result<void> {
    if (mount(source, target, fstype, flags, data) != 0) {
        if (errno == EBUSY) {
            return {};
        }
        return std::unexpected(SystemError::MountError);
    }
    return {};
}

/// Initializes PID 1 guest appliance filesystem hierarchy and pseudo-filesystems.
inline auto mount_essential_filesystems() noexcept -> Result<void> {
    if (getpid() != 1) {
        return {};
    }

    umask(0022);

    auto res = ensure_directory("/proc", 0755);
    if (!res) {
        return res;
    }
    res = mount_fs("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr);
    if (!res) {
        return res;
    }

    res = ensure_directory("/sys", 0755);
    if (!res) {
        return res;
    }
    res = mount_fs("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr);
    if (!res) {
        return res;
    }

    res = ensure_directory("/dev", 0755);
    if (!res) {
        return res;
    }
    res = mount_fs("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
    if (!res) {
        return res;
    }

    res = ensure_directory("/dev/pts", 0755);
    if (!res) {
        return res;
    }
    res = mount_fs("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, nullptr);
    if (!res) {
        return res;
    }

    res = ensure_directory("/dev/shm", 0777);
    if (!res) {
        return res;
    }
    res = mount_fs("tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
    if (!res) {
        return res;
    }

    res = ensure_directory("/tmp", 0777);
    if (!res) {
        return res;
    }
    res = mount_fs("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
    if (!res) {
        return res;
    }

    res = ensure_directory("/run", 0755);
    if (!res) {
        return res;
    }
    res = mount_fs("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");
    if (!res) {
        return res;
    }

    return {};
}

} // namespace axiom::init
