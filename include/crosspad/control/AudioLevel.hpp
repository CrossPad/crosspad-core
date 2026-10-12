// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file AudioLevel.hpp
 * @brief The output-level verb (board AUDIO_LEVEL, simulator audio_level),
 *        gathered once.
 */

#include <cstdint>

namespace crosspad::control {

struct AudioLevel {
    uint8_t stream = 0;
    float   left   = 0.0f;   ///< output peak the audio module reports, [0, 1]
    float   right  = 0.0f;
};

/// The audio module's output level of @p stream.
AudioLevel readAudioLevel(uint8_t stream);

} // namespace crosspad::control
