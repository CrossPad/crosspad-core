#pragma once
#include "crosspad/settings/AudioEngineSettings.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/settings/SettingsIo.hpp"

namespace crosspad {

/// What belongs to the device rather than to a firmware: pad feel and STM32
/// power behaviour, the panel, the USB and radio choices a bench depends on,
/// and the clock the kits were made for. A per-firmware import never moves these.
struct SharedSettingsSnapshot {
    Stm32Settings stm;
    uint8_t LCDbrightness;
    bool usbAudioEnabled;
    bool usbSerialGuard;
    bool enableBLE;
    decltype(AudioEngineSettings::sampleRate) sampleRate;
};

SharedSettingsSnapshot captureSharedSettings(const CrosspadSettings& s);
void restoreSharedSettings(CrosspadSettings& s, const SharedSettingsSnapshot& snap);

/// settingsImportAll() with the shared settings put back afterwards.
SettingsIoResult settingsImportPerFirmware(CrosspadSettings& s, const char* path);

} // namespace crosspad
