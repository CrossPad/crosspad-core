// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/platform/IImu.hpp"

namespace crosspad {

/**
 * @brief No-op IImu stub for platforms without IMU hardware.
 *
 * Used as the default singleton on platforms that lack an IMU
 * (e.g. the PC simulator).
 */
class NullImu : public IImu {
public:
    bool isAvailable() const override { return false; }
    bool getData(ImuData&) const override { return false; }
    Orientation getOrientation() const override { return Orientation::Unknown; }
    bool isShaking() const override { return false; }
    bool isFreeFall() const override { return false; }
};

} // namespace crosspad
