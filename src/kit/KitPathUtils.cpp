// SPDX-License-Identifier: GPL-3.0-or-later
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/platform/IFileSystem.hpp>

namespace crosspad {

namespace {

/* Callers pass logical paths, but choosing between the two layouts means
 * asking the filesystem, which needs the mount prefix. */
std::string g_fsPrefix;

bool pathExists(const std::string& path) {
    return getFileSystem().exists(path.c_str());
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

std::string kitFsPath(const std::string& logicalPath) {
    return logicalPath.empty() ? std::string() : g_fsPrefix + logicalPath;
}

std::string renderedSamplePath(const std::string& logicalPath, const SampleFx& fx) {
    if (!fx.any() || logicalPath.empty()) return logicalPath;
    const auto slash = logicalPath.rfind('/');
    const std::string dir  = slash != std::string::npos ? logicalPath.substr(0, slash) : "";
    std::string name = slash != std::string::npos ? logicalPath.substr(slash + 1) : logicalPath;
    const auto dot = name.rfind('.');
    if (dot != std::string::npos) name.resize(dot);
    if (fx.semitones) name += (fx.semitones > 0 ? "_p+" : "_p") + std::to_string(fx.semitones);
    if (fx.hpHz) name += "_h" + std::to_string(fx.hpHz);
    if (fx.lpHz) name += "_l" + std::to_string(fx.lpHz);
    if (fx.reverse) name += "_r";
    return dir + "/.fx/" + name + ".wav";
}

std::string kitAssetPath(const std::string& kitJsonPath,
                         const std::string& fileName) {
    return g_fsPrefix + parentDir(kitJsonPath) + "/" + fileName;
}

} // namespace crosspad
