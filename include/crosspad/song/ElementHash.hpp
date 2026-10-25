// SPDX-License-Identifier: GPL-3.0-or-later
//
// What an element's bake is named by: a hash of everything that is heard of
// the song, and of the tempo and rate it is mixed down at. Equal keys are the
// same sound, so a bake already on the card is found, not written again.
#pragma once

#include "crosspad/song/AudioSong.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace crosspad {

/// Bumped when the mix-down changes what it writes for the same song.
constexpr uint32_t kRenderVersion = 1;

/// How the hash looks at the card: the engine passes its file system, a test a
/// fake. A file it cannot find hashes as missing, which is not the same as
/// empty.
struct ElementHashReader {
    /// @p path's size in bytes and when it was last written.
    std::function<bool(const std::string& path, uint64_t& bytes, int64_t& modified)> info;
    /// @p path's contents.
    std::function<bool(const std::string& path, std::string& bytes)> read;
    /// @p each(path) for every file directly in @p dir, @p path in full. Unset
    /// leaves a kit's or an instrument's folder out of the hash.
    std::function<void(const std::string& dir, const std::function<void(const std::string& path)>& each)> list;
    /// The key of the bake @p ref plays now (ElementResolver). Set, an element
    /// block is hashed by it, so a change deep in a nest reaches every element
    /// above; unset, by the bake name the block stores.
    std::function<bool(const std::string& ref, uint64_t& key)> childKey;
};

/// FNV-1a 64 over whose it is -- @p identity: the element's ref
/// (.elements/E%08X), a song's project name -- and what @p song plays, each
/// track that is heard in order: its
/// kind, volume, pan and stretch; each block's file (an audio block's with its
/// size and time on the card, an element's by the key of the bake it plays,
/// or with no childKey the one it stores), place, cut
/// and notes; a seq track's freeze (files and sizes). Then the length and
/// metre, and the kit and the instrument with every file in their folders.
/// A silenced track gives its index and a marker. Not the tempo, the title,
/// the armed track or the launcher's sets: those are no part of the sound
/// (the tempo is in element_bake_key()). The identity keeps a key to one
/// folder: a recording is told apart by its name, size and time, and two
/// folders can hold different takes alike in all three -- the same element in
/// many songs still shares its bakes, a copy of it bakes its own once.
uint64_t element_content_hash(const AudioSong& song, const std::string& identity, const ElementHashReader& rd);

/// What the player has edited of @p song, and nothing the engine derives:
/// each track's kind, source, scene, levels, pan, mute, solo, input and kit;
/// each block's file (an element block by its ref), place, cut and notes; the
/// length, the metre, the kit's and the instrument's names; and @p scenes, the
/// project's sequences.json less the tempo it was last saved at and the scene
/// left selected (the store rewrites both when the clock moves or a scene is
/// looked at). Not the tempo, the stretch modes, the
/// freezes or the copies stretched to a tempo, which ElementNav's enter and
/// leave rewrite on an element nobody touched. Memory and @p scenes only: it
/// reads no file, so it is the same for the same edits however the card is.
uint64_t element_edit_hash(const AudioSong& song, const std::string& scenes);

/// The bake's key: @p content at @p bpm and @p sampleRate under kRenderVersion.
uint64_t element_bake_key(uint64_t content, float bpm, uint32_t sampleRate);

/// "B%016llX.wav", the bake's name in the cache.
std::string element_bake_file(uint64_t key);

/// A context stem's key: @p content, the song's content hash with the open
/// element silent wherever it plays, over [@p from, @p from + @p frames) of
/// the song, with @p lacking -- every thing it should hold and was made
/// without, or played from an older bake; empty when whole -- at @p bpm and
/// @p sampleRate under kRenderVersion. A stem made short of something is
/// another stem from the one made when that thing is there.
uint64_t context_bake_key(uint64_t content, uint32_t from, uint32_t frames, const std::string& lacking,
                          float bpm, uint32_t sampleRate);

/// "C%016llX.wav", a context stem's name in the cache.
std::string context_bake_file(uint64_t key);

} // namespace crosspad
