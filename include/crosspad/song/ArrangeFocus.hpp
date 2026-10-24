// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/song/AudioSong.hpp"

#include <cstdint>

namespace crosspad {

/// Which Arrange track the screen now open is filling.
///
/// Arrange is where a song starts, and some of its work is done on another
/// screen: CLIP EDIT opens an audio block in the Recorder and a seq block in
/// the sequencer on that track's scene, a Pitch track with no instrument
/// opens Pitch Play, and a seq track's EDIT opens the Sampler or Pitch Play
/// on the sound it plays. Each of those screens is the app that already does
/// that job -- they only need to know, while they are up, which track sent
/// them and how to get back.
///
/// Set by Arrange immediately before it hands over, cleared when the way back
/// is taken or when one of those apps is opened from the MODE map instead.
/// @c track == @c kNone means nobody handed anything over, and every screen
/// then behaves as it does when opened on its own.
struct ArrangeFocus {
    static constexpr int kNone = -1;

    int     track = kNone;        ///< 0 .. AudioSong::kTracks - 1
    bool    seq   = false;        ///< a seq track (sequencer) rather than audio (recorder)
    SeqSource source = SeqSource::Kit;   ///< what a seq track plays through
    uint8_t scene = 0;            ///< the scene a seq track plays
    /// The seq view's BOUNCE: Arrange opens on "Bounce T<n> to audio?" for
    /// this track, the dialog over the arrangement it changes.
    bool    bounce = false;
    /// Where Arrange's playhead stood, 0-based: an audio track's take, with
    /// the song, starts on this bar.
    uint32_t bar = 0;

    bool held() const { return track >= 0 && track < AudioSong::kTracks; }
    void clear() { *this = ArrangeFocus{}; }
};

/// The one focus. Header-only on purpose: an inline function's local static is
/// one object across every translation unit, so this needs no source file --
/// and no new entry in library.json or a CMake re-configure to be found.
inline ArrangeFocus& arrangeFocus()
{
    static ArrangeFocus focus;
    return focus;
}

} // namespace crosspad
