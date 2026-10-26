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
    float       bpm = 0.0f;      ///< the tempo @c frames and the bake are at (the resolver's); 0: the song's it plays in
    bool        exists = false;  ///< its bake is on the card
    bool        bakeable = false; ///< every heard seq track is frozen to files that are there and every child exists or is bakeable
    bool        kept = false;    ///< resolved from its snapshot: an edit of it is not decided yet, and this is it as it was
    std::string why;             ///< the first thing that makes it not bakeable; empty when it is
    /// What keeps it from the card is its own and a rebake mends it -- the element
    /// entered and left: a track not frozen, frozen at another tempo, older than its
    /// notes or kit, a freeze gone, a recording not fitted. Not a cycle, a song.json
    /// that cannot be read, an edit undecided or a child's state.
    bool        rebakes = false;
};

/// Worker only: it reads song.json files and asks the card about every file a
/// key is made of. Each element is resolved once, children first; a nest that
/// leads back to itself is reported, not followed.
///
/// An element being edited holds a snapshot (AudioSong::snapshotFileName())
/// until copy or replace is decided, and its song.json is the edit: every
/// song but the one the edit is made for hears it as it was, from the snapshot.
class ElementResolver {
public:
    /// Elements are baked at @p bpm and @p sampleRate, whatever tempo their own
    /// song.json holds (the bake key says the same). @p rd is how the hash and the
    /// existence checks look at the card; its childKey is the resolver's own.
    /// @p heardDraft is the element whose edit the song resolved for hears
    /// (the one just left to come back to it): resolved from its song.json even
    /// with a snapshot beside it. @p editing is the element open in the engine,
    /// edited as this runs: never baked or linked from its song.json, only
    /// from its snapshot when it has one.
    ElementResolver(std::string songsRoot, uint32_t sampleRate, float bpm, ElementHashReader rd,
                    std::string heardDraft = {}, std::string editing = {});

    /// @p ref's element: <songsRoot>/<ref> is its project folder. The answer
    /// stays valid as long as the resolver.
    ElementResolver(const ElementResolver&) = delete;   // the hash's childKey points back at it
    ElementResolver& operator=(const ElementResolver&) = delete;

    const ResolvedElement& resolve(const std::string& ref);

    /// @p ref's song as this resolver hears it, into @p song: its snapshot
    /// while it holds one (@p kept set), else its song.json.
    SongLoad load(AudioSong& song, const std::string& ref, bool* kept = nullptr) const;

private:
    ResolvedElement make(const std::string& ref);

    std::string       root_;
    uint32_t          rate_;
    float             bpm_;
    ElementHashReader rd_;
    std::string       heardDraft_;
    std::string       editing_;
    std::map<std::string, ResolvedElement> done_;
    std::set<std::string> open_;   ///< being resolved: meeting one again is a cycle
    ResolvedElement cycle_;        ///< the answer for the last element met again
};

} // namespace crosspad
