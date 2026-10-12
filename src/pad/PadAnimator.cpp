#include "crosspad/pad/PadAnimator.hpp"
#include "crosspad/led/HslColor.hpp"
#include <cstdio>
#include <cmath>

namespace crosspad {

void PadAnimator::init(ILedStrip& strip, IClock& clock) {
    strip_ = &strip;
    clock_ = &clock;
}

void PadAnimator::update() {
    CompletionCallback done = nullptr;
    bool justFinished = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || !strip_) return;

        strip_->lock();
        switch (type_) {
            case AnimType::PROGRESS: renderProgress(); break;
            case AnimType::SUCCESS:  renderSuccess();  break;
            case AnimType::ERROR:    renderError();    break;
            case AnimType::BOOT:     renderBoot();     break;
            case AnimType::CHARGING: renderCharging(); break;
            case AnimType::FRAME:    renderFrame();    break;
            default: break;
        }
        justFinished = !active_; // finish() may have been called during render
        strip_->unlock();

        if (justFinished) {
            done = completionCb_;
            completionCb_ = nullptr;
        }
    }

    // Outside both locks: the LED restore takes the strip lock, and the
    // completion callback may start the next animation -- then the pads stay
    // animated and nothing ended.
    if (done) done();
    if (justFinished && !isActive()) notifyAnimState(false);
}

bool PadAnimator::isActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

bool PadAnimator::owns(AnimToken token) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return token != kNoAnim && token == token_;
}

PadAnimator::AnimToken PadAnimator::beginLocked(AnimType type) {
    type_ = type;
    active_ = true;
    if (++lastToken_ == kNoAnim) ++lastToken_;
    token_ = lastToken_;
    return token_;
}

bool PadAnimator::endLocked() {
    if (!active_) return false;
    active_ = false;
    type_ = AnimType::NONE;
    token_ = kNoAnim;
    completionCb_ = nullptr;
    return true;
}

PadAnimator::AnimToken PadAnimator::startProgress(RgbColor color) {
    AnimToken token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        animColor_ = color;
        currentProgress_ = 0.0f;
        token = beginLocked(AnimType::PROGRESS);

        // Render initial state (all black)
        strip_->lock();
        for (int i = 0; i < kPadCount; i++) {
            setPixel(i, RgbColor(0, 0, 0));
        }
        refresh();
        strip_->unlock();
    }
    notifyAnimState(true);
    return token;
}

void PadAnimator::setProgress(float progress) {
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    std::lock_guard<std::mutex> lock(mutex_);
    currentProgress_ = progress;
}

PadAnimator::AnimToken PadAnimator::showFrame(const RgbColor (&colors)[kPadCount]) {
    AnimToken token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < kPadCount; i++) frame_[i] = colors[i];
        token = beginLocked(AnimType::FRAME);
    }
    notifyAnimState(true);
    return token;
}

bool PadAnimator::updateFrame(AnimToken token, const RgbColor (&colors)[kPadCount]) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (token == kNoAnim || token != token_) return false;
    for (int i = 0; i < kPadCount; i++) frame_[i] = colors[i];
    return true;
}

void PadAnimator::renderFrame() {
    for (int i = 0; i < kPadCount; i++) setPixel(i, frame_[i]);
    refresh();
}

PadAnimator::AnimToken PadAnimator::begin(AnimType type) {
    AnimToken token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        startTime_ = clock_->getTimeUs();
        token = beginLocked(type);
    }
    notifyAnimState(true);
    return token;
}

PadAnimator::AnimToken PadAnimator::playSuccess(uint32_t fadeMs) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        duration_ = fadeMs;
    }
    return begin(AnimType::SUCCESS);
}

PadAnimator::AnimToken PadAnimator::playError(uint32_t holdMs) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        duration_ = holdMs;
    }
    return begin(AnimType::ERROR);
}

PadAnimator::AnimToken PadAnimator::playBootAnim(uint32_t durationMs) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!strip_) return kNoAnim;
        duration_ = durationMs;
    }
    return begin(AnimType::BOOT);
}

PadAnimator::AnimToken PadAnimator::startCharging(float soc) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        chargeSoc_ = (soc < 0.0f) ? 0.0f : (soc > 1.0f ? 1.0f : soc);
        chargingActive_ = true;     // assume charging until the caller says otherwise
    }
    return begin(AnimType::CHARGING);
}

void PadAnimator::setChargePercent(float soc) {
    std::lock_guard<std::mutex> lock(mutex_);
    chargeSoc_ = (soc < 0.0f) ? 0.0f : (soc > 1.0f ? 1.0f : soc);
}

