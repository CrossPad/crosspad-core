// SPDX-License-Identifier: MIT
#pragma once

#include "crosspad/audio/IAudioNode.hpp"
#include "crosspad/song/ISongEngine.hpp"
#include "crosspad/song/ISongEnginePlatform.hpp"

#include <memory>
#include <string>

namespace crosspad {

class MetronomeNode;
class PatternSequencer;

/// The audio song engine: projects under ISongEnginePlatform::songsRoot(),
/// bouncing a sequencer scene to a WAV, and playing four tracks of clips back
/// as a mixer channel. The platform adds it to its mixer (never on the bus a
/// bounce records) and calls rtCapture() with that bus after every render.
class SongEngine final : public ISongEngine, public IAudioNode {
public:
    SongEngine(ISongEnginePlatform& platform, MetronomeNode& clock, PatternSequencer& seq);
    ~SongEngine() override;

    const char* name() const override;
    bool isGenerator() const override;
    void onPrepare(uint32_t maxFrames, uint32_t sampleRate) override;
    void onSampleRateChange(uint32_t sr) override;
    void mix(float* out, uint32_t frames) override;

    /// The pads-only bus, one block, from the render. Does nothing unless a
    /// bounce is armed.
    void rtCapture(const float* padsBus, uint32_t frames);
    /// Stop playback, cancel any bounce and give both rings back; returns
    /// once they are. Never from the worker.
    void releaseRings();
    const char* debugLine() const override;

    bool requestProjects() override;
    std::vector<std::string> projects() override;
    bool requestOpen(const std::string& name) override;
    SongOpState openState() const override;
    AudioSong& song() override;
    std::recursive_mutex& songMutex() override;
    void commit() override;
    bool requestClips() override;
    std::vector<ClipInfo> clips() override;
    uint32_t listGeneration() const override;
    bool requestRename(const std::string& name) override;
    bool requestDelete() override;
    bool requestTempo(float bpm) override;
    SongOpState editState() const override;
    bool requestMerge(uint8_t trackMask, uint32_t fromBar, uint32_t toBar) override;
    uint8_t editProgress() const override;
    bool requestExportLiveSet() override;
    std::vector<std::string> editReport() override;
    bool clipPeaks(const std::string& file, ClipPeaks& out) override;
    uint32_t peaksGeneration() const override;
    uint32_t framesPerBar() override;
    bool requestImport(const std::string& file, uint32_t start, uint32_t end,
                       uint8_t track, uint32_t bar, uint32_t lead) override;
    bool bounceScene(uint8_t scene, uint8_t track) override;
    bool bounceSeqTrack(uint8_t track) override;
    uint32_t playEndBars() override;
    bool liveStart() override;
    bool liveLaunch(uint8_t track, const std::string& file, uint16_t bars) override;
    bool liveStop(uint8_t track) override;
    void liveQuant(uint8_t bars) override;
    bool liveState(LiveState& out) override;
    BounceStatus bounceStatus() override;
    void cancelBounce() override;
    bool play(uint32_t fromBar) override;
    bool playWithTake(uint32_t fromBar, SyncTake take) override;
    bool playCountedIn(uint32_t fromBar, SyncTake take, uint8_t beats) override;
    uint8_t countInBeatsLeft() const override;
    void stop() override;
    bool playing() const override;
    double positionBars() const override;
    uint32_t framesPlayed() const override;
    uint32_t positionFrames() const override;
    uint32_t underruns() const override;
    uint32_t openFailures() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace crosspad
