// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/platform/PosixFileSystem.hpp"

#include <cstdio>
#include <string>

#ifdef _WIN32
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <io.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace crosspad {

#ifdef _WIN32
namespace {

std::filesystem::path native(const char* p) { return std::filesystem::u8path(p); }

} // namespace

bool PosixFileSystem::list(const char* dir, EntryFn each, void* ctx)
{
    std::error_code ec;
    std::filesystem::directory_iterator it(native(dir), ec);
    if (ec) return false;
    for (const auto& e : it) {
        const auto u8 = e.path().filename().u8string();
        std::error_code dec;
        each(ctx, std::string(u8.begin(), u8.end()).c_str(), e.is_directory(dec));
    }
    return true;
}

FsStat PosixFileSystem::stat(const char* path, FileStat* out)
{
    std::error_code ec;
    const auto st = std::filesystem::status(native(path), ec);
    if (st.type() == std::filesystem::file_type::not_found) return FsStat::Missing;
    if (st.type() == std::filesystem::file_type::none) return FsStat::Failed;
    if (out) {
        out->isDir = std::filesystem::is_directory(st);
        out->size = out->isDir ? 0 : std::filesystem::file_size(native(path), ec);
        if (ec) return FsStat::Failed;
        const auto t = std::filesystem::last_write_time(native(path), ec);
        if (ec) return FsStat::Failed;
        const auto sys = t - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now();
        out->modified = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
    }
    return FsStat::Found;
}

bool PosixFileSystem::makeDir(const char* path)
{
    std::error_code ec;
    if (std::filesystem::is_directory(native(path), ec)) return true;
    return std::filesystem::create_directory(native(path), ec);
}

bool PosixFileSystem::remove(const char* path)
{
    std::error_code ec;
    return std::filesystem::remove(native(path), ec);
}

bool PosixFileSystem::removeDir(const char* path)
{
    std::error_code ec;
    return std::filesystem::remove(native(path), ec);
}

bool PosixFileSystem::rename(const char* from, const char* to)
{
    std::error_code ec;
    std::filesystem::rename(native(from), native(to), ec);
    return !ec;
}

int  PosixFileSystem::openRead(const char* path) { return ::_wopen(native(path).c_str(), _O_RDONLY | _O_BINARY); }
long PosixFileSystem::read(int fd, void* dst, size_t n) { return ::_read(fd, dst, (unsigned)n); }
bool PosixFileSystem::seek(int fd, uint32_t offset) { return ::_lseeki64(fd, offset, SEEK_SET) >= 0; }
void PosixFileSystem::close(int fd) { ::_close(fd); }

#else

namespace {

/// What a directory made here may do; FATFS keeps no modes.
constexpr mode_t kDirMode = 0755;

bool statIsDir(const std::string& path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

} // namespace

bool PosixFileSystem::list(const char* dir, EntryFn each, void* ctx)
{
    DIR* d = ::opendir(dir);
    if (!d) return false;
    /* Closed however the walk ends: on the card a directory open is one of a
     * handful of FAT handles, and a callback out of memory must not keep it. */
    const std::unique_ptr<DIR, int (*)(DIR*)> closer(d, ::closedir);
    while (const dirent* e = ::readdir(d)) {
        const char* name = e->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
        /* The entry's own type where the file system gives one (FATFS always
         * does); a link is asked about for what it points at. */
        bool isDir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
            std::string path = dir;
            if (path.empty() || path.back() != '/') path += '/';
            isDir = statIsDir(path + name);
        }
        each(ctx, name, isDir);
    }
    return true;
}

FsStat PosixFileSystem::stat(const char* path, FileStat* out)
{
    struct stat st;
    if (::stat(path, &st) != 0) return errno == ENOENT ? FsStat::Missing : FsStat::Failed;
    if (out) {
        out->isDir = S_ISDIR(st.st_mode);
        out->size = (uint64_t)st.st_size;
        out->modified = (int64_t)st.st_mtime;
    }
    return FsStat::Found;
}

bool PosixFileSystem::makeDir(const char* path)
{
    return ::mkdir(path, kDirMode) == 0 || statIsDir(path);
}

bool PosixFileSystem::remove(const char* path) { return ::unlink(path) == 0; }
bool PosixFileSystem::removeDir(const char* path) { return ::rmdir(path) == 0; }
bool PosixFileSystem::rename(const char* from, const char* to) { return std::rename(from, to) == 0; }

int  PosixFileSystem::openRead(const char* path) { return ::open(path, O_RDONLY); }
long PosixFileSystem::read(int fd, void* dst, size_t n) { return (long)::read(fd, dst, n); }
bool PosixFileSystem::seek(int fd, uint32_t offset) { return ::lseek(fd, (off_t)offset, SEEK_SET) >= 0; }
void PosixFileSystem::close(int fd) { ::close(fd); }

#endif

} // namespace crosspad
