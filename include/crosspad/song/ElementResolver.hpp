// SPDX-License-Identifier: GPL-3.0-or-later
//
// What each element of a song is NOW: its bake's key, named bottom-up, so a
// change deep in a nest changes the key of every element above it, and whether
// that bake is on the card or could be made from files already there.
#pragma once

#include "crosspad/song/ElementHash.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace crosspad {

/// One element as the resolver found it.
struct ResolvedElement {
    std::string ref;
    uint64_t    key = 0;
    std::string file;            ///< the bake's name in the cache (element_bake_file(key))
    uint32_t    frames = 0;      ///< one pass of it at the resolver's tempo and rate
    bool        exists = false;  ///< its bake is on the card
    bool        bakeable = false; ///< every heard seq track is frozen to files that are there and every child exists or is bakeable
    std::string why;             ///< the first thing that makes it not bakeable; empty when it is
};

/// Worker only: it reads song.json files and asks the card about every file a
/// key is made of. Each element is resolved once, children first; a nest that
/// leads back to itself is reported, not followed.
class ElementResolver {
public:
    /// Elements are baked at @p bpm and @p sampleRate, whatever tempo their own
    /// song.json holds (the bake key says the same). @p rd is how the hash and the
    /// existence checks look at the card; its childKey is the resolver's own.
    ElementResolver(std::string songsRoot, uint32_t sampleRate, float bpm, ElementHashReader rd);

    /// @p ref's element: <songsRoot>/<ref> is its project folder. The answer
    /// stays valid as long as the resolver.
    const ResolvedElement& resolve(const std::string& ref);

private:
    ResolvedElement make(const std::string& ref);

    std::string       root_;
    uint32_t          rate_;
    float             bpm_;
    ElementHashReader rd_;
    std::map<std::string, ResolvedElement> done_;
    std::set<std::string> open_;   ///< being resolved: meeting one again is a cycle
    ResolvedElement cycle_;        ///< the answer for the last element met again
};

} // namespace crosspad
