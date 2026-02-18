#pragma once
#include <cstdint>
#include <cstddef>

namespace crosspad {

class Stm32Status {
public:
    bool isDfuMode = false;
    // REG1:
    bool usbChargingBusy = false;
    bool batteryFault = false;
    bool usbStatus = false;
    bool chargingStatus = false;
    bool usbFault = false;
    bool imuInt1 = false;
    bool sdDetected = false;
    // REG2:
    bool encoderDirection = false;
    bool isAnyPressed = false;
    bool powerOffRequested = false;
    bool halfInactive = false;
    bool midiTxType = false;
    bool stmPcCommunication = false;
    bool reserved = false;
    // Voltages
    uint16_t VUSB_STM = 0;
    uint16_t VUSB_ESP = 0;
    uint16_t VBAT = 0;

    void SetStm32Status(uint8_t reg1, uint8_t reg2) {
        isDfuMode = false;
        usbChargingBusy = (reg1 & 0x01);
        batteryFault = (reg1 & 0x02);
        usbStatus = (reg1 & 0x04);
        chargingStatus = (reg1 & 0x08);
        usbFault = (reg1 & 0x10);
        imuInt1 = (reg1 & 0x20);
        sdDetected = (reg1 & 0x40);
        encoderDirection = (reg2 & 0x01);
        isAnyPressed = (reg2 & 0x02);
        powerOffRequested = (reg2 & 0x04);
        halfInactive = (reg2 & 0x08);
        midiTxType = (reg2 & 0x10);
        stmPcCommunication = (reg2 & 0x20);
        reserved = (reg2 & 0x40);
    }

    /**
     * Unpack three 14-bit voltage values from 7-bit SysEx pairs.
     * @param sysexData pointer to array[3] of the VOLTAGES_MV message
     *        (6 bytes: VUSB_STM_H, VUSB_STM_L, VUSB_ESP_H, VUSB_ESP_L, VBAT_H, VBAT_L)
     */
    void unpackVoltages(const uint8_t* sysexData) {
        VUSB_STM = ((sysexData[0] & 0x7F) << 7) | (sysexData[1] & 0x7F);
        VUSB_ESP = ((sysexData[2] & 0x7F) << 7) | (sysexData[3] & 0x7F);
        VBAT     = ((sysexData[4] & 0x7F) << 7) | (sysexData[5] & 0x7F);
    }

    /**
     * Linear battery percentage: 3300 mV → 0%, 4200 mV → 100%.
     * Returns -1 if voltage is below 3200 mV (no battery / dead).
     */
    static int8_t calculateBatteryPct(uint16_t vbat_mv) {
        if (vbat_mv < 3200) return -1;
        int pct = static_cast<int>(vbat_mv - 3300) * 100 / (4200 - 3300);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        return static_cast<int8_t>(pct);
    }
};

class CrosspadStatus {
public:
    enum CurrentGUI {
        GUI_WELCOME,
        GUI_MAIN,
        GUI_SETTINGS,
        GUI_SAMPLER,
        GUI_MASTERING,
        GUI_DFU,
        GUI_USER_APP,
    };

    Stm32Status stm32;

    CurrentGUI currentGUI = GUI_WELCOME;
    bool isGuiInitialised = false;
    bool isGuiStoped = false;
    bool isStatusBarOpened = false;
    bool isQuickSettingsOpened = false;
    uint8_t cpu0Usage = 0;
    uint8_t cpu1Usage = 0;
    bool bluetoothConnected = false;
    bool wifiConnected = false;
    bool sdCardDetected = false;
    bool imuDetected = false;
    int8_t batteryPct = 0;

    void* CurrentApp = nullptr;
    void* PreviousApp = nullptr;

    bool isSamplerPlaying = false;
    bool isSequencerEnabled = false;
    uint8_t keyOffset = 36; // C2 MIDI
    bool audioEngineStarted = false;
    bool samplerLoaded = false;
    bool keypadDirty = false;

    /// Returns false if transition is blocked (e.g. DFU mode).
    /// Platform code should handle blocking behavior.
    bool SetCurrentGUI(CurrentGUI gui) {
        if (currentGUI == GUI_DFU || gui == GUI_WELCOME) {
            return false;
        }
        currentGUI = gui;
        return true;
    }
};

} // namespace crosspad
