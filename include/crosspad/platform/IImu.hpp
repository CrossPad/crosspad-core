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
 * Platform implementations:
 *   - ESP-IDF: wraps bsp_imu_* (LSM6DSO + Kalman filter)
 *   - Arduino: stub returning isAvailable()=false (no IMU driver yet)
 *
 * The interface does NOT manage initialization or background tasks —
 * those remain in the platform layer. This is read-only access to data
 * that the platform's IMU task produces.
 */
class IImu {
public:
    virtual ~IImu() = default;

    /// Whether IMU hardware is present and producing data
    virtual bool isAvailable() const = 0;

    /// Get latest IMU data snapshot (thread-safe, returns cached data from background task)
    virtual bool getData(ImuData& out) const = 0;

    /// Coarse orientation derived from accelerometer
    virtual Orientation getOrientation() const = 0;

    /// Detect sudden shake (gyro magnitude above threshold)
    virtual bool isShaking() const = 0;

    /// Detect free-fall condition (acceleration near zero)
    virtual bool isFreeFall() const = 0;
};

/// Platform-injected singleton (same pattern as getClock())
IImu& getImu();

} // namespace crosspad
