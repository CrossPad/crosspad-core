// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/control/AudioLevel.hpp"

#include "crosspad/audio/IAudioModule.hpp"
#include "crosspad/platform/PlatformServices.hpp"

namespace crosspad::control {

AudioLevel readAudioLevel(uint8_t stream) {
    AudioLevel lv;
    lv.stream = stream;
    getAudioModule().getOutputLevel(stream, lv.left, lv.right);
    return lv;
}

} // namespace crosspad::control
