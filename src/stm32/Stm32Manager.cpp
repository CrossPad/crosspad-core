// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/stm32/Stm32Manager.hpp"
#include "crosspad/stm32/Stm32ConfigSync.hpp"
#include "crosspad/stm32/reg_map.h"
#include "crosspad/platform/IClock.hpp"

namespace crosspad {

namespace {

constexpr uint32_t kKeepAliveIntervalMs = 5000;
constexpr uint32_t kRoutingIntervalMs = 3000;
/// Audible output counts as use at most this often, like the keep-alive.
constexpr uint32_t kAudioActivityIntervalMs = 5000;
/// Output within this long counts as audible.
constexpr uint32_t kAudibleWindowMs = 2000;

} // namespace

void Stm32Manager::init(Stm32CommandSender& cmdSender,
                         CrosspadSettings* settings,
                         IClock& clock) {
    cmdSender_ = &cmdSender;
    settings_ = settings;
    clock_ = &clock;
}

void Stm32Manager::start() {
    if (running_) return;

    // Push the full ESP-authoritative config, then routing + keepalive.
    if (configSync_ && settings_) {
        uint8_t bc = 0;
        if (configSync_->pushAll(settings_->stm)) {
            if (configSync_->readBootCount(bc)) lastBootCount_ = bc;
        }
    }
    if (cmdSender_ && settings_) {
        cmdSender_->sendRoutingMatrix(settings_->stmRouting);
        cmdSender_->sendKeepAlive();
    }

    lastKeepAliveMs_ = 0;
    lastRoutingMs_ = 0;
    lastAudioActivityMs_ = 0;
    running_ = true;
}

void Stm32Manager::stop() {
    running_ = false;
}

void Stm32Manager::setBrightness(uint8_t brightness) {
    if (brightness == lastBrightness_) return;
    if (cmdSender_) {
        cmdSender_->sendBrightness(brightness);
        lastBrightness_ = brightness;
    }
}

void Stm32Manager::tick() {
    if (!running_ || !clock_) return;

    const uint32_t now = static_cast<uint32_t>(clock_->getTimeUs() / 1000);

    // Keep-alive: every 5s or on request
    if (keepAliveRequested_ || (now - lastKeepAliveMs_ >= kKeepAliveIntervalMs)) {
        if (cmdSender_) cmdSender_->sendKeepAlive();
        lastKeepAliveMs_ = now;
        keepAliveRequested_ = false;
    }

    // Routing matrix from settings: every 3s
    if (now - lastRoutingMs_ >= kRoutingIntervalMs) {
        if (cmdSender_ && settings_) {
            cmdSender_->sendRoutingMatrix(settings_->stmRouting);
        }
        lastRoutingMs_ = now;
    }

    // ESP-side user input (touch/GUI) — tell the STM so its idle timer sees
    // interaction that never touches the pad scan.
    if (userActivityPending_) {
        userActivityPending_ = false;
        if (configSync_) configSync_->writeReg8(REG_CMD_USER_ACTIVITY, 1);
    }

    // Speaker amp: enable only while the output bus actually carries signal
    // (peaks fed from the audio thread). Writes only on a decision change so
    // the I2C bus stays quiet; ampApplied_ < 0 forces a write after a
    // config push, which sets REG_CTL_AUDIO_EN from settings behind our back.
    if (configSync_) {
        if (settings_) ampGate_.setAllowed(settings_->masterFX.speakersEnabled);
        bool wantAmp = ampGate_.poll(now);
        // Audible output (speaker or headphones) is use: the STM's idle
        // timer only sees pads/encoder/buttons/touch, so playback left alone
        // went silent at the dim timeout (LIGHT cuts the amp) and off at the
        // sleep timeout. Every 5 s at most, like the keep-alive.
        if (ampGate_.signalWithin(now, kAudibleWindowMs) &&
            now - lastAudioActivityMs_ >= kAudioActivityIntervalMs) {
            if (configSync_->writeReg8(REG_CMD_USER_ACTIVITY, 1)) lastAudioActivityMs_ = now;
        }
        if (ampResyncPending_) {
            ampResyncPending_ = false;
            ampApplied_ = -1;
        }
        if (ampApplied_ < 0 || (bool)ampApplied_ != wantAmp) {
            if (configSync_->writeReg8(REG_CTL_AUDIO_EN, wantAmp ? 1 : 0)) {
                ampApplied_ = wantAmp ? 1 : 0;
            }
        }
    }

    // Detect STM reset (boot count changed) -> re-push full config.
    // now - lastRoutingMs_ == 0 only on the pass the routing block above just set lastRoutingMs_ = now -> polls boot count on the 3s routing cadence.
    if (configSync_ && settings_ && (now - lastRoutingMs_ == 0)) {
        uint8_t bc = 0;
        if (configSync_->readBootCount(bc) && stm32ShouldResync(lastBootCount_, bc)) {
            if (configSync_->pushAll(settings_->stm)) {
                lastBootCount_ = bc;
                ampApplied_ = -1;   // pushAll wrote AUDIO_EN — re-assert the gate
                // A reset STM boots its backlight at its own default (50 %)
                // and pushAll does not carry brightness; setBrightness()
                // skips an unchanged value, so the user's level never came
                // back — the screen stayed brighter (or dimmer) than set.
                if (cmdSender_ && lastBrightness_ != 0xFF) {
                    cmdSender_->sendBrightness(lastBrightness_);
                }
            }
        }
    }
}

} // namespace crosspad
