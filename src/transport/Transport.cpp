// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/transport/Transport.hpp"

#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/settings/IKeyValueStore.hpp"
#include "crosspad/sequencer/SequencerController.hpp"
#include "crosspad/song/ISongEngine.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace crosspad {
namespace {

/* Atomic because the claim is made on whatever thread an app runs on and read
 * by the UI thread on every repaint. The pointed-at target outlives the claim
 * by contract: whoever claims releases before it goes. */
std::atomic<ITransportTarget*> s_target{nullptr};
std::atomic<ITransportTarget*> s_home{nullptr};

std::atomic<bool> s_tempoFromHost{false};
/* The last tempo asked for, which a commit hands the song. */
std::atomic<float> s_pendingTempo{0.0f};

bool can(ITransportTarget* t, uint8_t bit)
{
    return t != nullptr && (t->caps() & bit) != 0;
}

} // namespace

void transportClaim(ITransportTarget* target, bool stopPrevious)
{
    ITransportTarget* prev = s_target.load(std::memory_order_acquire);
    if (prev == target) return;
    /* One transport means one thing sounding. Stopping the old holder before
     * the new one takes over is the whole point of a single claim -- but only
     * one that is running: stop() on a stopped target rewinds it, and taking
     * the transport is not asking for that. */
    if (prev && stopPrevious && prev->state() != TransportState::Stopped) prev->stop();
    s_target.store(target, std::memory_order_release);
}

void transportSetHome(ITransportTarget* target)
{
    s_home.store(target, std::memory_order_release);
    if (target) transportClaim(target);
}

void transportGoHome()
{
    ITransportTarget* home = s_home.load(std::memory_order_acquire);
    ITransportTarget* cur = s_target.load(std::memory_order_acquire);
    if (!home || cur == home) return;
    if (cur && cur->state() != TransportState::Stopped) return;
    /* Only from the holder just looked at: a claim made meanwhile wins. */
    s_target.compare_exchange_strong(cur, home, std::memory_order_acq_rel);
}

void transportRelease(ITransportTarget* target)
{
    ITransportTarget* expected = target;
    s_target.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

ITransportTarget* transportTarget() { return s_target.load(std::memory_order_acquire); }

void transportPlay()
{
    ITransportTarget* t = transportTarget();
    if (can(t, kTransportCanPlay)) t->play();
}

void transportStop()
{
    ITransportTarget* t = transportTarget();
    if (can(t, kTransportCanStop)) t->stop();
}

void transportRecord()
{
    ITransportTarget* t = transportTarget();
    if (can(t, kTransportCanRecord)) t->record();
}

void transportOverdub()
{
    ITransportTarget* t = transportTarget();
    if (can(t, kTransportCanOverdub)) t->overdub();
}

TransportState transportState()
{
    ITransportTarget* t = transportTarget();
    return t ? t->state() : TransportState::Stopped;
}

uint8_t transportCaps()
{
    ITransportTarget* t = transportTarget();
    return t ? t->caps() : 0;
}

float transportBpm()
{
    ITransportTarget* t = transportTarget();
    return t ? t->bpm() : 0.0f;
}

TransportPos transportPosition()
{
    ITransportTarget* t = transportTarget();
    return t ? t->position() : TransportPos{};
}

void transportLocate(int bars)
{
    ITransportTarget* t = transportTarget();
    if (!can(t, kTransportCanLocate) || t->state() != TransportState::Stopped) return;
    t->locate(bars);
}

float transportTempo()
{
    SequencerController* seq = getPlatformServices().sequencer;
    return seq ? seq->getBpm() : 0.0f;
}

void transportSetTempo(float bpm)
{
    if (bpm < kTransportTempoMin) bpm = kTransportTempoMin;
    if (bpm > kTransportTempoMax) bpm = kTransportTempoMax;
    s_pendingTempo.store(bpm);
    ISongEngine* se = getPlatformServices().songEngine;
    if (se && se->playing()) return;
    if (SequencerController* seq = getPlatformServices().sequencer) seq->setBpm(bpm);
}

void transportCommitTempo()
{
    const float bpm = s_pendingTempo.exchange(0.0f);
    if (bpm <= 0.0f) return;
    if (SequencerController* seq = getPlatformServices().sequencer) {
        if (std::fabs(seq->getBpm() - bpm) > 0.01f) seq->setBpm(bpm);
    }
    ISongEngine* se = getPlatformServices().songEngine;
    if (!se) return;
    float song = 0.0f;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        song = se->song().bpm;
    }
    /* A song with no tempo yet takes the first clip's or the sequencer's; one
     * that already runs at this tempo has nothing to re-measure. */
    if (song > 0.0f && std::fabs(song - bpm) > 0.01f) se->requestTempo(bpm);
}

