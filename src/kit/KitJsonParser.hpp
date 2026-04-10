// SPDX-License-Identifier: MIT
#pragma once

#include <ArduinoJson.h>
#include <crosspad/kit/KitInfo.hpp>

namespace crosspad {

/// Parse kit metadata (name, author, description, version, created_at, type).
/// Does NOT parse pads — call kit_parse_pads() separately for full parse.
bool kit_parse_metadata(KitInfo& kit, JsonDocument& doc);

/// Parse the pads array (16 pads, samples per pad).
/// Handles samples as plain strings or {sample, start, end} objects.
/// Sets kit.parsed = true on success.
bool kit_parse_pads(KitInfo& kit, JsonDocument& doc);

/// Serialize KitInfo into a JsonDocument (metadata + all pads).
bool kit_serialize(const KitInfo& kit, JsonDocument& doc);

} // namespace crosspad
