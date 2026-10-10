// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/transport/Transport.hpp"

#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sequencer/SequencerController.hpp"
#include "crosspad/song/ISongEngine.hpp"

#include <atomic>
#include <cmath>
#include <mutex>

namespace crosspad {
namespace {

/* Atomic because the claim is made on whatever thread an app runs on and read
 * by the UI thread on every repaint. The pointed-at target outlives the claim
 * by contract: whoever claims releases before it goes. */
std::atomic<ITransportTarget*> s_target{nullptr};

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

void transportSetClick(bool on)
{
    if (SequencerController* seq = getPlatformServices().sequencer) seq->setClickActive(on);
}

const char* transportName()
{
    ITransportTarget* t = transportTarget();
    const char* n = t ? t->name() : "";
    return n ? n : "";
}

} // namespace crosspad