bool transportTempoFromHost() { return s_tempoFromHost.load(std::memory_order_relaxed); }

void transportSetTempoFromHost(bool host) { s_tempoFromHost.store(host, std::memory_order_relaxed); }

bool transportClick()
{
    SequencerController* seq = getPlatformServices().sequencer;
    return seq && seq->getClickActive();
}

/* The metronome is the player's, kept across a restart: every change is
 * written to the platform's store (when it has one) and read back once the
 * sequencer is published (transportRestoreClick()). */
namespace {
constexpr const char* kClickNs = "cfg_click";
IKeyValueStore* clickStore() { return getPlatformServices().kvStore; }
}

void transportSetClick(bool on)
{
    SequencerController* seq = getPlatformServices().sequencer;
    if (!seq || seq->getClickActive() == on) return;
    seq->setClickActive(on);
    if (IKeyValueStore* kv = clickStore()) kv->saveBool(kClickNs, "on", on);
}

float transportClickLevel()
{
    SequencerController* seq = getPlatformServices().sequencer;
    return seq ? seq->getClickLevel() : 0.0f;
}

void transportSetClickLevel(float level)
{
    SequencerController* seq = getPlatformServices().sequencer;
    if (!seq) return;
    seq->setClickLevel(level);
    const uint8_t pct = (uint8_t)std::lround(std::clamp(level, 0.0f, 1.0f) * 100.0f);
    if (IKeyValueStore* kv = clickStore()) kv->saveU8(kClickNs, "level", pct);
}

uint8_t transportClickDivision()
{
    SequencerController* seq = getPlatformServices().sequencer;
    return seq ? seq->getClickDivision() : 1;
}

void transportSetClickDivision(uint8_t div)
{
    SequencerController* seq = getPlatformServices().sequencer;
    if (!seq) return;
    seq->setClickDivision(div);
    if (IKeyValueStore* kv = clickStore()) kv->saveU8(kClickNs, "div", div);
}

uint8_t transportCountInBars()
{
    SequencerController* seq = getPlatformServices().sequencer;
    return seq && seq->getCountIn() ? seq->getCountInBars() : 0;
}

void transportSetCountInBars(uint8_t bars)
{
    SequencerController* seq = getPlatformServices().sequencer;
    if (!seq) return;
    if (bars) seq->setCountInBars(bars);
    seq->setCountIn(bars != 0);
    if (IKeyValueStore* kv = clickStore()) kv->saveU8(kClickNs, "countin", bars);
}

void transportRestoreClick()
{
    SequencerController* seq = getPlatformServices().sequencer;
    IKeyValueStore* kv = clickStore();
    if (!seq || !kv) return;
    seq->setClickActive(kv->readBool(kClickNs, "on", seq->getClickActive()));
    const uint8_t pct = kv->readU8(kClickNs, "level", 255);
    if (pct <= 100) seq->setClickLevel((float)pct / 100.0f);
    const uint8_t div = kv->readU8(kClickNs, "div", 0);
    if (div == 1 || div == 2 || div == 4) seq->setClickDivision(div);
    const uint8_t bars = kv->readU8(kClickNs, "countin", 255);
    if (bars <= 2) {
        if (bars) seq->setCountInBars(bars);
        seq->setCountIn(bars != 0);
    }
}

const char* transportName()
{
    ITransportTarget* t = transportTarget();
    const char* n = t ? t->name() : "";
    return n ? n : "";
}

} // namespace crosspad
