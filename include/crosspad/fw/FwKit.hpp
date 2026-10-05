#pragma once
#include <cstddef>

namespace crosspad {

/// CPFI kit_id is char[16]; release files are CrossPad-<tag>-<rev>-<kit>.bin.
constexpr size_t kFwKitMax = 16;
/// a-z first, then a-z 0-9 _, at most 15 characters.
bool fwKitIdValid(const char* kit);

/// Kits on the beta channel, one space-separated string (NVS fwupd/beta_kits).
constexpr size_t kFwBetaListMax = 256;
bool fwBetaListHas(const char* list, const char* kit);
/// False (list untouched) for an invalid kit or when the result does not fit `cap`.
bool fwBetaListSet(char* list, size_t cap, const char* kit, bool beta);

} // namespace crosspad
