// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/control/WaveStatus.hpp"

#include <cstdio>
#include <string>

#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/platform/PlatformServices.hpp"

namespace crosspad::control {

namespace {
/// Room a "pad:layer" entry needs beside its comma and the terminator.
constexpr size_t kMissingEntryChars = 8;
}

void readWaveCoverage(WaveStatus& out, bool (*isCached)(const char* path)) {
    IKitManager* mgr = getKitManager();
    KitInfo* kit = mgr ? mgr->getCurrentKit() : nullptr;
    out.kit = mgr ? mgr->getCurrentKitId() : -1;
    out.total = 0;
    out.cached = 0;
    out.missing[0] = '\0';
    if (!kit) return;
    size_t m = 0;
    for (int i = 0; i < KIT_PADS; ++i) {
        int layer = 0;
        for (const auto& sample : kit->pads[i].samples) {
            if (sample.sample.empty()) { ++layer; continue; }
            ++out.total;
            bool hit;
            if (sample.sample[0] == '/') {
                hit = isCached(sample.sample.c_str());
            } else {
                const std::string dir = kit->path.substr(0, kit->path.rfind('/'));
                hit = isCached((dir + "/" + sample.sample).c_str())
                   || isCached((dir + "/SAMPLES/" + sample.sample).c_str());
            }
            if (hit) {
                ++out.cached;
            } else if (m < sizeof(out.missing) - kMissingEntryChars) {
                const int r = snprintf(out.missing + m, sizeof(out.missing) - m, "%s%d:%d",
                                       m ? "," : "", i, layer);
                /* A cut entry ends the list where the buffer does. */
                if (r > 0) m = m + (size_t)r < sizeof(out.missing) ? m + (size_t)r : sizeof(out.missing) - 1;
            }
            ++layer;
        }
    }
}

} // namespace crosspad::control
