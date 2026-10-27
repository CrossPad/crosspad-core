// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/pad/IPadLogicHandler.hpp"
#include "crosspad/pad/PadLedController.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sampler/ISecondKit.hpp"

#include <memory>

namespace crosspad {

/// The fingers on a kit of a track's own (ISecondKit): Arrange pushes it while
/// such a track is armed, aimed at the kit slot the track holds (setSlot()),
/// so what is played is what a REC on the track keeps. Plain hits -- the 16
/// levels are the Sampler's, on its own kit.
///
/// Only the fingers: a song's notes on the pads' route (the first kit track,
/// EventSource::SequencerCallback) still reach the pads through whichever
/// logic is on top, and are the first kit's -- they go on to the base logic
/// (the Sampler's), or the armed second track would play the first's notes.
class SecondKitPadLogic : public IPadLogicHandler {
public:
    /// The pads in one colour, the second kit's -- its pads' own colours are
    /// in a kit the board holds only as samples.
    static constexpr uint32_t kColor = 0xFF8A00;

    void onActivate(PadManager&) override
    {
        const RgbColor c((kColor >> 16) & 0xFF, (kColor >> 8) & 0xFF, kColor & 0xFF);
        auto& leds = getPadLedController();
        for (uint8_t i = 0; i < 16; ++i) {
            leds.setPadColor(i, c.Dim(96));
            leds.setPadPressedColor(i, c);
            leds.setPadPlayingColor(i, c.Dim(48));
        }
    }
    /// A pad still held when the pads go elsewhere lets go here: a pad on
    /// HOLD would sound on until its sample ends.
    void onDeactivate(PadManager&) override
    {
        for (uint8_t i = 0; i < 16; ++i) {
            if (heldOn_[i]) heldOn_[i]->noteOff(i);
            heldOn_[i] = nullptr;
        }
    }

    /// The kit slot the fingers play (AudioSong::kitSlots): 0 the second kit
    /// engine, 1 the third. A pad held as it changes lets go where it was hit.
    void setSlot(int slot) { slot_ = slot ? 1 : 0; }
    int slot() const { return slot_; }

    void onPadPress(PadManager& pm, uint8_t pad, uint8_t velocity) override
    {
        if (pad >= 16) return;
        if (pm.pressSource(pad) == EventSource::SequencerCallback) {
            if (auto base = beneath(pm)) base->onPadPress(pm, pad, velocity);
            return;
        }
        if (heldOn_[pad]) heldOn_[pad]->noteOff(pad);
        heldOn_[pad] = engine();
        if (heldOn_[pad]) heldOn_[pad]->noteOn(pad, velocity);
        pm.notifyNoteOn(pad, velocity, /*fromSampler=*/true);
    }
    void onPadRelease(PadManager& pm, uint8_t pad) override
    {
        if (pad >= 16) return;
        if (pm.pressSource(pad) == EventSource::SequencerCallback) {
            if (auto base = beneath(pm)) base->onPadRelease(pm, pad);
            return;
        }
        if (heldOn_[pad]) heldOn_[pad]->noteOff(pad);
        heldOn_[pad] = nullptr;
        pm.notifyNoteOff(pad, /*fromSampler=*/true);
    }
    void onPadPressure(PadManager&, uint8_t, uint8_t) override {}

private:
    /// The logic that plays the first kit: the base one, never this.
    std::shared_ptr<IPadLogicHandler> beneath(PadManager& pm) const
    {
        auto base = pm.getPadLogic(pm.getBasePadLogic());
        return base.get() == this ? nullptr : base;
    }

    ISecondKit* engine() const
    {
        return slot_ ? getPlatformServices().thirdKit : getPlatformServices().secondKit;
    }

    int slot_ = 0;
    ISecondKit* heldOn_[16] = {};   ///< the engine each pad a finger holds was hit on
};

} // namespace crosspad
