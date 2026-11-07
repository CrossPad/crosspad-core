// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/sampler/KitTake.hpp"

#include "crosspad/kit/KitPathUtils.hpp"

#include <cstdio>
#include <cstring>

/* Runs on a request (a kit load's end, an editor's knob, a verb), never in the
 * render; the board builds the sampler files -O2 for the render's sake. */
#if defined(__GNUC__)
#define CROSSPAD_COLD __attribute__((cold))
#else
#define CROSSPAD_COLD
#endif

namespace crosspad {

namespace {

/// Whole or not at all: a name cut to fit would be another file's name.
bool copyWhole(char* dst, size_t cap, const std::string& src)
{
    if (src.size() >= cap) return false;
    std::memcpy(dst, src.c_str(), src.size() + 1);
    return true;
}

CROSSPAD_COLD void copyPad(const KitInfo& kit, const Pad& pad, TakePad& tp)
{
    tp = TakePad{};
    tp.mode    = pad.playbackMode;
    /* One voice: every slice cuts the one sounding, as on a hardware sampler. */
    tp.choke   = static_cast<uint8_t>(kit.engine == KitEngine::Slice ? 1 : pad.choke_group);
    tp.volume  = static_cast<int16_t>(pad.volume);
    tp.pan     = static_cast<int16_t>(pad.pan);
    const int pitch = pad.pitch + kit.pitch;
    tp.pitch   = static_cast<int8_t>(pitch < -24 ? -24 : pitch > 24 ? 24 : pitch);
    tp.attack  = pad.attack;
    tp.hp      = pad.hp;
    tp.lp      = pad.lp;
    tp.reverse = pad.reverse;
    tp.cycle   = pad.cycle;
}

/// The pad's layers into @p layers[at..cap). @return layers written; the
/// rest (no room, or a name that does not fit) counted in @p dropped.
CROSSPAD_COLD uint16_t copyLayers(const Pad& pad, TakeLayer* layers, uint16_t at, uint16_t cap,
                                  uint16_t& dropped)
{
    uint16_t n = 0;
    const size_t count = pad.sampleCount < pad.samples.size() ? pad.sampleCount : pad.samples.size();
    for (size_t i = 0; i < count && i < KitPad::kMaxLayers; ++i) {
        if (at + n >= cap) { ++dropped; continue; }
        TakeLayer& l = layers[at + n];
        if (!copyWhole(l.sample, sizeof(l.sample), pad.samples[i].sample)) { ++dropped; continue; }
        l.start = pad.samples[i].start;
        l.end   = pad.samples[i].end;
        ++n;
    }
    return n;
}

bool takeable(const KitInfo& kit) { return kit.parsed && kit.engine != KitEngine::Pitched; }

} // namespace

CROSSPAD_COLD bool buildTakeSpec(const KitInfo& kit, int kitId, TakeSpec& out)
{
    out.kitId = -1;
    out.kitPath[0] = '\0';
    out.name[0] = '\0';
    out.layerCount = 0;
    out.layersDropped = 0;
    out.voiceCap = 0;
    for (auto& p : out.pads) p = TakePad{};
    if (!takeable(kit) || !copyWhole(out.kitPath, sizeof(out.kitPath), kit.path)) return false;

    out.kitId = kitId;
    std::snprintf(out.name, sizeof(out.name), "%s", kit.name.c_str());   // a label: cut is fine
    out.voiceCap = kit.voiceCap();
    for (uint8_t p = 0; p < SamplerKit::kPads && p < KIT_PADS; ++p) {
        TakePad& tp = out.pads[p];
        copyPad(kit, kit.pads[p], tp);
        tp.firstLayer = static_cast<uint8_t>(out.layerCount);
        tp.layerCount = static_cast<uint8_t>(
            copyLayers(kit.pads[p], out.layers, out.layerCount, TakeSpec::kMaxLayers, out.layersDropped));
        out.layerCount = static_cast<uint16_t>(out.layerCount + tp.layerCount);
    }
    return true;
}

CROSSPAD_COLD bool buildTakePadSpec(const KitInfo& kit, int kitId, uint8_t pad, TakePadSpec& out)
{
    out.kitId = -1;
    out.kitPath[0] = '\0';
    out.layersDropped = 0;
    out.pad = TakePad{};
    if (pad >= SamplerKit::kPads || pad >= KIT_PADS || !takeable(kit)) return false;
    if (!copyWhole(out.kitPath, sizeof(out.kitPath), kit.path)) return false;
    out.kitId = kitId;
    copyPad(kit, kit.pads[pad], out.pad);
    out.pad.firstLayer = 0;
    out.pad.layerCount = static_cast<uint8_t>(
        copyLayers(kit.pads[pad], out.layers, 0, KitPad::kMaxLayers, out.layersDropped));
    return true;
}

CROSSPAD_COLD std::string takeLayerLogical(const char* kitPath, const TakeLayer& layer)
{
    return resolveKitSamplePath(kitPath ? kitPath : "", layer.sample);
}

CROSSPAD_COLD std::string takeLayerCopy(const TakePad& pad, const std::string& logical)
{
    if (!pad.wantsCopy() || logical.empty()) return {};
    return renderedSamplePath(logical, pad.fx());
}

} // namespace crosspad
