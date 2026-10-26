#pragma once

#include <algorithm>
#include <string>

namespace crosspad {

/// The instruments of the library (IInstrumentHost::libraryRoot()) are named
/// "<GROUP>_<name>" -- "808_Bright", "BASS_Reese", "PAD_Warm" -- and the lists
/// that offer them (Pitch Play's, the kit browser's PITCH) show the groups
/// first, then the one picked.
struct InstrumentGroups {
    /// What a name without a group is filed under.
    static constexpr const char* kOther = "OTHER";

    /// @p name's group: up to its first '_', kOther without one.
    static std::string group(const std::string& name) {
        const size_t u = name.find('_');
        return u == std::string::npos || u == 0 ? std::string(kOther) : name.substr(0, u);
    }

    /// @p name inside its group: the group left off, every '_' a space.
    static std::string shortName(const std::string& name) {
        const size_t u = name.find('_');
        std::string n = u == std::string::npos || u == 0 || u + 1 >= name.size() ? name : name.substr(u + 1);
        std::replace(n.begin(), n.end(), '_', ' ');
        return n;
    }
};

} // namespace crosspad
