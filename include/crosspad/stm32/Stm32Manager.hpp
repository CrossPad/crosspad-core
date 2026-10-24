// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include "crosspad/protocol/Stm32CommandSender.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/stm32/Stm32ConfigSync.hpp"
#include "crosspad/audio/SpeakerAmpGate.hpp"

namespace crosspad {

class IClock;

/**
 * @brief Portable STM32 co-processor manager
 *
 * Periodically sends keep-alive and routing matrix messages to the STM32,
 * polls its boot count and drives the speaker amp gate. Routing is read from
 * CrosspadSettings (not hardcoded).
 *
 * The platform owns the thread: after start() it calls tick() every
 * kTickPeriodMs, and stops calling it before stop().
 */
class Stm32Manager {
public:
    /// How often the platform calls tick(), measured from the end of one pass
    /// to the start of the next.
    static constexpr uint32_t kTickPeriodMs = 100;

    Stm32Manager() = default;

    /**
     * @brief Store dependencies (does not start maintenance)
     *
     * @param clock time base for the keep-alive, routing and amp cadences
     */
    void init(Stm32CommandSender& cmdSender,
              CrosspadSettings* settings,
              IClock& clock);

    /**
     * @brief Push the full config, routing and a keep-alive; tick() then runs
     *        maintenance until stop().
     */
    void start();

    /**
     * @brief End maintenance: tick() does nothing until the next start().
     */
    void stop();

    /**
     * @brief One maintenance pass (keep-alive, routing, user activity, amp
     *        gate, STM reset detection). Called by the platform every
     *        kTickPeriodMs while running.
     */
    void tick();

    bool isRunning() const { return running_; }

    /**
     * @brief Send brightness command (only on change)
     */
    void setBrightness(uint8_t brightness);

    /**
     * @brief Request a keep-alive to be sent on next loop iteration
     */
    void requestKeepAlive() { keepAliveRequested_ = true; }

    /**
     * @brief Report ESP-side user input (touch, GUI navigation) to the STM.
     *
     * Touch never reaches the STM pad scan, so without this the idle timer would
     * only see pads/encoder/buttons. Flags only — the maintenance task does the
     * I2C write, so this is safe from LVGL/input callbacks.
     */
    void notifyUserActivity() { userActivityPending_ = true; }

    void setConfigSync(Stm32ConfigSync* sync) { configSync_ = sync; }
    Stm32ConfigSync* configSync() const { return configSync_; }

    /**
     * @brief Level-driven speaker amp policy.
     *
     * The audio thread feeds it peaks via submitLevel(); the maintenance loop
     * polls it and writes REG_CTL_AUDIO_EN only when the decision changes, so
     * the amp draws no current while nothing is playing.
     */
    SpeakerAmpGate& ampGate() { return ampGate_; }

    /**
     * @brief Forget what we believe the STM's amp register holds.
     *
     * Call this on every wake notify, so the next poll re-asserts the amp gate
     * unconditionally. The STM gates the amp off by itself when it dims into
     * LIGHT sleep, without touching REG_CTL_AUDIO_EN, so the cached value
     * cannot tell whether the speakers' amp is on after a wake.
     */
    void invalidateAmpState() { ampResyncPending_ = true; }

private:
    Stm32CommandSender* cmdSender_ = nullptr;
    CrosspadSettings* settings_ = nullptr;
    IClock* clock_ = nullptr;

    bool running_ = false;
    volatile bool keepAliveRequested_ = false;
    volatile bool userActivityPending_ = false;
    volatile bool ampResyncPending_ = false;
    uint8_t lastBrightness_ = 0xFF; // impossible initial value → first call always sends

    // Maintenance cadence, in IClock milliseconds (wrapping like the tick count
    // they replaced). Zero at start: the first pass sends a keep-alive and the
    // routing matrix once the board has been up longer than their intervals.
    uint32_t lastKeepAliveMs_ = 0;
    uint32_t lastRoutingMs_ = 0;
    uint32_t lastAudioActivityMs_ = 0;

    Stm32ConfigSync* configSync_ = nullptr;
    int lastBootCount_ = -1;   // <0 = never synced

    SpeakerAmpGate ampGate_;
    int ampApplied_ = -1;      // <0 = unknown, forces the next decision to be written
};

} // namespace crosspad
