// SPDX-License-Identifier: GPL-3.0-or-later
//
// What "Make a copy" of a shared element takes for one song: which elements are
// copied and which are re-pointed in place, read from the reverse index alone
// (ISongEngine::usesIndex()). Pure: no card, no engine, so the cascade is tested
// on its own and ElementNav and the worker only carry it out.
#pragma once

#include "crosspad/song/ISongEngine.hpp"

#include <map>
#include <string>
#include <vector>

namespace crosspad {

/// What a song needs so that it alone hears an element's new version: every
/// element of the song's tree that plays a ref going away either is copied --
/// something outside the song hears it too, and must keep the original -- or,
/// heard only through the song, is re-pointed where it is. A copy is itself a
/// ref going away for the elements that play it, so the copies go up the tree
/// as far as the song.
struct ElementCopyPlan {
    /// Elements to copy, each once: the seeds first, then in the order found.
    std::vector<std::string> forks;
    /// Elements heard only through the song that play a ref going away: their
    /// blocks take the copy where they are.
    std::vector<std::string> rewrites;
    /// The song's own blocks play a ref going away.
    bool song = false;
};

/// @p song: the project the copies are for. @p uses: who plays each element.
/// @p seeds: elements to copy now (the one just edited), each copied whatever
/// @p copied says;
/// @p copied: elements already copied for the song (their copies are what it
/// plays from now on; the originals, which other users keep, are not touched).
ElementCopyPlan plan_element_copies(const std::string& song, const ElementUses& uses,
                                    const std::vector<std::string>& seeds,
                                    const std::vector<std::string>& copied);

/// A copy's title that none in @p taken has: "REFREN" gives "REFREN 2", "REFREN 2"
/// gives "REFREN 3"; an untitled element's are counted from "ELEMENT".
std::string element_copy_title(const std::string& title, const std::vector<std::string>& taken);

} // namespace crosspad
