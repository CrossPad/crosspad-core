#pragma once
#include <crosspad/sequencer/SequencerTypes.hpp>
#include <crosspad/sequencer/ISequencerEngine.hpp>

namespace crosspad {

/**
 * @brief Cross-platform sequencer state manager and controller.
 *
 * Owns all sequencer business logic: mode/state transitions, BPM clamping,
 * solo-mode toggling, sequence navigation.  Delegates actual MIDI
 * sequencing + click to ISequencerEngine (platform-supplied).
 */
class SequencerController {
public:
    using Limits = SequencerLimits;

    // Backward-compatible nested aliases used by GUI code
    using quantization_e = Quantization;
    static constexpr Quantization q_1_4      = Quantization::Quarter;
    static constexpr Quantization q_1_8      = Quantization::Eighth;
    static constexpr Quantization q_1_16     = Quantization::Sixteenth;
    static constexpr Quantization q_1_32     = Quantization::ThirtySecond;
    static constexpr Quantization q_1_8T     = Quantization::EighthTriplet;
    static constexpr Quantization q_1_16T    = Quantization::SixteenthTriplet;
    static constexpr Quantization q_INFINITE = Quantization::Off;

    void init(ISequencerEngine& engine) {
        engine_ = &engine;
        engine_->setLength(seqLength_);
        engine_->setClickAudible(clickActive_);
        engine_->setClickBpm(bpm_);
        engine_->setQuantization(quantization_);
        engine_->setLoop(loopActive_);
        engine_->setPreLen(4);
        engine_->selectSeqRecord(recSequence_);
        setSoloMode(soloMode_);
        for (uint8_t i = 0; i <= Limits::MAX_SEQ_INDEX; i++) {
            engine_->selectDirect(i, seqPlyState[i]);
        }
        engine_->resetClick();
        enabled_ = true;
    }

    // ---------- getters ----------

    SequencerMode  getMode()        const { return mode_; }
    SequencerState getState()       const { return state_; }
    SequencerMode  getNextMode()    const { return nextMode_; }
    SequencerState getNextState()   const { return nextState_; }
    uint8_t        getBar()         const { return bar_; }
    uint8_t        getSeqLength()   const { return seqLength_; }
    bool           getClickActive() const { return clickActive_; }
    uint8_t        getLoopLength()  const { return loopLength_; }
    uint8_t        getPlaybackSequence() const { return plySequence_; }
    uint8_t        getRecordSequence()   const { return recSequence_; }
    /// From the engine: the clock is its, and the tempo is set behind this
    /// controller's back every time a kit lands with a bpm of its own.
    float          getBpm()         const { return engine_ ? engine_->getClickBpm() : bpm_; }
    Quantization   getQuantization() const { return quantization_; }
    bool           getSoloMode()    const { return soloMode_; }
    bool           isEnabled()      const { return enabled_; }
    bool           isLoopActive()   const { return loopActive_; }

    bool isRecording() const {
        return (mode_ == SequencerMode::Record || mode_ == SequencerMode::Overdub)
            && state_ == SequencerState::Running;
    }

    bool getSeqPlyState(uint8_t idx) const {
        if (idx > Limits::MAX_SEQ_INDEX) return false;
        return seqPlyState[idx];
    }

    // ---------- setters (state from callbacks) ----------

    void setMode(SequencerMode m) { mode_ = m; }

    void setState(SequencerState s) {
        state_ = s;
        if (s == SequencerState::Idle) {
            mode_      = SequencerMode::None;
            nextMode_  = SequencerMode::None;
            nextState_ = SequencerState::Idle;
        }
    }

    void setNextState(SequencerState s) { nextState_ = s; }
    void setNextMode(SequencerMode m)   { nextMode_ = m; }
    void setBar(uint8_t b)              { bar_ = b; }

    void setNoteCount(uint8_t count)    { noteCount_ = count; }

    // ---------- setters (user actions → engine) ----------

    void setLoopActive(bool v) {
        loopActive_ = v;
        if (engine_) engine_->setLoop(v);
    }

    void setSeqLength(uint8_t len) {
        if (len > Limits::MAX_SEQ_LENGTH) len = Limits::MAX_SEQ_LENGTH;
        seqLength_ = len;
        if (engine_) engine_->setLength(len);
    }

