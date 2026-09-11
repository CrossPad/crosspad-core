// SPDX-License-Identifier: MIT
#pragma once

#include "crosspad/sequencer/PatternSequencer.hpp"

#include <string>

namespace crosspad {

/// A kit's scenes on the card, as `<kit dir>/sequences.json`.
///
/// Beside `kit.json` rather than inside it on purpose: kit.json is written by
/// tools and describes the samples, this is user data the device rewrites every
/// time a take is closed. Keeping them apart means a failed sequence write can
/// never cost somebody their kit.
///
/// Positions are stored in ticks (`PatternSequencer::kPpq` per quarter), never
/// in samples -- a pattern has to survive being loaded at another tempo or on a
/// board running a different sample rate, and a sample offset does not. The
/// file also carries the bpm it was played at, so a kit without `bpm` in
/// kit.json still comes back at the tempo it was recorded.
///
/// Portable: std::fstream and ArduinoJson, the same pair PortableKitLoader
/// uses, so the PC simulator reads and writes the same files.
class SequenceStore {
public:
    /// File format version written into every file. Bumped only when a reader
    /// would otherwise misread an older file.
    static constexpr int kVersion = 1;

    /// The conventional name, next to kit.json.
    static const char* fileName() { return "sequences.json"; }

    /// Writes every scene that has events. Returns false if the file could not
    /// be opened; the sequencer's dirty flag is left alone either way, so the
    /// caller decides whether a failed write is worth retrying.
    static bool save(const PatternSequencer& seq, const std::string& path);

    /// Replaces every scene with what the file holds. Returns false when the
    /// file is absent or unreadable, which is the normal case for a kit nobody
    /// has recorded into -- the caller should treat it as "no scenes yet",
    /// not as an error.
    static bool load(PatternSequencer& seq, const std::string& path);
};

} // namespace crosspad
