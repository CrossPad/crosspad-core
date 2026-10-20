// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/platform/IFileSystem.hpp"

#include <utility>
#include <vector>

namespace crosspad {

bool IFileSystem::removeTree(const std::string& dir, void (*afterEntry)(void*), void* ctx)
{
    std::vector<std::pair<std::string, bool>> entries;
    forEach(dir, [&](const char* name, bool isDir) { entries.emplace_back(name, isDir); });
    bool ok = true;
    for (const auto& e : entries) {
        const std::string path = dir + "/" + e.first;
        if (e.second) ok = removeTree(path, afterEntry, ctx) && ok;
        else          ok = remove(path.c_str()) && ok;
        if (afterEntry) afterEntry(ctx);
    }
    return removeDir(dir.c_str()) && ok;
}

} // namespace crosspad
