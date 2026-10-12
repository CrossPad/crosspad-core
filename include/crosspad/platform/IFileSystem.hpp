// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace crosspad {

/// What IFileSystem::stat() found.
enum class FsStat : uint8_t {
    Found,
    Missing,   ///< nothing by that name
    Failed,    ///< the path could not be asked about (I/O error, no handle, ...)
};

struct FileStat {
    bool     isDir    = false;
    uint64_t size     = 0;
    int64_t  modified = 0;   ///< last written, unix seconds
};

/**
 * @brief The file system's directories and metadata, for portable code.
 *
 * Paths are whole and UTF-8, as the platform's file system names them
 * (on the board under the VFS mount, e.g. /sdcard/crosspad/kits). File
 * contents stay with stdio (fopen/fread/fwrite); openRead() is here for the
 * unbuffered descriptor reads a streamer makes in large pieces.
 *
 * Implementations:
 *   - PosixFileSystem (core): POSIX on ESP-IDF's VFS, Linux and macOS;
 *     the CRT and std::filesystem on Windows. The default.
 *   - a test double, set with PlatformServices::setFileSystem().
 */
class IFileSystem {
public:
    /// One entry of a listing: its name (no path), and whether it is a directory.
    using EntryFn = void (*)(void* ctx, const char* name, bool isDir);

    virtual ~IFileSystem() = default;

    /// @p each for every entry of @p dir but "." and "..", in the order the
    /// file system keeps them. The directory is closed before this returns,
    /// also when @p each throws. False when @p dir cannot be opened.
    virtual bool list(const char* dir, EntryFn each, void* ctx) = 0;

    /// Found fills @p out when it is given.
    virtual FsStat stat(const char* path, FileStat* out) = 0;

    /// True when the directory is there afterwards, made now or before.
    virtual bool makeDir(const char* path) = 0;

    /// A file.
    virtual bool remove(const char* path) = 0;

    /// An empty directory.
    virtual bool removeDir(const char* path) = 0;

    /// Whether an existing @p to is replaced is the file system's call (FATFS
    /// refuses), so a caller that means to replace removes it first.
    virtual bool rename(const char* from, const char* to) = 0;

    /// An unbuffered read-only descriptor, or -1.
    virtual int openRead(const char* path) = 0;

    /// Bytes read into @p dst, 0 at the end, negative on an error.
    virtual long read(int fd, void* dst, size_t n) = 0;

    /// To @p offset bytes from the start.
    virtual bool seek(int fd, uint32_t offset) = 0;

    virtual void close(int fd) = 0;

    bool exists(const char* path) { return stat(path, nullptr) == FsStat::Found; }

    bool isDir(const char* path) {
        FileStat st;
        return stat(path, &st) == FsStat::Found && st.isDir;
    }

    /// list() with a callable taking (const char* name, bool isDir).
    template <typename F> bool forEach(const std::string& dir, F each) {
        return list(dir.c_str(),
                    [](void* ctx, const char* name, bool isDir) { (*static_cast<F*>(ctx))(name, isDir); },
                    &each);
    }

    /// @p dir and everything under it, @p afterEntry(ctx) after each entry
    /// removed. The names of a directory are read before any of it goes:
    /// removing entries from a directory open for reading is not something
    /// FATFS promises.
    bool removeTree(const std::string& dir, void (*afterEntry)(void*) = nullptr, void* ctx = nullptr);
};

/// PlatformServices::fileSystem: the platform's, or the portable POSIX one.
IFileSystem& getFileSystem();

} // namespace crosspad
