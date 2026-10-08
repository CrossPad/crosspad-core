// SPDX-License-Identifier: MIT
#include "crosspad/transport/Transport.hpp"

#include <atomic>

namespace crosspad {
namespace {

/* Atomic because the claim is made on whatever thread an app runs on and read
 * by the UI thread on every repaint. The pointed-at target outlives the claim
 * by contract: whoever claims releases before it goes. */
std::atomic<ITransportTarget*> s_target{nullptr};

bool can(ITransportTarget* t, uint8_t bit)
{
    return t != nullptr && (t->caps() & bit) != 0;
}

} // namespace

void transportClaim(ITransportTarget* target)
{
    ITransportTarget* prev = s_target.load(std::memory_order_acquire);
    if (prev == target) return;
    /* One transport means one thing sounding. Stopping the old holder before
     * the new one takes over is the whole point of a single claim. */
    if (prev) prev->stop();
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

const char* transportName()
{
    ITransportTarget* t = transportTarget();
    const char* n = t ? t->name() : "";
    return n ? n : "";
}

} // namespace crosspad
