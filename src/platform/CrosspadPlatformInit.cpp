#include "crosspad/platform/CrosspadPlatformInit.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/platform/PlatformCapabilities.hpp"

#include "crosspad/event/IEventBus.hpp"
#include "crosspad/platform/IClock.hpp"
#include "crosspad/midi/IMidiOutput.hpp"
#include "crosspad/led/ILedStrip.hpp"
#include "crosspad/settings/IKeyValueStore.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/status/CrosspadStatus.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/pad/PadLedController.hpp"
#include "crosspad/pad/PadAnimator.hpp"

namespace crosspad {

static bool s_initialized = false;

bool crosspad_platform_init(const CrosspadPlatformConfig& config) {
    if (s_initialized) return true;

    // Validate required fields
    if (!config.eventBus || !config.clock || !config.ledStrip || !config.midiOutput ||
        !config.padManager || !config.padLedController || !config.padAnimator) {
        return false;
    }

    // 1. Event bus
    config.eventBus->init();

    // 2. Settings
    CrosspadSettings* settings = config.settings
        ? config.settings
        : CrosspadSettings::getInstance();

    // 3. Key-value store — load/save settings (if provided)
    if (config.kvStore) {
        config.kvStore->init();
        settings->loadFrom(*config.kvStore);
        settings->saveTo(*config.kvStore);  // persist defaults on first run
    }

    // 4. Status
    static CrosspadStatus defaultStatus;
    CrosspadStatus* status = config.status ? config.status : &defaultStatus;

    // 5. Pad subsystem
    config.padAnimator->init(*config.ledStrip, *config.clock);
    config.padLedController->init(*config.ledStrip, *config.eventBus, settings, config.padCount);
    config.padManager->init(*config.padLedController, *config.padAnimator,
                            *config.midiOutput, *config.eventBus, settings, status);

    // 6. Start
    config.padManager->begin();
    config.padLedController->begin();

    // 7. Populate PlatformServices registry
    auto& svc = getPlatformServices();
    svc.eventBus        = config.eventBus;
    svc.clock           = config.clock;
    svc.ledStrip        = config.ledStrip;
    svc.midiOutput      = config.midiOutput;
    svc.padManager      = config.padManager;
    svc.padLedController = config.padLedController;
    svc.padAnimator     = config.padAnimator;
    svc.kvStore         = config.kvStore;
    svc.settings        = settings;
    svc.status          = status;

    s_initialized = true;

    return true;
}

void crosspad_platform_set_midi(IMidiOutput& midi) {
    getPlatformServices().setMidiOutput(&midi);
}

void crosspad_platform_reset() {
    s_initialized = false;
    auto& svc = getPlatformServices();
    svc = PlatformServices{};
    svc.setFileSystem(nullptr);
    setPlatformCapabilities(Capability::None);
}

} // namespace crosspad
