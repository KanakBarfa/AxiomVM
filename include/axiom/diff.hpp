#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <span>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "axiom/common.hpp"
#include "axiom/fixed_vector.hpp"

namespace axiom::diff {

inline constexpr size_t MAX_SNAPSHOT_FILES = 256;
inline constexpr size_t MAX_PATH_LEN = 128;
inline constexpr size_t MAX_SCAN_DEPTH = 4;

#ifndef SYS_getdents64
#if defined(__x86_64__)
#define SYS_getdents64 217
#elif defined(__aarch64__)
#define SYS_getdents64 61
#endif
#endif

/// Kernel dirent64 structure for direct non-allocating directory traversal.
struct [[gnu::packed]] LinuxDirent64 {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[1];
};

/// File metadata record captured during a snapshot.
struct FileRecord {
    std::array<char, MAX_PATH_LEN> path{};
    uint64_t ino{0};
    uint64_t size{0};
    int64_t mtime_sec{0};
    int64_t mtime_nsec{0};
    uint32_t mode{0};

    /// Returns the file path as string_view.
    [[nodiscard]] constexpr auto path_view() const noexcept -> std::string_view {
        return std::string_view(path.data(), strnlen(path.data(), MAX_PATH_LEN));
    }
};

/// Bounded snapshot of file metadata within a target directory tree.
class DirectorySnapshot {
public:
    DirectorySnapshot() noexcept = default;

    /// Captures file metadata for the specified directory tree without heap allocations.
    auto capture(std::string_view base_path) noexcept -> Result<void> {
        entries_.clear();
        if (base_path.empty() || base_path.size() >= MAX_PATH_LEN) {
            return std::unexpected(SystemError::InvalidArgument);
        }
        std::array<char, MAX_PATH_LEN> root_buf{};
        std::memcpy(root_buf.data(), base_path.data(), base_path.size());
        root_buf[base_path.size()] = '\0';

        scan_recursive(root_buf.data(), 0);
        return {};
    }

    /// Returns the captured file records.
    [[nodiscard]] constexpr auto entries() const noexcept -> std::span<const FileRecord> {
        return entries_.as_span();
    }

    /// Clears the snapshot entries.
    constexpr void clear() noexcept { entries_.clear(); }

    /// Returns the number of captured file entries.
    [[nodiscard]] constexpr auto size() const noexcept -> size_t { return entries_.size(); }

private:
    FixedVector<FileRecord, MAX_SNAPSHOT_FILES> entries_{};

    void scan_recursive(const char* dir_path, size_t depth) noexcept {
        if (depth >= MAX_SCAN_DEPTH || entries_.size() >= MAX_SNAPSHOT_FILES) {
            return;
        }

        int fd = open(dir_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            return;
        }

        alignas(alignof(LinuxDirent64)) std::array<char, 1024> buf{};
        while (entries_.size() < MAX_SNAPSHOT_FILES) {
            long nread = syscall(SYS_getdents64, fd, buf.data(), buf.size());
            if (nread <= 0) {
                break;
            }

            size_t pos = 0;
            while (pos < static_cast<size_t>(nread) && entries_.size() < MAX_SNAPSHOT_FILES) {
                const auto* d = reinterpret_cast<const LinuxDirent64*>(buf.data() + pos);
                const char* d_name = d->d_name;

                bool is_special = (std::strcmp(d_name, ".") == 0 || std::strcmp(d_name, "..") == 0);
                if (!is_special) {
                    char full_path[MAX_PATH_LEN];
                    int written =
                        std::snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, d_name);
                    if (written > 0 && static_cast<size_t>(written) < sizeof(full_path)) {
                        struct stat st{};
                        if (fstatat(fd, d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                            if (S_ISDIR(st.st_mode)) {
                                scan_recursive(full_path, depth + 1);
                            } else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
                                FileRecord rec{};
                                std::memcpy(rec.path.data(), full_path,
                                            static_cast<size_t>(written));
                                rec.path[static_cast<size_t>(written)] = '\0';
                                rec.ino = st.st_ino;
                                rec.size = static_cast<uint64_t>(st.st_size);
                                rec.mtime_sec = static_cast<int64_t>(st.st_mtim.tv_sec);
                                rec.mtime_nsec = static_cast<int64_t>(st.st_mtim.tv_nsec);
                                rec.mode = static_cast<uint32_t>(st.st_mode);
                                entries_.push_back(rec);
                            }
                        }
                    }
                }

                pos += d->d_reclen;
            }
        }

        close(fd);
    }
};

/// Computes structured file delta summary between two snapshots into target buffer.
inline auto compute_diff(const DirectorySnapshot& pre, const DirectorySnapshot& post,
                         std::span<char> dest) noexcept -> size_t {
    if (dest.empty()) {
        return 0;
    }

    size_t written_total = 0;

    for (const auto& curr : post.entries()) {
        std::string_view curr_path = curr.path_view();
        bool found = false;
        const FileRecord* old_rec = nullptr;

        for (const auto& prev : pre.entries()) {
            if (prev.path_view() == curr_path) {
                found = true;
                old_rec = &prev;
                break;
            }
        }

        if (!found) {
            int n = std::snprintf(dest.data() + written_total, dest.size() - written_total,
                                  "+ %.*s (%lu B)\n", static_cast<int>(curr_path.size()),
                                  curr_path.data(), static_cast<unsigned long>(curr.size));
            if (n > 0 && static_cast<size_t>(n) < dest.size() - written_total) {
                written_total += static_cast<size_t>(n);
            }
        } else if (old_rec != nullptr) {
            if (old_rec->size != curr.size || old_rec->mtime_sec != curr.mtime_sec ||
                old_rec->mtime_nsec != curr.mtime_nsec) {
                int n = std::snprintf(dest.data() + written_total, dest.size() - written_total,
                                      "M %.*s (%lu -> %lu B)\n", static_cast<int>(curr_path.size()),
                                      curr_path.data(), static_cast<unsigned long>(old_rec->size),
                                      static_cast<unsigned long>(curr.size));
                if (n > 0 && static_cast<size_t>(n) < dest.size() - written_total) {
                    written_total += static_cast<size_t>(n);
                }
            }
        }
    }

    for (const auto& prev : pre.entries()) {
        std::string_view prev_path = prev.path_view();
        bool found = false;

        for (const auto& curr : post.entries()) {
            if (curr.path_view() == prev_path) {
                found = true;
                break;
            }
        }

        if (!found) {
            int n = std::snprintf(dest.data() + written_total, dest.size() - written_total,
                                  "- %.*s\n", static_cast<int>(prev_path.size()), prev_path.data());
            if (n > 0 && static_cast<size_t>(n) < dest.size() - written_total) {
                written_total += static_cast<size_t>(n);
            }
        }
    }

    return written_total;
}

} // namespace axiom::diff
