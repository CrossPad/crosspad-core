#include "crosspad/platform/PlatformCapabilities.hpp"

namespace crosspad {

static Capability s_capabilities = Capability::None;

void setPlatformCapabilities(Capability caps) {
    s_capabilities = caps;
}

void addPlatformCapability(Capability cap) {
    s_capabilities |= cap;
}

void removePlatformCapability(Capability cap) {
    s_capabilities &= ~cap;
}

bool hasCapability(Capability cap) {
    return (s_capabilities & cap) == cap;
}

bool hasAnyCapability(Capability caps) {
    return (s_capabilities & caps) != Capability::None;
}

Capability getPlatformCapabilities() {
    return s_capabilities;
}

} // namespace crosspad
