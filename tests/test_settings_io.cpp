// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"
#include "crosspad/settings/SettingsIo.hpp"

#include <cstdio>
#include <string>

using namespace crosspad;

namespace {

// Files land in the test working directory and are removed with the fixture —
// no /sdcard, no fixed path that a parallel ctest run could share.
struct TempFile {
    explicit TempFile(const char* name) : path(name) { remove(path.c_str()); }
    ~TempFile() { remove(path.c_str()); }
    const char* c_str() const { return path.c_str(); }
    std::string path;
};

void writeRaw(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    REQUIRE(f != nullptr);
    fputs(text, f);
    fclose(f);
}

/// Copy of the live settings — CrosspadSettings' constructor is private, and
/// comparing against a snapshot keeps these tests independent of whatever the
/// other test files did to the singleton.
CrosspadSettings snapshot() {
    return *CrosspadSettings::getInstance();
}

} // namespace

TEST_CASE("SettingsIo: full settings round-trip", "[settings][io]") {
    TempFile file("cp_io_all.json");

    CrosspadSettings src = snapshot();
    src.keypad.enableKeypad = false;
    src.keypad.noteOffOnRelease = true;
    src.keypad.upperRowFunctions = true;
    src.stmRouting.Midi2ESP = true;
    src.stmRouting.ESP2USB = false;
    src.esp32Routing.BLE2Audio = false;
    src.wireless.enableWiFi = true;
    src.wireless.bleMidiMode = 1;
    src.wireless.bleMidiNoteOffset = -12;
    src.masterFX.mute = true;
    src.masterFX.outVolume = 77;
    src.masterFX.inVolume = 21;
    src.masterFX.speakersEnabled = false;
    src.masterFX.outVolumeCodec[1][0] = 31;
    src.masterFX.inVolumeCodec[0][1] = 9;
    src.masterFX.reverb.bypass = false;
    src.masterFX.flanger.bypass = false;
    src.audioEngine.sampleRate = 96000;
    src.audioEngine.frameCount = 128;
    src.audioEngine.queueFrames = 192;
    src.vibration.enable = false;
    src.vibration.outputMax = 199;
    src.stm.midiChannel = 9;
    src.stm.noteOffset = -5;
    src.stm.dimTimeoutS = 45;
    src.stm.velFloorDef = 7;
    src.stm.sensitivity(0) = 2048;
    src.stm.lrRatio(0) = 60;
    src.LCDbrightness = 42;
    src.themeColorIndex = 3;
    src.perfStatsFlags = 5;
    src.RGBbrightness = 66;
    src.quickSettingsEnabled = false;
    src.AudioEngineEnabled = false;
    src.usbAudioEnabled = true;
    src.usbSerialGuard = false;
    src.Kit = 4;
    src.launcherStyle = LauncherStyle::Grid;
    src.touchSensitivity = TouchSensitivity::High;

    SettingsIoResult wrote = settingsExportAll(src, file.c_str());
    REQUIRE(wrote.ok);
    REQUIRE(wrote.error == nullptr);
    REQUIRE(wrote.version == kSettingsFileVersion);

    CrosspadSettings dst = snapshot();
    SettingsIoResult read = settingsImportAll(dst, file.c_str());
    REQUIRE(read.ok);
    REQUIRE(read.version == kSettingsFileVersion);

    REQUIRE(dst.keypad.enableKeypad == false);
    REQUIRE(dst.launcherStyle == LauncherStyle::Grid);
    REQUIRE(dst.touchSensitivity == TouchSensitivity::High);
    REQUIRE(dst.keypad.noteOffOnRelease == true);
    REQUIRE(dst.keypad.upperRowFunctions == true);
    REQUIRE(dst.stmRouting.Midi2ESP == true);
    REQUIRE(dst.stmRouting.ESP2USB == false);
    REQUIRE(dst.esp32Routing.BLE2Audio == false);
    REQUIRE(dst.wireless.enableWiFi == true);
    REQUIRE(dst.wireless.bleMidiMode == 1);
    REQUIRE(dst.wireless.bleMidiNoteOffset == -12);
    REQUIRE(dst.masterFX.mute == true);
    REQUIRE(dst.masterFX.outVolume == 77);
    REQUIRE(dst.masterFX.inVolume == 21);
    REQUIRE(dst.masterFX.speakersEnabled == false);
    REQUIRE(dst.masterFX.outVolumeCodec[1][0] == 31);
    REQUIRE(dst.masterFX.inVolumeCodec[0][1] == 9);
    REQUIRE(dst.masterFX.reverb.bypass == false);
    REQUIRE(dst.masterFX.flanger.bypass == false);
    REQUIRE(dst.masterFX.delay.bypass == true);
    REQUIRE(dst.audioEngine.sampleRate == 96000);
    REQUIRE(dst.audioEngine.frameCount == 128);
    REQUIRE(dst.audioEngine.queueFrames == 192);
    REQUIRE(dst.vibration.enable == false);
    REQUIRE(dst.vibration.outputMax == 199);
    REQUIRE(dst.stm.midiChannel == 9);
    REQUIRE(dst.stm.noteOffset == -5);
    REQUIRE(dst.stm.dimTimeoutS == 45);
    REQUIRE(dst.stm.velFloorDef == 7);
    REQUIRE(dst.stm.sensitivity(0) == 2048);
    REQUIRE(dst.stm.lrRatio(0) == 60);
    REQUIRE(dst.LCDbrightness == 42);
    REQUIRE(dst.themeColorIndex == 3);
    REQUIRE(dst.perfStatsFlags == 5);
    REQUIRE(dst.RGBbrightness == 66);
    REQUIRE(dst.quickSettingsEnabled == false);
    REQUIRE(dst.AudioEngineEnabled == false);
    REQUIRE(dst.usbAudioEnabled == true);
    REQUIRE(dst.usbSerialGuard == false);
    REQUIRE(dst.Kit == 4);
}