void PadAnimator::setChargingActive(bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    chargingActive_ = active;
}

bool PadAnimator::stop(AnimToken token) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (token == kNoAnim || token != token_ || !endLocked()) return false;
    }
    notifyAnimState(false);
    return true;
}

void PadAnimator::stopAll() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!endLocked()) return;
    }
    notifyAnimState(false);
}

void PadAnimator::setCompletionCallback(CompletionCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    completionCb_ = cb;
}

void PadAnimator::setAnimationStateCallback(AnimationStateCallback cb) {
    animStateCb_ = cb;
}

void PadAnimator::setPixel(uint8_t padIdx, RgbColor color) {
    if (padIdx >= kPadCount || !strip_) return;
    strip_->setPixel(padIdx, color);
}

void PadAnimator::refresh() {
    if (strip_) {
        strip_->refresh();
    }
}

void PadAnimator::finish() {
    // Inside mutex_ and the strip lock: update() runs the completion callback
    // and the state notification once both are released.
    active_ = false;
    type_ = AnimType::NONE;
    token_ = kNoAnim;
}

void PadAnimator::notifyAnimState(bool active) {
    if (animStateCb_) {
        animStateCb_(active);
    }
}

// ── Progress bar rendering ──────────────────────────────────────────────

void PadAnimator::renderProgress() {
    float filledCount = currentProgress_ * kPadCount;
    int fullPads = (int)filledCount;
    float fractional = filledCount - fullPads;

    for (int i = 0; i < kPadCount; i++) {
        if (i < fullPads) {
            setPixel(i, animColor_);
        } else if (i == fullPads) {
            uint8_t dim = (uint8_t)(fractional * 255.0f);
            setPixel(i, animColor_.Dim(dim));
        } else {
            setPixel(i, RgbColor(0, 0, 0));
        }
    }
    refresh();
}

// ── Success rendering (green fade-out) ──────────────────────────────────

void PadAnimator::renderSuccess() {
    int64_t elapsed = clock_->getTimeUs() - startTime_;
    float t = (float)elapsed / ((float)duration_ * 1000.0f); // duration is ms, timer is us

    if (t >= 1.0f) {
        for (int i = 0; i < kPadCount; i++) {
            setPixel(i, RgbColor(0, 0, 0));
        }
        refresh();
        finish();
        return;
    }

    // Exponential fade-out: brightness = (1 - t)^2
    float brightness = (1.0f - t) * (1.0f - t);
    uint8_t bri = (uint8_t)(brightness * 255.0f);
    RgbColor green(0, bri, 0);

    for (int i = 0; i < kPadCount; i++) {
        setPixel(i, green);
    }
    refresh();
}

// ── Error rendering (red flash 3x) ─────────────────────────────────────

void PadAnimator::renderError() {
    int64_t elapsedUs = clock_->getTimeUs() - startTime_;
    int64_t elapsedMs = elapsedUs / 1000;

    // 3 cycles of 200ms on + 200ms off = 1200ms total
    int cycle = (int)(elapsedMs / 400);
    int phase = (int)((elapsedMs % 400) < 200 ? 0 : 1); // 0=on, 1=off

    if (cycle >= 3) {
        for (int i = 0; i < kPadCount; i++) {
            setPixel(i, RgbColor(0, 0, 0));
        }
        refresh();
        finish();
        return;
    }

    RgbColor color = (phase == 0) ? RgbColor(255, 0, 0) : RgbColor(0, 0, 0);
    for (int i = 0; i < kPadCount; i++) {
        setPixel(i, color);
    }
    refresh();
}

// ── Boot animation (rainbow wipe + breathe) ────────────────────────────

static RgbColor rainbowColor(int padIdx) {
    float hue = (float)padIdx / PadAnimator::kPadCount * 360.0f;
    HslColor hsl(hue, 1.0f, 0.5f);
    return RgbColor(hsl);
}

