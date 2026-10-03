// SPDX-License-Identifier: MIT
#pragma once

#include "crosspad/looper/ILooper.hpp"

namespace crosspad {

/// The looper the platform runs, or nullptr where it runs none -- published
/// on its own rather than through PlatformServices, so a platform can add
/// looping without the registry (and every caller has to check anyway).
ILooper* getLooper();
void setLooper(ILooper* looper);

} // namespace crosspad
