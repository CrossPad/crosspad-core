// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/audio/AbstractAudioModule.hpp"

namespace crosspad {

/**
 * @brief No-op audio module — used as default in PlatformServices until a
 *        real platform module is registered via setAudioModule().
 *
 * process() runs the node chain but discards output (no streams). Safe to
 * call from any thread; safe to keep nodes registered against it.
 */
class NullAudioModule : public AbstractAudioModule {
public:
    IAudioStream* getOutputStream(uint8_t /*index*/) override { return nullptr; }
    IAudioStream* getInputStream(uint8_t /*index*/) override { return nullptr; }
};

} // namespace crosspad