TEST_CASE("SettingsIo: pad tuning round-trip", "[settings][io]") {
    TempFile file("cp_io_pads.json");

    CrosspadSettings src = snapshot();
    src.stm.velCurveDef = 2;
    src.stm.velFloorDef = 33;
    src.stm.ccPressBase = 20;
    src.stm.panCcEn = 1;
    src.stm.ccPanBase = 52;
    src.stm.analogMinMs = 24;
    src.stm.analogDelta = 5;
    src.stm.pressDebounceMs = 8;
    src.stm.hitMode = 1;
    src.stm.hitMinMs = 3;
    src.stm.hitPeakDrop = 40;
    src.stm.hitPeakSamples = 4;
#if defined(PAD_CONFIG_PER_PAD)
    for (int i = 0; i < Stm32Settings::kPads; ++i) {
        src.stm.padSensitivity[i] = (uint16_t)(100 + i);
        src.stm.padLrRatio[i]     = (uint8_t)(10 + i);
        src.stm.padAnalogMode[i]  = (uint8_t)(i % 4);
        src.stm.padVelCurve[i]    = (uint8_t)(i % 3);
        src.stm.padVelFloor[i]    = (uint8_t)(i * 2);
    }
    src.stm.padVelCurve[5] = 0xFF;   // inherit sentinel must survive the trip
    src.stm.padVelFloor[5] = 0xFF;
#else
    src.stm.sensitivity(0) = 1234;
    src.stm.lrRatio(0)     = 70;
    src.stm.analogMode(0)  = 1;
    src.stm.velCurve(0)    = 0xFF;
    src.stm.velFloor(0)    = 0xFF;
#endif

    REQUIRE(settingsExportPads(src, file.c_str()).ok);

    CrosspadSettings dst = snapshot();
    SettingsIoResult read = settingsImportPads(dst, file.c_str());
    REQUIRE(read.ok);
    REQUIRE(read.version == kSettingsFileVersion);

    REQUIRE(dst.stm.velCurveDef == 2);
    REQUIRE(dst.stm.velFloorDef == 33);
    REQUIRE(dst.stm.ccPressBase == 20);
    REQUIRE(dst.stm.panCcEn == 1);
    REQUIRE(dst.stm.ccPanBase == 52);
    REQUIRE(dst.stm.analogMinMs == 24);
    REQUIRE(dst.stm.analogDelta == 5);
    REQUIRE(dst.stm.pressDebounceMs == 8);
    REQUIRE(dst.stm.hitMode == 1);
    REQUIRE(dst.stm.hitMinMs == 3);
    REQUIRE(dst.stm.hitPeakDrop == 40);
    REQUIRE(dst.stm.hitPeakSamples == 4);
#if defined(PAD_CONFIG_PER_PAD)
    for (int i = 0; i < Stm32Settings::kPads; ++i) {
        REQUIRE(dst.stm.padSensitivity[i] == (uint16_t)(100 + i));
        REQUIRE(dst.stm.padLrRatio[i] == (uint8_t)(10 + i));
        REQUIRE(dst.stm.padAnalogMode[i] == (uint8_t)(i % 4));
    }
    REQUIRE(dst.stm.padVelCurve[5] == 0xFF);
    REQUIRE(dst.stm.padVelFloor[5] == 0xFF);
    REQUIRE(dst.stm.padVelCurve[4] == (uint8_t)(4 % 3));
    REQUIRE(dst.stm.padVelFloor[4] == 8);
#else
    REQUIRE(dst.stm.sensitivity(0) == 1234);
    REQUIRE(dst.stm.lrRatio(0) == 70);
    REQUIRE(dst.stm.analogMode(0) == 1);
    REQUIRE(dst.stm.velCurve(0) == 0xFF);
    REQUIRE(dst.stm.velFloor(0) == 0xFF);
#endif
}

