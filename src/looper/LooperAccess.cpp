// SPDX-License-Identifier: MIT
#include "crosspad/looper/LooperAccess.hpp"

namespace crosspad {

namespace {
ILooper* g_looper = nullptr;
}

ILooper* getLooper() { return g_looper; }
void setLooper(ILooper* looper) { g_looper = looper; }

} // namespace crosspad
