// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file SampleBank.hpp
 * @brief Owns the PCM of one pitched instrument: up to four recorded zones.
 *
 * Memory comes from caller-supplied callbacks so the platform decides where it
 * lands (PSRAM on the device, plain malloc on PC and in tests). The bank owns
 * every block handed out by allocPcm() and frees them in the destructor,
 * whether or not a zone ended up referencing them.
 *
 * Not thread-safe. Build a bank on the loader thread, then publish the pointer
 * to the audio thread through an atomic; never mutate a published bank.
 */

#include <cstddef>
#include <cstdint>

namespace crosspad {

struct Zone {
    int16_t* pcm = nullptr;
    uint32_t length = 0;      ///< frames of mono PCM
    float rootMidi = 69.0f;   ///< MIDI note the recording sounds at
    /// Where a note begins. Its own point, not the loop start: a held note is
    /// its attack once and then the loop, and a loop placed after the attack
    /// would otherwise cut the attack off. Stored by startFor().
    uint32_t start = 0;
    uint32_t loopStart = 0;
    uint32_t loopEnd = 0;     ///< 0 (or <= loopStart) means one-shot
    /**
     * @brief Frames before loopEnd that are crossfaded with the frames before
     *        loopStart, so the wrap has no seam.
     *
     * A plain loop jumps from pcm[loopEnd - 1] to pcm[loopStart], and unless
     * the two happen to meet that is a click every loop -- and a held note
     * built from one sample is nothing but loops. Here the last loopXfade
     * frames of the loop fade out while the same stretch *before* loopStart
     * fades in, so at the wrap the voice is already playing what comes right
     * before loopStart and steps into it. Done in the render, not baked into
     * the PCM, so a loop edited while the bank plays needs no rebuild.
     *
     * Never more than the loop is long nor than there is before its start:
     * addZone() and editZone() store loopXfadeFor() of what they are given.
     */
    uint32_t loopXfade = 0;
    /// The PCM's rate over the engine's. A zone kept at its file's rate
    /// (PitchedLoadEnv::convertRate off) steps this much slower or faster, so
    /// its root still sounds at rootMidi.
    float rate = 1.0f;
    /// Applied to every voice of the zone (PitchedLoadEnv::normalize): a
    /// take recorded quietly plays as loud as one recorded hot.
    float gain = 1.0f;
};

class SampleBank {
public:
    /// A multisample's worth: the auto-sampler takes one zone every two or
    /// three semitones over four octaves. A zone is ~20 B here; the PCM is
    /// what the bank budget limits.
    static constexpr uint8_t kMaxZones = 24;

    /**
     * @brief Shortest loop a zone may declare, in samples.
     *
     * Four samples is the shortest span the renderer's four-tap interpolator
     * can read without leaving the loop. It is also the floor that keeps the
     * wrap cheap: a one-sample loop played at ratio 1533 (a zone rooted eight
     * octaves below the note) wraps ~98k times per 64-frame block, which the
     * RT task pays for one voice.
     */
    static constexpr uint32_t kMinLoopSamples = 4;

    SampleBank(void* (*alloc)(size_t), void (*free)(void*));
    ~SampleBank();

    SampleBank(const SampleBank&) = delete;
    SampleBank& operator=(const SampleBank&) = delete;

    /// Allocate PCM for one zone. nullptr when full, empty, or out of memory.
    int16_t* allocPcm(uint32_t length);

    /**
     * @brief Register a zone. Its pcm must have come from allocPcm() on this bank.
     *
     * A zone with `loopEnd <= loopStart` is stored as a one-shot. A zone that
     * does declare a loop is rejected outright -- not clamped -- when the loop
     * runs past `length` or spans fewer than kMinLoopSamples samples: silently
     * repairing a loop the file asked for hides a broken kit, and the engine
     * has to trust the zone it is handed.
     */
    bool addZone(const Zone& z);

    /**
     * @brief Is this a loop the renderer can play on a zone of @p length?
     *
     * A disabled loop (`loopEnd <= loopStart`) always is. A real one must end
     * inside the zone and span at least kMinLoopSamples. addZone() and every
     * later edit ask the same question, so an editor can offer the same
     * verdict before it commits anything.
     */
    static bool loopValid(uint32_t length, uint32_t loopStart, uint32_t loopEnd);

    /// The crossfade a loop can actually have: @p xfade, cut to the loop's
    /// length and to what precedes its start. 0 for a one-shot.
    static uint32_t loopXfadeFor(uint32_t loopStart, uint32_t loopEnd, uint32_t xfade);

    /// Keep the zone's crossfade, or its start, in editZone().
    static constexpr uint32_t kKeepXfade = 0xFFFFFFFFu;
    static constexpr uint32_t kKeepStart = 0xFFFFFFFFu;

    /// Where a note can begin: inside the PCM, and for a loop not past its
    /// end -- a start there would never reach the loop -- else the loop start.
    static uint32_t startFor(uint32_t length, uint32_t loopStart, uint32_t loopEnd, uint32_t start);

    /**
     * @brief Change a registered zone's root and loop, keeping its PCM.
     *
     * What a kit editor changes while the kit is loaded: nothing here reads or
     * writes sample data. The caller owns the concurrency — the bank is
     * published to the audio thread, so this belongs inside
     * PitchedInstrument::editZone()'s gate, not on its own.
     *
     * @param loopXfade frames of crossfade, or kKeepXfade for what the zone
     *        has; either way cut to loopXfadeFor() the new loop.
     * @param start where a note begins, or kKeepStart; either way startFor().
     * @return false for an unknown zone or a loop loopValid() rejects; the
     *         zone is then untouched.
     */
    bool editZone(uint8_t i, float rootMidi, uint32_t loopStart, uint32_t loopEnd,
                  uint32_t loopXfade = kKeepXfade, uint32_t start = kKeepStart);

    uint8_t zoneCount() const { return zoneCount_; }
    const Zone& zone(uint8_t i) const;

    /// Zone whose root is closest to @p midi in semitones; ties pick the lower
    /// root. nullptr when the bank is empty.
    const Zone* nearestZone(float midi) const;

    /// Bytes of PCM currently owned.
    size_t bytes() const { return bytes_; }

private:
    void* (*alloc_)(size_t);
    void (*free_)(void*);

    int16_t* blocks_[kMaxZones] = {};
    uint8_t blockCount_ = 0;

    Zone zones_[kMaxZones] = {};
    uint8_t zoneCount_ = 0;

    size_t bytes_ = 0;
};

} // namespace crosspad
