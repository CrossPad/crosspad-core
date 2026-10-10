// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/sequencer/PatternSequencer.hpp"

#include <string>

namespace crosspad {

/// What SequenceStore::open() found. Unreadable is never "no scenes": a file
/// that is there but cannot be read holds somebody's grooves, and treating it
/// as empty lets the next save write over them.
enum class SequenceLoad : uint8_t { Absent, Loaded, Unreadable };

/// The sequencer's scenes on the card, as `sequences.json` (where it lives is
/// the platform's call; the board keeps one for every kit).
///
/// Never inside a `kit.json`: kit.json is written by tools and describes the
/// samples, this is user data the device rewrites every time a take is closed.
/// Keeping them apart means a failed sequence write can never cost somebody
/// their kit.
///
/// The song arrangement travels in the same file, under "song": its loop
/// switch and its blocks as [scene, repeats] pairs. A file without it is an
/// empty arrangement; an older firmware reading a newer file ignores it.
///
/// Positions are stored in ticks (`PatternSequencer::kPpq` per quarter), never
/// in samples -- a pattern has to survive being loaded at another tempo or on a
/// board running a different sample rate, and a sample offset does not. The
/// file also carries the bpm it was played at, so the scenes come back at the
/// tempo they were recorded.
///
/// Portable: stdio and ArduinoJson, the same pair PortableKitLoader uses, so
/// the PC simulator reads and writes the same files.
class SequenceStore {
public:
    /// File format version written into every file. Bumped only when a reader
    /// would otherwise misread an older file.
    static constexpr int kVersion = 1;

    /// The conventional name.
    static const char* fileName() { return "sequences.json"; }

    /// Writes every scene that has events, through `.tmp` and `.bak` so that
    /// there is never a moment with no complete file. Returns false if the
    /// file could not be written; the sequencer's dirty flag is left alone either way, so the
    /// caller decides whether a failed write is worth retrying.
    static bool save(const PatternSequencer& seq, const std::string& path);

    /// Replaces every scene with what the file holds: the file, else what a
    /// save cut short leaves (the new one complete as `.tmp`, or the old one
    /// moved aside as `.bak`). The sequencer is untouched unless Loaded.
    static SequenceLoad open(PatternSequencer& seq, const std::string& path);

    /// open() == Loaded.
    static bool load(PatternSequencer& seq, const std::string& path);
};

} // namespace crosspad
