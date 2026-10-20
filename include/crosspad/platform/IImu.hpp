// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace crosspad {

/**
 * @brief Portable IMU data snapshot
 *
 * Read-only struct returned by IImu::getData().
 */
struct ImuData {
    float accel_x = 0;      ///< Acceleration X-axis (g)
    float accel_y = 0;      ///< Acceleration Y-axis (g)
    float accel_z = 0;      ///< Acceleration Z-axis (g)
    float gyro_x = 0;       ///< Gyroscope X-axis (dps)
    float gyro_y = 0;       ///< Gyroscope Y-axis (dps)
    float gyro_z = 0;       ///< Gyroscope Z-axis (dps)
    float temperature = 0;  ///< Temperature (Celsius)
    float pitch = 0;        ///< Fused pitch angle (degrees), Kalman-filtered where available
    float roll = 0;         ///< Fused roll angle (degrees), Kalman-filtered where available
};

/**
 * @brief Coarse device orientation derived from accelerometer
 */
enum class Orientation : uint8_t {
    FaceUp,
    FaceDown,
    LandscapeLeft,
    LandscapeRight,
    PortraitUp,
    PortraitDown,
    Unknown
};

/**
 * @brief Abstract IMU interface for portable motion data access
 *
 * On CrossPad the data comes from the board's ST LSM6DS3TR-C motion sensor, read
 * by the platform's IMU task and fused with a Kalman filter. A platform
 * without a sensor provides NullImu, which reports isAvailable() false.
 *
 * The interface does not manage initialization or background tasks; those
 * stay in the platform layer. It is read-only access to the data the
 * platform's IMU task produces. Apps that only need gestures (tap, double
 * tap, free fall) subscribe to EventType::MotionGesture instead.
 */
class IImu {
public:
    virtual ~IImu() = default;

    /// Whether IMU hardware is present and producing data
    virtual bool isAvailable() const = 0;

    /// Copy the latest IMU snapshot into @p out (thread-safe; the data is
    /// cached by the platform's IMU task). Returns false, leaving @p out
    /// unchanged, when no data is available.
    virtual bool getData(ImuData& out) const = 0;

    /// Coarse orientation derived from accelerometer
    virtual Orientation getOrientation() const = 0;

    /// Detect sudden shake (gyro magnitude above threshold)
    virtual bool isShaking() const = 0;

    /// Detect free-fall condition (acceleration near zero)
    virtual bool isFreeFall() const = 0;
};

/// The platform's IMU; never null (NullImu where the board has no sensor).
IImu& getImu();

} // namespace crosspad
