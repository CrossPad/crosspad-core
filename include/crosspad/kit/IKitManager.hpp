#pragma once
#include <cstdint>
#include <crosspad/kit/KitInfo.hpp>

namespace crosspad {

/**
 * @brief Abstract interface for kit management.
 *
 * Provides platform-independent kit enumeration and selection.
 * Platform implementations handle storage (SD card, filesystem, etc.)
 * and audio engine integration.
 */
class IKitManager {
public:
    virtual ~IKitManager() = default;

    virtual int      getKitCount() const = 0;
    virtual KitInfo* getKit(int id) = 0;
    virtual KitInfo* getCurrentKit() = 0;
    virtual int      getCurrentKitId() const = 0;
    virtual bool     isLoading() const = 0;
};

} // namespace crosspad
