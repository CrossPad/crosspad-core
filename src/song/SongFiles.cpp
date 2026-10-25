// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/song/SongFiles.hpp"

#include "crosspad/platform/IFileSystem.hpp"

#include <algorithm>
#include <cstdio>

namespace crosspad {

namespace {

constexpr size_t kPieceBytes = 4096;

struct Entry {
    std::string name;
    bool isDir;
};

/// @p dir's entries, read whole so its handle is closed when this returns.
bool readEntries(const std::string& dir, std::vector<Entry>& out)
{
    return getFileSystem().forEach(dir, [&](const char* name, bool isDir) { out.push_back({name, isDir}); });
}

bool copyLevel(const std::string& src, const std::string& dst, const std::string& rel,
               const std::function<bool(const std::string&)>& keep, const std::function<void()>& yield)
{
    if (!getFileSystem().makeDir(dst.c_str())) return false;
    std::vector<Entry> entries;
    if (!readEntries(src, entries)) return false;
    for (const Entry& e : entries) {
        const std::string childRel = rel.empty() ? e.name : rel + "/" + e.name;
        if (!keep(childRel)) continue;
        const std::string from = src + "/" + e.name;
        const std::string to = dst + "/" + e.name;
        if (!(e.isDir ? copyLevel(from, to, childRel, keep, yield) : song_copy_file(from, to, yield))) return false;
    }
    return true;
}

} // namespace

bool song_copy_file(const std::string& src, const std::string& dst, const std::function<void()>& yield)
{
    IFileSystem& fs = getFileSystem();
    const std::string part = dst + ".part";
    const int in = fs.openRead(src.c_str());
    if (in < 0) return false;
    FILE* out = std::fopen(part.c_str(), "wb");
    if (!out) {
        fs.close(in);
        return false;
    }
    uint8_t piece[kPieceBytes];
    bool ok = true;
    for (;;) {
        const long n = fs.read(in, piece, sizeof piece);
        if (n < 0) ok = false;
        if (n <= 0) break;
        if (std::fwrite(piece, 1, (size_t)n, out) != (size_t)n) {
            ok = false;
            break;
        }
        if (yield) yield();
    }
    fs.close(in);
    ok = (std::fclose(out) == 0) && ok;
    // A rename does not replace on every file system, so a file in the way goes first.
    if (ok && fs.exists(dst.c_str()) && !fs.isDir(dst.c_str())) ok = fs.remove(dst.c_str());
    ok = ok && fs.rename(part.c_str(), dst.c_str());
    if (!ok) fs.remove(part.c_str());
    return ok;
}

bool song_copy_tree(const std::string& srcDir, const std::string& dstDir,
                    const std::function<bool(const std::string& rel)>& keep,
                    const std::function<void()>& yield)
{
    return copyLevel(srcDir, dstDir, "", keep, yield);
}

std::vector<std::string> song_list_dirs(const std::string& dir)
{
    std::vector<Entry> entries;
    std::vector<std::string> names;
    if (!readEntries(dir, entries)) return names;
    for (const Entry& e : entries)
        if (e.isDir) names.push_back(e.name);
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace crosspad
