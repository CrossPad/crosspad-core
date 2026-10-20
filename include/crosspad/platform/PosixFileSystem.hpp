// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/platform/IFileSystem.hpp"

namespace crosspad {

/// IFileSystem on POSIX (ESP-IDF's VFS, Linux, macOS), and on the CRT and
/// std::filesystem where there is no POSIX (Windows, the simulator built
/// with MSVC). Holds no state: one instance serves every thread.
class PosixFileSystem : public IFileSystem {
public:
    bool   list(const char* dir, EntryFn each, void* ctx) override;
    FsStat stat(const char* path, FileStat* out) override;
    bool   makeDir(const char* path) override;
    bool   remove(const char* path) override;
    bool   removeDir(const char* path) override;
    bool   rename(const char* from, const char* to) override;
    int    openRead(const char* path) override;
    long   read(int fd, void* dst, size_t n) override;
    bool   seek(int fd, uint32_t offset) override;
    void   close(int fd) override;
};

} // namespace crosspad
