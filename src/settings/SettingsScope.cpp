#include "crosspad/settings/SettingsScope.hpp"

namespace crosspad {

SharedSettingsSnapshot captureSharedSettings(const CrosspadSettings& s) {
    return { s.stm, s.LCDbrightness, s.usbAudioEnabled, s.usbSerialGuard,
             s.wireless.enableBLE, s.audioEngine.sampleRate };
}

void restoreSharedSettings(CrosspadSettings& s, const SharedSettingsSnapshot& snap) {
    s.stm = snap.stm;
    s.LCDbrightness = snap.LCDbrightness;
    s.usbAudioEnabled = snap.usbAudioEnabled;
    s.usbSerialGuard = snap.usbSerialGuard;
    s.wireless.enableBLE = snap.enableBLE;
    s.audioEngine.sampleRate = snap.sampleRate;
}

SettingsIoResult settingsImportPerFirmware(CrosspadSettings& s, const char* path) {
    SharedSettingsSnapshot snap = captureSharedSettings(s);
    SettingsIoResult res = settingsImportAll(s, path);
    if (res.ok) restoreSharedSettings(s, snap);
    return res;
}

} // namespace crosspad
