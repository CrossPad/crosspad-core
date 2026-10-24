// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/audio/IAudioNode.hpp"
#include "crosspad/sampletools/ISampleTools.hpp"
#include "crosspad/sampletools/ISampleToolsPlatform.hpp"

#include <memory>
#include <string>
#include <vector>

namespace crosspad {

/// The sample tools behind the Recorder and the Slicer: a take from an input
/// to a WAV on the card, a region of a WAV auditioned on the "Preview"
/// channel, and the file jobs -- listing, drawing a file, saving a loop,
/// rendering a pad's copy, writing a sliced loop out as a kit.
///
/// The platform adds it to its mixer as a generator and feeds the render's
/// taps: rtBlockBegin() first in every block, rtInput()/rtUsb() with the
/// inputs as they are read, rtBus() with the pads tap after the mixer.
class SampleToolsEngine final : public ISampleTools, public IAudioNode {
public:
    /// Someone else listening to the selected input -- every block of it,
    /// take or no take.
    using CaptureTap = void (*)(void* ctx, const int16_t* stereo, uint32_t frames);

    explicit SampleToolsEngine(ISampleToolsPlatform& platform);
    ~SampleToolsEngine() override;

    // ── IAudioNode: the preview ───────────────────────────────────────────
    const char* name() const override;
    bool isGenerator() const override;
    void onPrepare(uint32_t maxFrames, uint32_t sampleRate) override;
    void onSampleRateChange(uint32_t sr) override;
    void mix(float* out, uint32_t frames) override;

    // ── Render taps ───────────────────────────────────────────────────────
    /// Whether this block's ADC of @p codec is wanted (0 the mics, 1 the line-in).
    bool wantsCodec(int codec) const;
    /// One block of codec @p codec's ADC, stereo int16.
    void rtInput(int codec, const int16_t* block, uint32_t frames);
    /// One block of what the host plays in over USB audio, stereo int16.
    void rtUsb(const int16_t* block, uint32_t frames);
    /// A render block begins, before any of it is read or mixed.
    void rtBlockBegin();
    /// The pads tap of this block, after the mixer; also where a take armed
    /// with the song sees the song start.
    void rtBus(const float* padsBus, uint32_t frames);
    /// rtBus() has something to do this block: a take of the pads or the
    /// Pitch instrument, or one waiting for the song to start. Without, the
    /// platform need not make the tap at all.
    bool wantsBus() const;
    void setCaptureTap(CaptureTap fn, void* ctx);

    /// Before the platform takes back memory it lent a ring: a lent preview
    /// ring is given up, by the worker between card accesses, within ~2 s.
    /// Bounded either way: a worker inside one long card access has the ring
    /// marked lost, and forgets it without touching it.
    void reclaimLentRing();

    // ── ISampleTools ──────────────────────────────────────────────────────
    bool     selectInput(RecInput in) override;
    RecInput input() const override;
    bool     inputAvailable(RecInput in) const override;
    void     peaks(float& left, float& right) override;
    bool     startRecording() override;
    bool     startRecordingWithSong(uint32_t fromBar) override;
    bool     startRecordingWithSong(uint32_t fromBar, uint8_t countInBeats) override;
    void     stopRecording() override;
    RecStatus recStatus() override;
    void     releaseInput() override;

    bool requestRecordings() override;
    std::vector<std::string> recordings() override;
    bool requestOverview(const std::string& file, uint32_t start, uint32_t end, uint16_t columns) override;
    WaveOverview overview() override;
    bool requestFolder(const std::string& dir) override;
    CardFolder folder() override;
    std::string cardRoot() const override;
    bool requestSaveRegion(const std::string& file, uint32_t start, uint32_t end) override;
    bool requestExportKit(const SliceKit& kit) override;
    bool requestRendered(const std::string& file, const std::string& out, const SampleFx& fx) override;
    bool requestNormalized(const std::string& file, const std::string& out, float peak) override;
    ToolJob     job() const override;
    std::string jobResult() override;
    uint8_t     jobProgress() const override;

    bool     preview(const std::string& file, uint32_t start, uint32_t end, bool loop) override;
    bool     previewAs(const std::string& file, uint32_t start, uint32_t end, bool loop,
                       const PreviewSound& sound) override;
    void     stopPreview() override;
    bool     previewing() const override;
    uint32_t previewFrame() const override;

    void setHandoff(const std::string& file, uint32_t start, uint32_t end) override;
    bool takeHandoff(std::string& file, uint32_t& start, uint32_t& end) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace crosspad