    void setClickActive(bool v) {
        clickActive_ = v;
        if (engine_) engine_->setClickAudible(v);
    }

    void setBpm(float v) {
        if (v < Limits::MIN_BPM) v = Limits::MIN_BPM;
        if (v > Limits::MAX_BPM) v = Limits::MAX_BPM;
        bpm_ = v;
        if (engine_) engine_->setClickBpm(v);
    }

    void setQuantization(Quantization q) {
        quantization_ = q;
        if (engine_) engine_->setQuantization(q);
    }

    void setSoloMode(bool v) {
        soloMode_ = v;
        for (uint8_t i = 0; i <= Limits::MAX_SEQ_INDEX; i++) {
            seqPlyState[i] = v ? (i == recSequence_) : true;
            if (engine_) engine_->selectDirect(i, seqPlyState[i]);
        }
    }

    void setSequence(uint8_t seq) {
        if (seq > Limits::MAX_SEQ_INDEX) seq = Limits::MAX_SEQ_INDEX;
        plySequence_ = seq;
        if (engine_) engine_->select(seq);
    }

    void setSequenceRecord(uint8_t seq) {
        if (seq > Limits::MAX_SEQ_INDEX) seq = Limits::MAX_SEQ_INDEX;
        recSequence_ = seq;
        if (engine_) engine_->selectSeqRecord(seq);
    }

    void setSequenceEnabled(uint8_t seq, bool enabled) {
        if (seq > Limits::MAX_SEQ_INDEX) return;
        seqPlyState[seq] = enabled;
        if (engine_) engine_->selectDirect(seq, enabled);
    }

    // ---------- navigation ----------

    void useNextSequence() {
        recSequence_++;
        if (recSequence_ > Limits::MAX_SEQ_INDEX) recSequence_ = 0;
        if (engine_) engine_->selectSeqRecord(recSequence_);
        if (soloMode_) applySoloMute();
    }

    void usePrevSequence() {
        if (recSequence_ == 0)
            recSequence_ = Limits::MAX_SEQ_INDEX;
        else
            recSequence_--;
        if (engine_) engine_->selectSeqRecord(recSequence_);
        if (soloMode_) applySoloMute();
    }

    // ---------- scenes ----------
    // One scene sounds at a time, each with its own length; selecting another
    // queues it for the loop end. An engine without scenes answers with the
    // interface defaults and these become no-ops, which is why the UI can call
    // them unconditionally.

    void    selectScene(uint8_t s)   { if (engine_) engine_->selectScene(s); }
    uint8_t getActiveScene()  const  { return engine_ ? engine_->activeScene() : 0; }
    uint8_t getPendingScene() const  { return engine_ ? engine_->pendingScene() : 0; }
    void    setSceneBars(uint8_t s, uint8_t bars) { if (engine_) engine_->setSceneBars(s, bars); }
    uint8_t getSceneBars(uint8_t s) const { return engine_ ? engine_->sceneBars(s) : 0; }
    uint8_t getSceneCount() const { return engine_ ? engine_->sceneCount() : 1; }
    bool    addScene()    { return engine_ ? engine_->addScene() : false; }
    bool    removeScene(uint8_t s) { return engine_ ? engine_->removeScene(s) : false; }
    uint16_t getSceneEventCount(uint8_t s) const { return engine_ ? engine_->sceneEventCount(s) : 0; }
    bool    getSceneEvent(uint8_t s, uint16_t i, float& at, uint8_t& pad, uint8_t& vel) const {
        return engine_ ? engine_->sceneEvent(s, i, at, pad, vel) : false;
    }
    float   getSceneEventLength(uint8_t s, uint16_t i) const {
        return engine_ ? engine_->sceneEventLength(s, i) : 0.0f;
    }
    bool    doubleScene(uint8_t s) { return engine_ ? engine_->doubleScene(s) : false; }
    bool    halveScene(uint8_t s)  { return engine_ ? engine_->halveScene(s) : false; }
    void    setErasing(bool on)    { if (engine_) engine_->setErasing(on); }
    bool    isErasing() const      { return engine_ ? engine_->erasing() : false; }
    float   getProgress() const    { return engine_ ? engine_->progress() : 0.0f; }
    void    setCountIn(bool on)    { if (engine_) engine_->setCountIn(on); }
    bool    getCountIn() const     { return engine_ && engine_->countIn(); }
    uint8_t getCountInLeft() const { return engine_ ? engine_->countInLeft() : 0; }
    uint8_t getCountInBeats() const { return engine_ ? engine_->countInBeats() : 0; }
    void    setSwing(uint8_t pct)   { if (engine_) engine_->setSwing(pct); }
    uint8_t getSwing() const        { return engine_ ? engine_->swing() : 0; }
    void    setSwingPerBeat(uint8_t n) { if (engine_) engine_->setSwingPerBeat(n); }
    uint8_t getSwingPerBeat() const { return engine_ ? engine_->swingPerBeat() : 2; }

