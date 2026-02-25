#include "crosspad/platform/CrosspadPlatformInit.hpp"

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

// Cached references for crosspad_platform_set_midi()
static PadManager*       s_padManager       = nullptr;
static PadLedController* s_padLedController = nullptr;
static PadAnimator*      s_padAnimator      = nullptr;
static IEventBus*        s_eventBus         = nullptr;
static CrosspadSettings* s_settings         = nullptr;
static CrosspadStatus*   s_status           = nullptr;
static bool              s_initialized      = false;

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

    // Cache for crosspad_platform_set_midi()
    s_padManager       = config.padManager;
    s_padLedController = config.padLedController;
    s_padAnimator      = config.padAnimator;
    s_eventBus         = config.eventBus;
    s_settings         = settings;
    s_status           = status;
    s_initialized      = true;

    return true;
}

void crosspad_platform_set_midi(IMidiOutput& midi) {
    if (!s_padManager || !s_padLedController || !s_padAnimator || !s_eventBus || !s_settings) {
        return;
    }
    s_padManager->init(*s_padLedController, *s_padAnimator, midi,
                       *s_eventBus, s_settings, s_status);
    s_padManager->begin();
}

} // namespace crosspad