void PadAnimator::renderBoot() {
    int64_t elapsedUs = clock_->getTimeUs() - startTime_;
    int64_t elapsedMs = elapsedUs / 1000;

    if (elapsedMs >= (int64_t)duration_) {
        for (int i = 0; i < kPadCount; i++) {
            setPixel(i, RgbColor(0, 0, 0));
        }
        refresh();
        finish();
        return;
    }

    // Phase 1 (0 to 66% of duration): sequential rainbow wipe
    int64_t wipeEnd = (int64_t)(duration_ * 2 / 3);
    int64_t breatheStart = wipeEnd;

    if (elapsedMs < wipeEnd) {
        float wipeFraction = (float)elapsedMs / (float)wipeEnd;
        float wipePad = wipeFraction * kPadCount;

        for (int i = 0; i < kPadCount; i++) {
            if ((float)i <= wipePad) {
                float fadeFactor = 1.0f;
                if ((float)i > wipePad - 1.0f && (float)i <= wipePad) {
                    fadeFactor = wipePad - (float)i;
                    if (fadeFactor > 1.0f) fadeFactor = 1.0f;
                }
                RgbColor c = rainbowColor(i);
                setPixel(i, c.Dim((uint8_t)(fadeFactor * 255.0f)));
            } else {
                setPixel(i, RgbColor(0, 0, 0));
            }
        }
    } else {
        // Phase 2: all pads rainbow, fading out
        int64_t breatheElapsed = elapsedMs - breatheStart;
        int64_t breatheDuration = (int64_t)duration_ - breatheStart;
        float t = (float)breatheElapsed / (float)breatheDuration;
        if (t > 1.0f) t = 1.0f;

        uint8_t bri = (uint8_t)((1.0f - t) * 255.0f);

        for (int i = 0; i < kPadCount; i++) {
            RgbColor c = rainbowColor(i);
            setPixel(i, c.Dim(bri));
        }
    }
    refresh();
}

// ── Charge-while-off gauge ──────────────────────────────────────────────
//
// Battery gauge on the 4x4 grid. Pad index 0 = bottom-left, 15 = top-right
// (visualRow = i/4, 0 = bottom). Lit set = pads 0..N-1 with N = round(soc*16),
// so the fill rises row by row, left→right, from the bottom.
//
//   • color: red (empty) → green (full), one uniform hue per frame = f(SoC).
//   • body (lit, non-frontier): a soft concentric glow blob drifts over the
//     grid on two incommensurate sines and lifts local brightness ~±12%.
//   • frontier (the single highest lit pad, only when N < 16): breathes fully
//     to 0 on top of the body color.
//   • full (soc == 1.0 → N == 16): all green + drift, no frontier breathing.
//
// Persistent: never calls finish(); runs until stop().
void PadAnimator::renderCharging() {
    // chargeSoc_ is already smoothed (EMA) by the caller, so a plain round is
    // stable — no extra hysteresis here.
    const float soc = chargeSoc_;
    int litCount = (int)(soc * (float)kPadCount + 0.5f);
    if (litCount < 0) litCount = 0;
    if (litCount > kPadCount) litCount = kPadCount;

    const RgbColor base = RgbColor::LinearBlend(RgbColor(255, 0, 0),
                                                RgbColor(0, 255, 0), soc);
    const int frontier = (litCount < kPadCount) ? (litCount - 1) : -1;

    // Plugged but NOT actually charging (gate off / data port / full / fault):
    // static gauge — solid level + colour, no breathing frontier, no drift glow.
    // Set via setChargingActive(false); the screen shows "Not charging".
    if (!chargingActive_) {
        for (int i = 0; i < kPadCount; i++) {
            setPixel(i, (i < litCount) ? base : RgbColor(0, 0, 0));
        }
        refresh();
        return;
    }

    const float t = (float)(clock_->getTimeUs() - startTime_) / 1000000.0f;

    // Drifting concentric glow centre, in grid coords (col 0..3, visualRow 0..3).
    const float bx = 1.5f + 1.5f * sinf(t * 0.37f);
    const float by = 1.5f + 1.5f * sinf(t * 0.29f + 1.3f);

    // Frontier breathing 0→1 (deep), ~2.6 s period.
    const float breath = 0.5f * (1.0f - cosf(t * (6.2831853f / 2.6f)));

    for (int i = 0; i < kPadCount; i++) {
        if (i >= litCount) {
            setPixel(i, RgbColor(0, 0, 0));
            continue;
        }
        if (i == frontier) {
            setPixel(i, base.Dim((uint8_t)(breath * 255.0f)));
            continue;
        }
        // Body pad: baseline 0.90, +0.12 near the drifting blob (→ 0.90..1.02).
        const float col = (float)(i % 4);
        const float vrow = (float)(i / 4);
        const float dx = col - bx;
        const float dy = vrow - by;
        const float dist = sqrtf(dx * dx + dy * dy);
        float fall = 1.0f - dist / 2.2f;
        if (fall < 0.0f) fall = 0.0f;
        float factor = 0.90f + 0.12f * fall;
        if (factor > 1.0f) factor = 1.0f;
        setPixel(i, base.Dim((uint8_t)(factor * 255.0f)));
    }
    refresh();
}

} // namespace crosspad