TEST_CASE("SettingsIo: export carries only what NVS persists", "[settings][io]") {
    TempFile file("cp_io_shape.json");
    REQUIRE(settingsExportAll(snapshot(), file.c_str()).ok);

    FILE* f = fopen(file.c_str(), "r");
    REQUIRE(f != nullptr);
    std::string text;
    for (int c; (c = fgetc(f)) != EOF;) text.push_back((char)c);
    fclose(f);

    // Version-1 files were 30 kB: sixteen per-pad effect chains plus a preview
    // chain that nothing read and nothing saved.
    REQUIRE(text.size() < 3000);
    REQUIRE(text.find("\"audio\"") == std::string::npos);
    REQUIRE(text.find("\"previewAudio\"") == std::string::npos);
    REQUIRE(text.find("\"eq\"") == std::string::npos);
    REQUIRE(text.find("\"roomSize\"") == std::string::npos);
    REQUIRE(text.find("\"masterFX\"") != std::string::npos);
    REQUIRE(text.find("\"reverb\":{\"bypass\":") != std::string::npos);
}

TEST_CASE("SettingsIo: version-1 file imports, per-pad sections ignored", "[settings][io]") {
    TempFile file("cp_io_v1.json");
    writeRaw(file.c_str(),
             "{\"kind\":\"crosspad-settings\",\"version\":1,\"settings\":{"
             "\"audio\":[{\"outVolume\":3,\"reverb\":{\"bypass\":false,\"roomSize\":12}}],"
             "\"previewAudio\":{\"inVolume\":4},"
             "\"masterFX\":{\"outVolume\":61,\"delay\":{\"bypass\":false,\"length\":70,\"feedback\":20},"
             "\"eq\":[{\"bypass\":false,\"mid\":66}]},"
             "\"LCDbrightness\":21}}");

    CrosspadSettings dst = snapshot();
    dst.audio[0].outVolume = 44;
    dst.previewAudio.inVolume = 45;
    dst.masterFX.delay.length = 50;

    SettingsIoResult read = settingsImportAll(dst, file.c_str());
    REQUIRE(read.ok);
    REQUIRE(read.version == 1);

    REQUIRE(dst.LCDbrightness == 21);
    REQUIRE(dst.masterFX.outVolume == 61);
    REQUIRE(dst.masterFX.delay.bypass == false);
    // Effect parameters and the per-pad chains are not settings any more.
    REQUIRE(dst.masterFX.delay.length == 50);
    REQUIRE(dst.audio[0].outVolume == 44);
    REQUIRE(dst.previewAudio.inVolume == 45);
}

TEST_CASE("SettingsIo: wrong kind is rejected", "[settings][io]") {
    TempFile file("cp_io_kind.json");

    CrosspadSettings src = snapshot();
    src.LCDbrightness = 11;
    REQUIRE(settingsExportPads(src, file.c_str()).ok);

    // A pad file must not be swallowed by the full-settings importer, nor the
    // other way round.
    CrosspadSettings dst = snapshot();
    dst.LCDbrightness = 99;
    SettingsIoResult read = settingsImportAll(dst, file.c_str());
    REQUIRE_FALSE(read.ok);
    REQUIRE(read.error != nullptr);
    REQUIRE(dst.LCDbrightness == 99);

    TempFile allFile("cp_io_kind_all.json");
    REQUIRE(settingsExportAll(src, allFile.c_str()).ok);
    REQUIRE_FALSE(settingsImportPads(dst, allFile.c_str()).ok);
}

