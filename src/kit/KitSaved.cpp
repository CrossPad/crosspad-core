// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/kit/KitSaved.hpp"

#include <mutex>
#include <unordered_map>

namespace crosspad {

namespace {

std::mutex& lock() {
    static std::mutex m;
    return m;
}

/// A save count per kit.json: as many entries as kits ever saved.
std::unordered_map<std::string, uint32_t>& saves() {
    static std::unordered_map<std::string, uint32_t> s;
    return s;
}

} // namespace

void kit_saved_notify(const std::string& kitJsonPath) {
    std::lock_guard<std::mutex> l(lock());
    ++saves()[kitJsonPath];
}

uint32_t kit_saved_generation(const std::string& kitJsonPath) {
    std::lock_guard<std::mutex> l(lock());
    const auto it = saves().find(kitJsonPath);
    return it == saves().end() ? 0u : it->second;
}

} // namespace crosspad
