// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace crosspad {

/**
 * @brief When and how a song's Pitch sound that would not load is told to the
 * player -- one rule for everyone who asks the instrument host for it
 * (SongSounds as a song opens, Arrange's sound keeper while it is open).
 *
 * A Pitch instrument built beside the kits keeps 1.5 MB of PSRAM back, so a
 * first try often fails only while a bounce (Arrange's auto-freeze) holds the
 * memory, and the retry 2 s later takes. The player hears of it only once the
 * retry has failed too, and in their words.
 */
struct InstrumentLoadNote {
    /// Failed tries before the player is told.
    static constexpr uint8_t kSayAfter = 2;

    /// A failure the player can do something about by giving memory back
    /// (PitchedKitLoader's "no memory", "budget ... exhausted").
    static bool forMemory(const std::string& why)
    {
        return why.find("no memory") != std::string::npos || why.find("budget") != std::string::npos;
    }

    /// What the player reads for @p why (IInstrumentHost::message()).
    static void text(const std::string& why, char* out, size_t outLen)
    {
        if (forMemory(why))
            std::snprintf(out, outLen, "Pitch sound needs more memory: remove a track");
        else
            std::snprintf(out, outLen, "Pitch sound did not load: %s", why.c_str());
    }
};

} // namespace crosspad