TEST_CASE("SettingsIo: newer file version is rejected", "[settings][io]") {
    TempFile file("cp_io_version.json");
    writeRaw(file.c_str(),
             "{\"kind\":\"crosspad-settings\",\"version\":99,"
             "\"settings\":{\"LCDbrightness\":7}}");

    CrosspadSettings dst = snapshot();
    uint8_t before = dst.LCDbrightness;
    SettingsIoResult read = settingsImportAll(dst, file.c_str());
    REQUIRE_FALSE(read.ok);
    REQUIRE(read.error != nullptr);
    REQUIRE(read.version == 99);          // reported so the UI can name the version
    REQUIRE(dst.LCDbrightness == before);

    TempFile padFile("cp_io_version_pads.json");
    writeRaw(padFile.c_str(),
             "{\"kind\":\"crosspad-pads\",\"version\":3,\"pads\":{\"velFloorDef\":9}}");
    uint8_t floorBefore = dst.stm.velFloorDef;
    REQUIRE_FALSE(settingsImportPads(dst, padFile.c_str()).ok);
    REQUIRE(dst.stm.velFloorDef == floorBefore);
}

TEST_CASE("SettingsIo: partial file keeps current values", "[settings][io]") {
    TempFile file("cp_io_partial.json");
    // A file from an older firmware: only the sections it knew about.
    writeRaw(file.c_str(),
             "{\"kind\":\"crosspad-settings\",\"version\":1,"
             "\"settings\":{\"LCDbrightness\":33,\"stm\":{\"midiChannel\":4}}}");

    CrosspadSettings dst = snapshot();
    dst.RGBbrightness = 55;
    dst.vibration.outputMax = 123;
    dst.audio[0].outVolume = 44;
    dst.stm.velFloorDef = 6;
    dst.stm.sensitivity(0) = 777;

    SettingsIoResult read = settingsImportAll(dst, file.c_str());
    REQUIRE(read.ok);
    REQUIRE(read.version == 1);

    REQUIRE(dst.LCDbrightness == 33);
    REQUIRE(dst.stm.midiChannel == 4);
    // Absent sections and absent keys are left alone.
    REQUIRE(dst.RGBbrightness == 55);
    REQUIRE(dst.vibration.outputMax == 123);
    REQUIRE(dst.audio[0].outVolume == 44);
    REQUIRE(dst.stm.velFloorDef == 6);
    REQUIRE(dst.stm.sensitivity(0) == 777);
}

