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
    uint32_t loopStart = 0;
    uint32_t loopEnd = 0;     ///< 0 (or <= loopStart) means one-shot
};

class SampleBank {
public:
    static constexpr uint8_t kMaxZones = 4;

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

    /**
     * @brief Change a registered zone's root and loop, keeping its PCM.
     *
     * What a kit editor changes while the kit is loaded: nothing here reads or
     * writes sample data. The caller owns the concurrency — the bank is
     * published to the audio thread, so this belongs inside
     * PitchedInstrument::editZone()'s gate, not on its own.
     *
     * @return false for an unknown zone or a loop loopValid() rejects; the
     *         zone is then untouched.
     */
    bool editZone(uint8_t i, float rootMidi, uint32_t loopStart, uint32_t loopEnd);

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
