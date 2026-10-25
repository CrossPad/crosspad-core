// SPDX-License-Identifier: GPL-3.0-or-later
//
// The audio song engine: projects in the platform's songs root, bouncing a
// sequencer scene to a WAV, and streaming four tracks of clips back into the
// mixer.
//
// Three threads touch it and each has one job:
//   render     takes the pads-only bus into the bounce ring, and plays the
//              song ring out (mix()). Never blocks, never touches storage.
//   worker     writes the bounce ring to storage, and keeps the song ring
//              filled -- the four tracks read and mixed down, in song time.
//   UI         edits the song, asks for bounces and playback.
// The rings between them are single-producer/single-consumer, so neither side
// waits on the other; storage work yields to whatever else the platform must
// read in time (ISongEnginePlatform::yieldIo).
//
// Everything storage does -- open, list, save, play, stop, bounce -- is a
// request to the worker; no public call waits for it. Only the worker
// allocates and frees the rings, and before it frees one it waits for the
// render to leave mix()/rtCapture() (rtGen_), so a Play pressed right after
// Stop can never hand the render a ring that is being freed.

#include "SongEngineImpl.hpp"

namespace crosspad {

static_assert(AudioSong::kSeqPpq == PatternSequencer::kPpq,
              "a song's seq notes and a scene's events count the same ticks");
static_assert(AudioSong::kSeqBlockBars == PatternSequencer::kMaxBars &&
              AudioSong::kSeqBlockNotes == PatternSequencer::kMaxEvents,
              "a block a take lays must fit the scene the seq view opens it in");

SongEngine::Impl::Impl(ISongEnginePlatform& platform, MetronomeNode& clock, PatternSequencer& sequencer)
    : p(platform), clk_(clock), seq_(sequencer), sz(checkedSizes(platform.sizes())) {
    ring_.platform = &p;
    bRing_.platform = &p;
}

/// Rings no smaller than what they start with; a chunk longer than the
/// WAV header.
SongEngineSizes SongEngine::Impl::checkedSizes(SongEngineSizes s) {
    s.bounceRingFrames = std::max(s.bounceRingFrames, kBounceMinFrames);
    s.songRingFrames = std::max(s.songRingFrames, kPrimeFrames);
    s.chunkFrames = std::max(s.chunkFrames, WavWriter::kHeaderBytes / 4 + 1);
    return s;
}

/// printf into the platform's log.
void SongEngine::Impl::logf(bool warning, const char* fmt, ...) {
    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    p.log(warning, msg);
}

/// The worker is told to quit and joined. Only a platform that destroys
/// the engine (PC, tests) gets here.
void SongEngine::Impl::shutdown() {
    if (!workerStarted_.load()) return;
    quit_.store(true);
    p.wake();
    p.joinWorker();
}

// ── IAudioNode (render) ───────────────────────────────────────────────
void SongEngine::Impl::onPrepare(uint32_t maxFrames, uint32_t sampleRate) {
    sampleRate_ = sampleRate ? sampleRate : 44100;
    scratch_.assign(maxFrames * 2, 0);
    capture_.assign(maxFrames * 2, 0);
}

/// The worker and its card buffer, on first use rather than at boot: boot
/// is when PSRAM is tightest (the kit scan grows its list in one block),
/// and a board that never opens a song should not pay for one.
bool SongEngine::Impl::ensureWorker() {
    if (workerStarted_.load()) return true;
    std::lock_guard<std::mutex> l(workerMutex_);
    if (workerStarted_.load()) return true;
    /* The one card buffer the worker uses, for whichever job it is on,
     * and the sum the tracks are mixed into. */
    if (!io_) io_ = static_cast<int16_t*>(p.allocScratch(sz.chunkFrames * 4));
    if (!acc_) acc_ = static_cast<int32_t*>(p.allocScratch(sz.chunkFrames * 8));
    if (!io_ || !acc_) return false;
    workerStarted_.store(p.startWorker(worker_entry, this));
    return workerStarted_.load();
}

bool SongEngine::Impl::request(uint8_t list) {
    if (!ensureWorker()) return false;
    listReq_.fetch_or(list);
    p.wake();
    return true;
}

/// One edit at a time: the caller that turns editState_ Busy owns the
/// edit parameters until the worker has finished with them.
bool SongEngine::Impl::claimEdit() {
    SongOpState s = editState_.load();
    do {
        if (s == SongOpState::Busy) return false;
    } while (!editState_.compare_exchange_weak(s, SongOpState::Busy));
    return true;
}

/// Publish a claimed edit; its parameters are written before this.
bool SongEngine::Impl::requestEdit(uint8_t edit) {
    yieldFreeze();          // or the edit waits out the whole freeze
    editReq_.store(edit);
    p.wake();
    return true;
}

/// Worker: wait until a render that may have seen the old state has left
/// mix()/rtCapture(). Call after publishing the state that keeps the next
/// render out (play_ Idle, bArmed_ false).
void SongEngine::Impl::waitRtOut() {
    const uint32_t g = rtGen_.load(std::memory_order_seq_cst);
    if (!(g & 1u)) return;
    while (rtGen_.load(std::memory_order_seq_cst) == g) p.sleepMs(1);
}

void SongEngine::Impl::worker() {
    while (!quit_.load()) {
        const bool streaming = job_.load() == Job::Play && play_.load() != Play::Idle;
        /* Waveforms are worked out only while nothing plays, one after
         * another without waiting. */
        const bool peaking = !streaming && peaksPending() && p.storageReady();
        /* Stretched copies likewise, after the waveforms. */
        const bool stretching = !streaming && !peaking && stretchPending() && p.storageReady();
        /* Element bakes from the card after them, also only while nothing
         * plays: one waiting while the song streams is no reason to skip the
         * wait. */
        const bool baking = !streaming && !peaking && !stretching && diskBakePending();
        /* Until a project is open, look for the card once a second; with a
         * save waiting, back in time to make it. */
        /* A request made while the last one was handled has had its wake-up
         * used: it runs now, not at the next one. A save due and the
         * elements' list are no such requests while the song streams: the
         * loop leaves them until the stop. */
        if (!requestWaiting(streaming))
            p.waitWake(streaming ? kStreamPollMs
                                 : peaking || stretching || baking ? 0
                                 : saveReq_.load() ? kSaveDelayMs : (lastTried_ ? UINT32_MAX : kCardPollMs));
        if (quit_.load()) break;

        if (releaseReq_.load()) {
            /* A bounce still queued would allocate into the arena being
             * wiped; it is refused, the one running was cancelled. */
            if (bounceReq_.exchange(false)) {
                setStatus(BounceState::Failed, "cancelled: the kit's engine changed");
                busy_.store(false);
            }
            stopReq_.store(false);
            endPlayback();
            releaseReq_.store(false);
            continue;
        }
        if (stopReq_.exchange(false) || play_.load() == Play::Ended) {
            endPlayback();
            continue;
        }
        if (liveStartReq_.exchange(false)) {
            startLive();
            continue;
        }
        if (playReq_.exchange(false)) {
            startPlayback(playFrom_.load());
            /* A take armed for this playback is its; one that never
             * started (no memory for the ring) is called off. */
            if (seqTake_.load() != SyncTake::None) {
                takeBound_ = play_.load() != Play::Idle;
                if (!takeBound_) {
                    std::lock_guard<std::recursive_mutex> l(songMutex_);
                    closeTake();
                }
            }
            continue;
        }
        if (openReq_.exchange(false)) {
            std::string name;
            bool remember = true;
            {
                std::lock_guard<std::recursive_mutex> l(songMutex_);
                name = openName_;
                remember = openRemember_;
            }
            /* A project asked for by name is the one open: opening the
             * last one afterwards would read it back over unsaved edits. */
            lastTried_ = true;
            if (saveReq_.exchange(false)) doSave();   // the edits waiting go to the song they were made in
            openState_.store(doOpen(name, remember));
            continue;
        }
        if (const uint8_t edit = editReq_.exchange(0)) {
            editState_.store(edit == kEditRename ? doRename()
                           : edit == kEditDelete ? doDelete()
                           : edit == kEditTempo  ? doTempo(editBpm_.load())
                           : edit == kEditImport ? doImport()
                           : edit == kEditExport ? doExport()
                           : edit == kEditBake   ? doBakeElement()
                           : edit == kEditElementRegion ? doElementFromRegion()
                           : edit == kEditElementSong   ? doElementFromSong()
                           : edit == kEditElementDiscard ? doElementDiscard()
                           : edit == kEditSnapshot       ? doSnapshot()
                           : edit == kEditRestore        ? doRestore()
                           : edit == kEditDropSnapshot   ? doDropSnapshot()
                           : edit == kEditEditHash       ? doEditHash()
                           : doMerge(editMask_.load(), editFrom_.load(), editTo_.load()));
            continue;
        }
        const uint8_t held = streaming ? kListsAfterStop : 0;
        if (const uint8_t lists = listReq_.fetch_and(held) & (uint8_t)~held) {
            if (lists & kListProjects) doListProjects();
            if (lists & kListClips)    doListClips();
            if (lists & kListElements) doListElements();
            if (lists & kListUses)     doListUses();
            listGen_.fetch_add(1);
            continue;
        }
        if (!lastTried_) openLast();
        /* Not while it streams: a song.json written on this card takes the
         * worker off the ring for longer than the ring lasts. The save
         * waits for the stop, which is never far off on a loop. */
        if (!streaming && saveReq_.load() && p.nowUs() >= saveAtUs_.load() && saveReq_.exchange(false)) doSave();
        /* The relink pass reads every element's song.json: like the elements'
         * list, it waits for the stop. */
        if (!streaming && relinkReq_.load() && p.storageReady()) {
            doRelink();
            continue;
        }
        if (bounceReq_.exchange(false)) {
            if (bFreeze_) runFreeze();
            else runBounce();
            busy_.store(false);
            continue;
        }
        if (job_.load() == Job::Play && play_.load() != Play::Idle) {
            serviceTake();
            fillRings();
        }
        /* The song around the element being edited first: it is what the
         * player hears next. */
        else if (contextPending()) doContext();
        else if (peaksPending() && p.storageReady()) doPeaks();
        else if (stretchPending() && p.storageReady()) doStretch();
        else if (diskBakePending()) doDiskBake();
    }
}

/// Worker: anything asked for that goes before a waveform -- while the song
/// @p streaming, not what waits for the stop.
bool SongEngine::Impl::requestWaiting(bool streaming) const {
    const uint8_t held = streaming ? kListsAfterStop : 0;
    return quit_.load() || releaseReq_.load() || stopReq_.load() || playReq_.load() || openReq_.load() ||
           editReq_.load() != 0 || (listReq_.load() & (uint8_t)~held) != 0 || bounceReq_.load() ||
           (!streaming && saveReq_.load() && p.nowUs() >= saveAtUs_.load()) ||
           (!streaming && relinkReq_.load() && p.storageReady()) ||
           (!streaming && ctxPending_.load() && p.storageReady());
}

SongEngine::SongEngine(ISongEnginePlatform& platform, MetronomeNode& clock, PatternSequencer& seq)
    : impl_(std::make_unique<Impl>(platform, clock, seq)) {}

SongEngine::~SongEngine()
{
    impl_->shutdown();
}

const char* SongEngine::name() const { return "Song"; }
bool SongEngine::isGenerator() const { return true; }
void SongEngine::onPrepare(uint32_t maxFrames, uint32_t sr) { impl_->onPrepare(maxFrames, sr); }
void SongEngine::onSampleRateChange(uint32_t sr) { impl_->onSampleRateChange(sr); }
void SongEngine::mix(float* out, uint32_t frames) { impl_->mix(out, frames); }
void SongEngine::rtCapture(const float* padsBus, uint32_t frames) { impl_->rtCapture(padsBus, frames); }
bool SongEngine::capturing() const { return impl_->capturing(); }
bool SongEngine::captureBacklogged() const { return impl_->captureBacklogged(); }
void SongEngine::releaseRings() { impl_->releaseRings(); }
const char* SongEngine::debugLine() const { return impl_->debugLine(); }
bool SongEngine::liveStart() { return impl_->liveStart(); }
bool SongEngine::liveLaunch(uint8_t track, const std::string& file, uint16_t bars)
{ return impl_->liveLaunch(track, file, bars); }
bool SongEngine::liveStop(uint8_t track) { return impl_->liveStop(track); }
void SongEngine::liveQuant(uint8_t bars) { impl_->liveQuant(bars); }
bool SongEngine::liveState(LiveState& out) { return impl_->liveState(out); }
bool SongEngine::requestProjects() { return impl_->requestProjects(); }
std::vector<std::string> SongEngine::projects() { return impl_->projects(); }
bool SongEngine::requestOpen(const std::string& name) { return impl_->requestOpen(name); }
SongOpState SongEngine::openState() const { return impl_->openState(); }
AudioSong& SongEngine::song() { return impl_->song(); }
std::recursive_mutex& SongEngine::songMutex() { return impl_->songMutex(); }
void SongEngine::commit() { impl_->commit(); }
bool SongEngine::requestClips() { return impl_->requestClips(); }
std::vector<ClipInfo> SongEngine::clips() { return impl_->clips(); }
uint32_t SongEngine::listGeneration() const { return impl_->listGeneration(); }
bool SongEngine::bounceScene(uint8_t scene, uint8_t track) { return impl_->bounceScene(scene, track); }
bool SongEngine::bounceSeqTrack(uint8_t track) { return impl_->bounceSeqTrack(track); }
bool SongEngine::freezeTrack(uint8_t track) { return impl_->freezeTrack(track); }
FreezeState SongEngine::freezeState(uint8_t track) { return impl_->freezeState(track); }
uint32_t SongEngine::playEndBars() { return impl_->playEndBars(); }
BounceStatus SongEngine::bounceStatus() { return impl_->bounceStatus(); }
void SongEngine::cancelBounce() { impl_->cancelBounce(); }
bool SongEngine::requestRename(const std::string& name) { return impl_->requestRename(name); }
bool SongEngine::requestDelete() { return impl_->requestDelete(); }
bool SongEngine::requestTempo(float bpm) { return impl_->requestTempo(bpm); }
SongOpState SongEngine::editState() const { return impl_->editState(); }
bool SongEngine::requestMerge(uint8_t mask, uint32_t fromBar, uint32_t toBar) { return impl_->requestMerge(mask, fromBar, toBar); }
uint8_t SongEngine::editProgress() const { return impl_->editProgress(); }
uint32_t SongEngine::stretchWaiting() const { return impl_->stretchWaiting(); }
void SongEngine::trackLevels(int track, float& left, float& right) { impl_->trackLevels(track, left, right); }
bool SongEngine::undo() { return impl_->undo(); }
bool SongEngine::redo() { return impl_->redo(); }
uint32_t SongEngine::undoSteps() const { return impl_->undoSteps(); }
uint32_t SongEngine::redoSteps() const { return impl_->redoSteps(); }
uint8_t SongEngine::stretchProgress() const { return impl_->stretchProgress(); }
bool SongEngine::requestExportLiveSet() { return impl_->requestExportLiveSet(); }
std::vector<std::string> SongEngine::editReport() { return impl_->editReport(); }
bool SongEngine::requestOpenElement(const std::string& ref) { return impl_->requestOpenElement(ref); }
bool SongEngine::requestElements() { return impl_->requestElements(); }
std::vector<ElementInfo> SongEngine::elements() { return impl_->elements(); }
bool SongEngine::requestBakeElement() { return impl_->requestBakeElement(); }
ElementBake SongEngine::bakeResult() { return impl_->bakeResult(); }
bool SongEngine::requestElementFromRegion(uint32_t fromBar, uint32_t toBar) { return impl_->requestElementFromRegion(fromBar, toBar); }
bool SongEngine::requestElementFromSong(const std::string& name, const std::vector<std::string>& openElements) { return impl_->requestElementFromSong(name, openElements); }
bool SongEngine::requestElementDiscard(const std::string& ref) { return impl_->requestElementDiscard(ref); }
bool SongEngine::requestUses() { return impl_->requestUses(); }
std::vector<ElementUse> SongEngine::usesOf(const std::string& ref) { return impl_->usesOf(ref); }
bool SongEngine::requestSnapshot(const std::string& ref) { return impl_->requestSnapshot(ref); }
bool SongEngine::requestEditHash() { return impl_->requestEditHash(); }
uint64_t SongEngine::editHash() { return impl_->editHash(); }
bool SongEngine::requestRestore(const std::string& ref) { return impl_->requestRestore(ref); }
bool SongEngine::requestDropSnapshot(const std::string& ref) { return impl_->requestDropSnapshot(ref); }
bool SongEngine::snapshotPending(const std::string& ref) { return impl_->snapshotPending(ref); }
std::string SongEngine::elementMade() { return impl_->elementMade(); }
ElementClipState SongEngine::elementClipState(uint8_t track, size_t clip) { return impl_->elementClipState(track, clip); }
std::string SongEngine::elementClipWhy(uint8_t track, size_t clip) { return impl_->elementClipWhy(track, clip); }
uint8_t SongEngine::backgroundProgress() const { return impl_->backgroundProgress(); }
bool SongEngine::backgroundBusy() const { return impl_->backgroundBusy(); }
bool SongEngine::requestContext(const ContextRequest& r) { return impl_->requestContext(r); }
void SongEngine::contextSolo(bool on) { impl_->contextSolo(on); }
ContextState SongEngine::contextState() const { return impl_->contextState(); }
std::string SongEngine::contextWhy() { return impl_->contextWhy(); }
bool SongEngine::clipPeaks(const std::string& file, ClipPeaks& out) { return impl_->clipPeaks(file, out); }
uint32_t SongEngine::peaksGeneration() const { return impl_->peaksGeneration(); }
uint32_t SongEngine::framesPerBar() { return impl_->framesPerBar(); }
bool SongEngine::requestImport(const std::string& file, uint32_t start, uint32_t end,
                               uint8_t track, uint32_t bar, uint32_t lead)
{
    return impl_->requestImport(file, start, end, track, bar, lead);
}
bool SongEngine::play(uint32_t fromBar) { return impl_->playWithTake(fromBar, SyncTake::None); }
bool SongEngine::playWithTake(uint32_t fromBar, SyncTake take) { return impl_->playWithTake(fromBar, take); }
bool SongEngine::playCountedIn(uint32_t fromBar, SyncTake take, uint8_t beats)
{
    return impl_->playCountedIn(fromBar, take, beats);
}
uint8_t SongEngine::countInBeatsLeft() const { return impl_->countInBeatsLeft(); }
void SongEngine::stop() { impl_->stop(); }
bool SongEngine::playing() const { return impl_->playing(); }
double SongEngine::positionBars() const { return impl_->positionBars(); }
uint32_t SongEngine::framesPlayed() const { return impl_->framesPlayed(); }
uint32_t SongEngine::positionFrames() const { return impl_->positionFrames(); }
uint32_t SongEngine::underruns() const { return impl_->underruns(); }
uint32_t SongEngine::openFailures() const { return impl_->openFailures(); }
bool SongEngine::recordSeq(uint32_t fromBar, SyncTake take, uint8_t beats) { return impl_->recordSeq(fromBar, take, beats); }
bool SongEngine::seqPunch(SyncTake take) { return impl_->seqPunch(take); }
SyncTake SongEngine::seqTake() const { return impl_->seqTake(); }
bool SongEngine::seqPunchable() const { return impl_->seqPunchable(); }
bool SongEngine::openSeqEdit(uint8_t track, uint32_t bar, uint8_t scene) { return impl_->openSeqEdit(track, bar, scene); }
void SongEngine::syncSeqEdit() { impl_->syncSeqEdit(); }
void SongEngine::closeSeqEdit() { impl_->closeSeqEdit(); }
int32_t SongEngine::seqEditBar() const { return impl_->seqEditBar(); }

} // namespace crosspad
