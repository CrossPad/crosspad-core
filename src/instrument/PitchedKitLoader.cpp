// SPDX-License-Identifier: MIT
#include "crosspad/instrument/PitchedKitLoader.hpp"

#include "crosspad/dsp/FftResampler.hpp"
#include "crosspad/dsp/PitchDetector.hpp"
#include "crosspad/instrument/WavPcm.hpp"

#include <cstdarg>
#include <cstdio>
#include <new>

namespace crosspad {

namespace {

/// One log line, formatted. 96 bytes is the budget: long enough for a zone
/// index, a truncated sample name and three numbers, short enough to sit on
/// the stack of a loader task that also holds the resampler's scratch.
void logLine(const PitchedLoadEnv& env, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));
void logLine(const PitchedLoadEnv& env, const char* fmt, ...) {
    if (!env.log) return;
    char line[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    env.log(line);
}

void fail(char* err, size_t errLen, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void fail(char* err, size_t errLen, const char* fmt, ...) {
    if (!err || errLen == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errLen, fmt, ap);
    va_end(ap);
}

/// Byte counts are computed in 64 bits (a corrupt WAV header can overflow a
/// 32-bit product) but reported in 32; saturate rather than wrap a message.
unsigned asUnsigned(uint64_t v) {
    return (v > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<unsigned>(v);
}

} // namespace

bool pitchedBuildBank(const KitInfo& kit, const PitchedLoadEnv& env, IFft& fft,
                      SampleBank*& outBank, char* err, size_t errLen) {
    if (err && errLen) err[0] = '\0';

    if (!env.alloc || !env.free) {
        fail(err, errLen, "no allocator");
        return false;
    }
    if (env.engineRate == 0) {
        fail(err, errLen, "engine rate 0");
        return false;
    }
    if (kit.instrument.zones.empty()) {
        fail(err, errLen, "no zones");
        return false;
    }

    SampleBank* bank = new (std::nothrow) SampleBank(env.alloc, env.free);
    if (!bank) {
        fail(err, errLen, "no bank");
        return false;
    }

    uint8_t  zoneCount = 0;
    uint64_t used = 0;

    const size_t wanted = kit.instrument.zones.size();
    for (size_t i = 0; i < wanted && zoneCount < SampleBank::kMaxZones; ++i) {
        const InstrumentZoneSpec& spec = kit.instrument.zones[i];
        const std::string path = env.resolvePath ? env.resolvePath(kit.path, spec.sample)
                                                 : resolveKitSamplePath(kit.path, spec.sample);

        WavInfo info;
        if (!wavReadInfo(path.c_str(), info) || info.frames == 0) {
            fail(err, errLen, "zone %u: bad wav '%s'", (unsigned)i, spec.sample.c_str());
            delete bank;
            return false;
        }

        const bool needResample = (info.sampleRate != env.engineRate);
        const uint32_t outFrames = needResample
            ? resampledLength(info.frames, info.sampleRate, env.engineRate)
            : info.frames;
        if (outFrames == 0) {
            fail(err, errLen, "zone %u: empty", (unsigned)i);
            delete bank;
            return false;
        }

        // 64-bit: outFrames comes from a file header by way of
        // resampledLength(), and size_t is 32 bits on the device — a corrupt
        // header that overflowed the product would have passed the budget
        // check.
        const uint64_t zoneBytes = static_cast<uint64_t>(outFrames) * sizeof(int16_t);
        if (env.maxBankBytes != 0 && used + zoneBytes > env.maxBankBytes) {
            fail(err, errLen, "zone %u: budget %u B > %u B", (unsigned)i,
                 asUnsigned(used + zoneBytes), asUnsigned(env.maxBankBytes));
            delete bank;
            return false;
        }

        int16_t* pcm = bank->allocPcm(outFrames);
        if (!pcm) {
            fail(err, errLen, "zone %u: no memory (%u B)", (unsigned)i, asUnsigned(zoneBytes));
            delete bank;
            return false;
        }

        if (!needResample) {
            if (!wavReadMono16(path.c_str(), pcm, outFrames)) {
                fail(err, errLen, "zone %u: read failed", (unsigned)i);
                delete bank;
                return false;
            }
        } else {
            // The source PCM does not belong to the bank: only the converted
            // copy is kept, so bytes() reports what is actually resident.
            int16_t* src = static_cast<int16_t*>(
                env.alloc(static_cast<size_t>(info.frames) * sizeof(int16_t)));
            if (!src) {
                fail(err, errLen, "zone %u: no memory for source", (unsigned)i);
                delete bank;
                return false;
            }
            const bool ok = wavReadMono16(path.c_str(), src, info.frames)
                         && fftResample(fft, src, info.frames, info.sampleRate,
                                        pcm, outFrames, env.engineRate, env.alloc, env.free);
            env.free(src);
            if (!ok) {
                // Rate conversion is the one step whose transient cost dwarfs
                // the zone: source + converted copy + the overlap-add scratch
                // (up to ~200 kB for an 8 kHz source into 44.1 kHz, the widest
                // ratio a kit ships), all live at once. At the launcher's
                // headroom that caps a converted zone at roughly half a
                // second, and the failure is an allocation, not the maths.
                fail(err, errLen, "zone %u: resample %u->%u failed", (unsigned)i,
                     (unsigned)info.sampleRate, (unsigned)env.engineRate);
                delete bank;
                return false;
            }
            logLine(env, "zone %u '%s': %u Hz -> %u Hz, %u frames", (unsigned)i,
                    spec.sample.c_str(), (unsigned)info.sampleRate,
                    (unsigned)env.engineRate, (unsigned)outFrames);
        }

        Zone z;
        z.pcm    = pcm;
        z.length = outFrames;

        if (spec.root >= 0.0f) {
            z.rootMidi = spec.root;
            logLine(env, "zone %u '%s': root %.2f (from kit.json)", (unsigned)i,
                    spec.sample.c_str(), (double)z.rootMidi);
        } else {
            PitchResult pr;
            if (!detectPitch(fft, pcm, outFrames, env.engineRate, pr)) {
                fail(err, errLen, "zone %u: pitch detection failed", (unsigned)i);
                delete bank;
                return false;
            }
            z.rootMidi = pr.midi;
            logLine(env, "zone %u '%s': root %.2f (%.1f Hz, conf %.1f) detected", (unsigned)i,
                    spec.sample.c_str(), (double)pr.midi, (double)pr.hz, (double)pr.confidence);
        }

        // Loop points are in source-file samples; rate conversion moves them.
        uint32_t loopStart = spec.loopStart;
        uint32_t loopEnd   = spec.loopEnd;
        if (needResample && info.frames > 0) {
            const double r = static_cast<double>(outFrames) / static_cast<double>(info.frames);
            loopStart = static_cast<uint32_t>(loopStart * r);
            loopEnd   = static_cast<uint32_t>(loopEnd * r);
        }
        if (loopEnd > outFrames) loopEnd = outFrames;
        if (loopEnd <= loopStart) { loopStart = 0; loopEnd = 0; }
        z.loopStart = loopStart;
        z.loopEnd   = loopEnd;

        if (!bank->addZone(z)) {
            fail(err, errLen, "zone %u: addZone rejected", (unsigned)i);
            delete bank;
            return false;
        }
        ++zoneCount;
        used += zoneBytes;
    }

    if (zoneCount == 0) {
        fail(err, errLen, "no usable zones");
        delete bank;
        return false;
    }

    logLine(env, "kit '%s': %u zones, %u B", kit.name.c_str(), (unsigned)zoneCount,
            asUnsigned(bank->bytes()));
    outBank = bank;
    return true;
}

} // namespace crosspad
