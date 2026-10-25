// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which kits were saved: a kit written under its own name sounds other than
// its path says, so whatever was rendered from it -- a seq track's freeze --
// has to hear of it. No card I/O, callable from any thread: saves come from
// the kit loader's and the instrument host's workers, the song's sounds
// look from the UI thread.
#pragma once

#include <cstdint>
#include <string>

namespace crosspad {

/// Kit.json @p kitJsonPath, as a song names its kit (from the card's root,
/// "/crosspad/kits/<folder>/kit.json"), was written.
void kit_saved_notify(const std::string& kitJsonPath);

/// How many times @p kitJsonPath was written since the program started;
/// compare two answers, a differing one is a save in between. 0 for a kit
/// never saved.
uint32_t kit_saved_generation(const std::string& kitJsonPath);

} // namespace crosspad
