// SPDX-License-Identifier: GPL-3.0-or-later
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
    /// A bounce or a freeze is armed or recording: rtCapture() needs the pads
    /// tap this block. Without one it only keeps the pads' peak, which a
    /// platform whose seqLevel() answers never reads.
    bool capturing() const;
    /// RT: the capture's ring is past half full -- an offline render waits
    /// here for the card to catch up instead of overflowing it.
    bool captureBacklogged() const;
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
    uint32_t stretchWaiting() const override;
    void trackLevels(int track, float& left, float& right) override;
    uint8_t stretchProgress() const override;
    bool undo() override;
    bool redo() override;
    uint32_t undoSteps() const override;
    uint32_t redoSteps() const override;
    bool requestExportLiveSet() override;
    std::vector<std::string> editReport() override;
    bool requestOpenElement(const std::string& ref) override;
    bool requestElements() override;
    std::vector<ElementInfo> elements() override;
    bool requestBakeElement() override;
    ElementBake bakeResult() override;
    bool requestElementFromRegion(uint32_t fromBar, uint32_t toBar) override;
    bool requestElementFromSong(const std::string& name, const std::vector<std::string>& openElements = {}) override;
    bool requestElementDiscard(const std::string& ref) override;
    bool requestUses() override;
    std::vector<ElementUse> usesOf(const std::string& ref) override;
    bool requestSnapshot(const std::string& ref) override;
    bool requestEditHash() override;
    uint64_t editHash() override;
    bool requestRestore(const std::string& ref) override;
    bool requestDropSnapshot(const std::string& ref) override;
    bool snapshotPending(const std::string& ref) override;
    std::string elementMade() override;
    ElementClipState elementClipState(uint8_t track, size_t clip) override;
    std::string elementClipWhy(uint8_t track, size_t clip) override;
    uint8_t backgroundProgress() const override;
    bool backgroundBusy() const override;
    bool requestContext(const ContextRequest& r) override;
    void contextSolo(bool on) override;
    ContextState contextState() const override;
    std::string contextWhy() override;
    bool clipPeaks(const std::string& file, ClipPeaks& out) override;
    uint32_t peaksGeneration() const override;
    uint32_t framesPerBar() override;
    bool requestImport(const std::string& file, uint32_t start, uint32_t end,
                       uint8_t track, uint32_t bar, uint32_t lead) override;
    bool bounceScene(uint8_t scene, uint8_t track) override;
    bool bounceSeqTrack(uint8_t track) override;
    bool freezeTrack(uint8_t track) override;
    FreezeState freezeState(uint8_t track) override;
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
    bool recordSeq(uint32_t fromBar, SyncTake take, uint8_t beats) override;
    bool seqPunch(SyncTake take) override;
    SyncTake seqTake() const override;
    bool seqPunchable() const override;
    bool openSeqEdit(uint8_t track, uint32_t bar, uint8_t scene) override;
    void syncSeqEdit() override;
    void closeSeqEdit() override;
    int32_t seqEditBar() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace crosspad
