// SPDX-License-Identifier: MIT
#include <crosspad/kit/KitPathUtils.hpp>

#include <sys/stat.h>

namespace crosspad {

namespace {

/* Callers pass logical paths, but choosing between the two layouts means
 * asking the filesystem, which needs the mount prefix. */
std::string g_fsPrefix;

bool pathExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

std::string parentDir(const std::string& path) {
    auto pos = path.rfind('/');
    return (pos != std::string::npos) ? path.substr(0, pos) : "";
}

} // namespace

void setKitPathFsPrefix(const std::string& prefix) { g_fsPrefix = prefix; }

std::string resolveKitSamplePath(const std::string& kitJsonPath,
                                 const std::string& sampleFile) {
    if (sampleFile.empty()) return "";
    if (sampleFile[0] == '/') return sampleFile;

    const std::string dir  = parentDir(kitJsonPath);
    const std::string flat = dir + "/" + sampleFile;
    if (pathExists(g_fsPrefix + flat)) return flat;

    const std::string nested = dir + "/SAMPLES/" + sampleFile;
    if (pathExists(g_fsPrefix + nested)) return nested;

    /* Neither exists: return the flat form, so the caller's "file not found"
     * still names where a flat kit would have kept it. */
    return flat;
}

std::string kitAssetPath(const std::string& kitJsonPath,
                         const std::string& fileName) {
    return g_fsPrefix + parentDir(kitJsonPath) + "/" + fileName;
}

} // namespace crosspad
