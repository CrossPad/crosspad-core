// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file SettingsIo.hpp
 * @brief Settings export/import to a JSON file (SD card on the device, plain
 *        filesystem on the desktop).
 *
 * Complements the IKeyValueStore path in CrosspadSettings: NVS is the live
 * store, these files are the transportable backup — a user can copy one to
 * another unit or hand-edit it, so every import validates and clamps before
 * anything reaches the live settings.
 *
 * Two file kinds, each with its own envelope so a wrong file is rejected
 * instead of half-applied:
 *   {"kind":"crosspad-settings","version":1,"settings":{...}}
 *   {"kind":"crosspad-pads","version":1,"pads":{...}}
 */

#include <cstdint>
#include "crosspad/settings/CrosspadSettings.hpp"

namespace crosspad {

struct SettingsIoResult {
    bool ok = false;
    uint16_t version = 0;        ///< version found in the file (import) or written (export)
    const char* error = nullptr; ///< static string, nullptr when ok
};

/// 2: masterFX carries only what NVS persists; there are no per-pad
///    `audio[]` and `previewAudio` sections. Version-1 files still import,
///    and those keys are ignored.
constexpr uint16_t kSettingsFileVersion = 2;

/// Whole-settings snapshot.
SettingsIoResult settingsExportAll(const CrosspadSettings& s, const char* path);
SettingsIoResult settingsImportAll(CrosspadSettings& s, const char* path);

/// Pad tuning only — importable independently of the general settings, so a
/// user can carry pad feel between units without dragging routing, audio and
/// wireless config along.
SettingsIoResult settingsExportPads(const CrosspadSettings& s, const char* path);
SettingsIoResult settingsImportPads(CrosspadSettings& s, const char* path);

} // namespace crosspad
