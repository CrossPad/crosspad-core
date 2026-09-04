// SPDX-License-Identifier: MIT
#include "crosspad/instrument/PitchedKitLoader.hpp"

#include "crosspad/dsp/FftResampler.hpp"
#include "crosspad/dsp/PitchDetector.hpp"
#include "crosspad/instrument/WavPcm.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
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

/// Input frames handed to the converter per step. 4096 frames is 8 kB of
/// scratch, one analysis block of the resampler, and one turn of the WAV
/// reader's own 4 kB chunk loop -- small enough that the whole load costs the
/// bank plus a block whatever the file's length.
constexpr uint32_t kReadChunkFrames = 4096;

/**
 * @brief Read @p reader through a rate conversion straight into @p out.
 *
 * Stops as soon as @p outFrames are written, which is what makes a
 * budget-truncated zone cheap: the tail of the file is never read at all.
 *
 * @return frames written; @p ok says whether the conversion itself held up.
 */
uint32_t streamResampleZone(const PitchedLoadEnv& env, IFft& fft, WavPcmReader& reader,
                            uint32_t inRate, int16_t* out, uint32_t outFrames, bool& ok) {
    ok = false;
    FftResamplerStream stream;
    if (!stream.begin(fft, inRate, env.engineRate, env.alloc, env.free)) return 0;

    int16_t* chunk = static_cast<int16_t*>(
        env.alloc(static_cast<size_t>(kReadChunkFrames) * sizeof(int16_t)));
    if (!chunk) return 0;

    uint32_t written = 0;
    while (written < outFrames) {
        const uint32_t n = reader.readMono16(chunk, kReadChunkFrames);
        if (n == 0) break;
        written += stream.push(chunk, n, out + written, outFrames - written);
        if (!stream.ok()) break;
    }
    if (stream.ok() && written < outFrames) {
        written += stream.flush(out + written, outFrames - written);
    }
    ok = stream.ok();
    env.free(chunk);
    stream.end();
    return written;
}

} // namespace

bool pitchedBuildBank(const KitInfo& kit, const PitchedLoadEnv& env, IFft& fft,
                      SampleBank*& outBank, char* err, size_t errLen) {
    return pitchedBuildBank(kit, env, fft, outBank, err, errLen, nullptr);
}

bool pitchedBuildBank(const KitInfo& kit, const PitchedLoadEnv& env, IFft& fft,
                      SampleBank*& outBank, char* err, size_t errLen,
                      PitchedLoadReport* report) {
    if (err && errLen) err[0] = '\0';
    if (report) *report = PitchedLoadReport{};

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

        WavPcmReader reader;
        if (!reader.open(path.c_str()) || reader.info().frames == 0) {
            fail(err, errLen, "zone %u: bad wav '%s'", (unsigned)i, spec.sample.c_str());
            delete bank;
            return false;
        }
        const WavInfo info = reader.info();

        const bool needResample = (info.sampleRate != env.engineRate);
        const uint32_t fullFrames = needResample
            ? resampledLength(info.frames, info.sampleRate, env.engineRate)
            : info.frames;
        if (fullFrames == 0) {
            fail(err, errLen, "zone %u: empty", (unsigned)i);
            delete bank;
            return false;
        }

        // The budget is settled in 64-bit: fullFrames comes from a file header
        // by way of resampledLength(), size_t is 32 bits on the device, and a
        // corrupt header that overflowed the product would otherwise look
        // like a zone that fits.
        uint32_t frames = fullFrames;
        bool truncated = false;
        if (env.maxBankBytes != 0) {
            const uint64_t roomBytes = (used < env.maxBankBytes) ? (env.maxBankBytes - used) : 0;
            const uint64_t roomFrames = roomBytes / sizeof(int16_t);
            if (roomFrames == 0) {
                fail(err, errLen, "zone %u: budget %u B exhausted", (unsigned)i,
                     asUnsigned(env.maxBankBytes));
                delete bank;
                return false;
            }
            if (roomFrames < frames) {
                frames = static_cast<uint32_t>(roomFrames);
                truncated = true;
            }
        }
        const uint64_t zoneBytes = static_cast<uint64_t>(frames) * sizeof(int16_t);

        int16_t* pcm = bank->allocPcm(frames);
        if (!pcm) {
            fail(err, errLen, "zone %u: no memory (%u B)", (unsigned)i, asUnsigned(zoneBytes));
            delete bank;
            return false;
        }

        uint32_t got = 0;
        if (!needResample) {
            while (got < frames) {
                const uint32_t n = reader.readMono16(pcm + got, frames - got);
                if (n == 0) break;
                got += n;
            }
        } else {
            bool ok = false;
            got = streamResampleZone(env, fft, reader, info.sampleRate, pcm, frames, ok);
            if (!ok) {
                // Rate conversion is the one step with scratch of its own: one
                // analysis block, taken and released per zone. The failure is
                // an allocation or a transform the backend refused, never the
                // length of the file — that is the point of streaming it.
                fail(err, errLen, "zone %u: resample %u->%u failed", (unsigned)i,
                     (unsigned)info.sampleRate, (unsigned)env.engineRate);
                delete bank;
                return false;
            }
            logLine(env, "zone %u '%s': %u Hz -> %u Hz, %u frames", (unsigned)i,
                    spec.sample.c_str(), (unsigned)info.sampleRate,
                    (unsigned)env.engineRate, (unsigned)frames);
        }
        // A file that ends early leaves the rest of its buffer as the
        // allocator handed it over; the engine renders every sample it is told
        // the zone has.
        if (got < frames) {
            std::memset(pcm + got, 0, static_cast<size_t>(frames - got) * sizeof(int16_t));
        }
        reader.close();

        if (truncated) {
            const double rate = static_cast<double>(env.engineRate);
            const double fullSec = fullFrames / rate;
            const double keptSec = frames / rate;
            char note[sizeof(PitchedLoadReport::warning)] = {};
            snprintf(note, sizeof(note), "zone %u '%s': %.1f s cut to %.1f s (bank budget)",
                     (unsigned)i, spec.sample.c_str(), fullSec, keptSec);
            logLine(env, "%s", note);
            if (report) {
                ++report->truncatedZones;
                report->droppedSeconds += static_cast<float>(fullSec - keptSec);
                if (report->warning[0] == '\0') {
                    std::memcpy(report->warning, note, sizeof(report->warning));
                    report->warning[sizeof(report->warning) - 1] = '\0';
                }
            }
        }

        Zone z;
        z.pcm    = pcm;
        z.length = frames;

        if (spec.root >= 0.0f) {
            z.rootMidi = spec.root;
            logLine(env, "zone %u '%s': root %.2f (from kit.json)", (unsigned)i,
                    spec.sample.c_str(), (double)z.rootMidi);
        } else {
            PitchResult pr;
            if (!detectPitch(fft, pcm, frames, env.engineRate, pr)) {
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
            const double r = static_cast<double>(fullFrames) / static_cast<double>(info.frames);
            loopStart = static_cast<uint32_t>(loopStart * r);
            loopEnd   = static_cast<uint32_t>(loopEnd * r);
        }
        // A truncated zone loses its loop end with the audio it named.
        if (loopEnd > frames) loopEnd = frames;
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
    if (report) report->zones = zoneCount;
    outBank = bank;
    return true;
}

} // namespace crosspad