    // ---------- song ----------
    uint8_t getSongLength() const  { return engine_ ? engine_->songLength() : 0; }
    bool    getSongBlock(uint8_t i, uint8_t& scene, uint8_t& repeats) const {
        return engine_ ? engine_->songBlock(i, scene, repeats) : false;
    }
    bool    songInsert(uint8_t i, uint8_t scene, uint8_t repeats) {
        return engine_ ? engine_->songInsert(i, scene, repeats) : false;
    }
    bool    songRemove(uint8_t i)  { return engine_ ? engine_->songRemove(i) : false; }
    bool    songSet(uint8_t i, uint8_t scene, uint8_t repeats) {
        return engine_ ? engine_->songSet(i, scene, repeats) : false;
    }
    void    setSongMode(bool on)   { if (engine_) engine_->setSongMode(on); }
    bool    getSongMode() const    { return engine_ ? engine_->songMode() : false; }
    void    setSongLoop(bool on)   { if (engine_) engine_->setSongLoop(on); }
    bool    getSongLoop() const    { return engine_ ? engine_->songLoop() : true; }
    uint8_t getSongPosition() const { return engine_ ? engine_->songPosition() : 0; }
    uint8_t getSongPass() const    { return engine_ ? engine_->songPass() : 0; }
    void    songLocate(uint8_t i)  { if (engine_) engine_->songLocate(i); }

    /// Beat within the bar, 0-based. What a metronome display counts.
    int getClickPos() const { return engine_ ? engine_->getClickPos() : 0; }

    // ---------- transport ----------

    /// Plays from where the last stop left the clock.
    void playback() { if (engine_) engine_->playback(); }
    /// Back to bar one, stopped. What a second STOP does.
    void rewind()   { if (engine_) engine_->resetClick(); }
    /* The click is the player's switch (the top bar's metronome, the
     * Sequencer's M): no transport command turns it on or off behind it. A
     * count-in counts whatever it is set to. */
    void record()   { if (engine_) engine_->record(); }
    void overdub()  { if (engine_) engine_->overdub(); }

    void reset() {
        if (engine_) {
            engine_->resetClick();
            engine_->reset();
        }
    }

    void stop() {
        if (engine_) engine_->stop();
    }

    void eraseSeq(int8_t idx = -1) {
        if (!engine_) return;
        if (idx < 0 || idx > Limits::MAX_SEQ_INDEX) {
            engine_->clearAll(0, 127);
        } else {
            engine_->clearSeq(idx, 127);
        }
    }

    // Public state array (backward compat with GUI code that accesses directly)
    bool seqPlyState[Limits::MAX_SEQ_INDEX + 1] = {true};

private:
    void applySoloMute() {
        for (uint8_t i = 0; i <= Limits::MAX_SEQ_INDEX; i++) {
            seqPlyState[i] = (i == recSequence_);
            if (engine_) engine_->selectDirect(i, seqPlyState[i]);
        }
    }

    ISequencerEngine* engine_ = nullptr;
    bool              enabled_ = false;
    SequencerMode     mode_      = SequencerMode::None;
    SequencerState    state_     = SequencerState::Idle;
    SequencerMode     nextMode_  = SequencerMode::None;
    SequencerState    nextState_ = SequencerState::Idle;
    uint8_t           bar_        = 0;
    uint8_t           seqLength_  = 4;
    bool              clickActive_ = true;
    bool              loopActive_  = true;
    uint8_t           loopLength_  = 0;
    float             bpm_         = 120.0f;
    uint8_t           plySequence_ = 0;
    uint8_t           recSequence_ = 0;
    Quantization      quantization_ = Limits::DEFAULT_QUANTIZATION;
    uint8_t           noteCount_   = 0;
    bool              soloMode_    = true;
};

} // namespace crosspad