TEST_CASE("SettingsIo: out-of-range values are clamped", "[settings][io]") {
    TempFile file("cp_io_clamp.json");
    writeRaw(file.c_str(),
             "{\"kind\":\"crosspad-pads\",\"version\":1,\"pads\":{"
             "\"velFloorDef\":900,\"velCurveDef\":9,\"ccPressBase\":500,\"panCcEn\":4,"
             "\"pressDebounceMs\":0,"
             "\"hitMode\":7,\"hitMinMs\":99,\"hitPeakDrop\":-1,\"hitPeakSamples\":0,"
             "\"padSensitivity\":[99999,0,-3],"
             "\"padLrRatio\":[0,250,50],"
             "\"padAnalogMode\":[9,1,2],"
             "\"padVelCurve\":[7,255,1],"
             "\"padVelFloor\":[200,255,10]}}");

    CrosspadSettings dst = snapshot();
    SettingsIoResult read = settingsImportPads(dst, file.c_str());
    REQUIRE(read.ok);

    REQUIRE(dst.stm.velFloorDef == 127);
    REQUIRE(dst.stm.velCurveDef == 2);
    REQUIRE(dst.stm.ccPressBase == 127);
    REQUIRE(dst.stm.panCcEn == 1);
    REQUIRE(dst.stm.pressDebounceMs == Stm32Settings::kPressDebounceMinMs);
    REQUIRE(dst.stm.hitMode == 1);
    REQUIRE(dst.stm.hitMinMs == Stm32Settings::kPressDebounceMaxMs);
    REQUIRE(dst.stm.hitPeakDrop == 0);
    REQUIRE(dst.stm.hitPeakSamples == 1);
#if defined(PAD_CONFIG_PER_PAD)
    REQUIRE(dst.stm.padSensitivity[0] == 4095);   // saturates instead of wrapping
    REQUIRE(dst.stm.padSensitivity[1] == 1);
    REQUIRE(dst.stm.padSensitivity[2] == 1);
    REQUIRE(dst.stm.padLrRatio[0] == 1);
    REQUIRE(dst.stm.padLrRatio[1] == 99);
    REQUIRE(dst.stm.padAnalogMode[0] == 3);
    REQUIRE(dst.stm.padVelCurve[0] == 2);
    REQUIRE(dst.stm.padVelCurve[1] == 0xFF);      // inherit is legal outside 0..2
    REQUIRE(dst.stm.padVelFloor[0] == 127);
    REQUIRE(dst.stm.padVelFloor[1] == 0xFF);
    // Indices the arrays don't cover keep whatever the unit had.
    REQUIRE(dst.stm.padSensitivity[15] == snapshot().stm.padSensitivity[15]);
#else
    REQUIRE(dst.stm.sensitivity(0) == 4095);
    REQUIRE(dst.stm.lrRatio(0) == 1);
    REQUIRE(dst.stm.analogMode(0) == 3);
    REQUIRE(dst.stm.velCurve(0) == 2);
    REQUIRE(dst.stm.velFloor(0) == 127);
#endif
}

TEST_CASE("SettingsIo: pad import leaves everything else alone", "[settings][io]") {
    TempFile file("cp_io_pads_only.json");

    CrosspadSettings src = snapshot();
    src.stm.velFloorDef = 12;
    src.stm.sensitivity(0) = 3000;
    // Non-pad values that must NOT travel in a pad file.
    src.stm.midiChannel = 11;
    src.LCDbrightness = 13;
    src.keypad.enableKeypad = false;
    REQUIRE(settingsExportPads(src, file.c_str()).ok);

    CrosspadSettings dst = snapshot();
    dst.stm.midiChannel = 2;
    dst.stm.dimTimeoutS = 90;
    dst.LCDbrightness = 88;
    dst.keypad.enableKeypad = true;
    dst.audio[1].outVolume = 17;

    REQUIRE(settingsImportPads(dst, file.c_str()).ok);

    REQUIRE(dst.stm.velFloorDef == 12);
    REQUIRE(dst.stm.sensitivity(0) == 3000);
    REQUIRE(dst.stm.midiChannel == 2);
    REQUIRE(dst.stm.dimTimeoutS == 90);
    REQUIRE(dst.LCDbrightness == 88);
    REQUIRE(dst.keypad.enableKeypad == true);
    REQUIRE(dst.audio[1].outVolume == 17);
}

TEST_CASE("SettingsIo: missing and corrupt files fail cleanly", "[settings][io]") {
    CrosspadSettings dst = snapshot();
    uint8_t before = dst.LCDbrightness;

    SettingsIoResult missing = settingsImportAll(dst, "cp_io_does_not_exist.json");
    REQUIRE_FALSE(missing.ok);
    REQUIRE(missing.error != nullptr);

    TempFile file("cp_io_corrupt.json");
    writeRaw(file.c_str(), "{\"kind\":\"crosspad-settings\",\"version\":");
    SettingsIoResult corrupt = settingsImportAll(dst, file.c_str());
    REQUIRE_FALSE(corrupt.ok);
    REQUIRE(corrupt.error != nullptr);

    REQUIRE(dst.LCDbrightness == before);
}

TEST_CASE("SettingsIo: remote management round-trips and defaults on", "[settings][io][remote]") {
    TempFile file("cp_io_remote.json");
    CrosspadSettings src = snapshot();
    REQUIRE(src.wireless.enableRemote == true);
    src.wireless.enableRemote = false;
    REQUIRE(settingsExportAll(src, file.c_str()).ok);
    CrosspadSettings dst = snapshot();
    REQUIRE(settingsImportAll(dst, file.c_str()).ok);
    REQUIRE(dst.wireless.enableRemote == false);
}
