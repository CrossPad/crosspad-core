// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/instrument/WavPcm.hpp"

#include <cstdio>
#include <cstring>
#include <new>

namespace crosspad {

namespace {

constexpr uint32_t kChunkBytes = 4096;
constexpr uint16_t kWaveFormatPcm = 0x0001;
constexpr uint16_t kWaveFormatExtensible = 0xFFFE;

uint16_t rdU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

/// The layout of a file wavReadLayout() accepted, as WavPcmReader reads it.
bool parseHeader(FILE* f, WavInfo& info, long& dataOffset, uint32_t& dataBytes,
                 bool& formatSupported) {
    formatSupported = false;
    WavLayout layout;
    const bool found = wavReadLayout(f, layout, true);
    info.channels   = layout.channels;
    info.sampleRate = layout.rate;
    info.bits       = layout.bits;
    if (!found) return false;
    dataOffset = static_cast<long>(layout.dataOffset);
    dataBytes  = layout.dataBytes;

    const uint16_t frameBytes = static_cast<uint16_t>(info.channels * (info.bits / 8));
    info.frames = (frameBytes > 0) ? (dataBytes / frameBytes) : 0;

    formatSupported = (layout.format == kWaveFormatPcm
                       || layout.format == kWaveFormatExtensible)
                   && info.bits == 16
                   && (info.channels == 1 || info.channels == 2);
    return true;
}

} // namespace

bool WavPcmReader::open(const char* path) {
    close();
    if (!path) return false;
    f_ = std::fopen(path, "rb");
    if (!f_) return false;

    long dataOffset = 0;
    uint32_t dataBytes = 0;
    bool supported = false;
    // info_ keeps whatever the walk understood even when the format is
    // rejected: wavReadInfo() promises its caller that much, and a rejected
    // file with a plausible header is exactly what a diagnostic wants to see.
    const bool parsed = parseHeader(f_, info_, dataOffset, dataBytes, supported);
    if (!parsed || !supported || std::fseek(f_, dataOffset, SEEK_SET) != 0) {
        release();
        return false;
    }

    buf_ = new (std::nothrow) uint8_t[kChunkBytes];
    if (!buf_) {
        release();
        return false;
    }
    left_ = info_.frames;
    return true;
}

uint32_t WavPcmReader::readMono16(int16_t* out, uint32_t frames) {
    if (!f_ || !buf_ || !out) return 0;
    const uint32_t frameBytes = static_cast<uint32_t>(info_.channels) * 2u;
    if (frameBytes == 0) return 0;
    if (frames > left_) frames = left_;

    // Whole frames per read so a stereo pair never straddles two chunks.
    const uint32_t chunkFrames = kChunkBytes / frameBytes;
    uint32_t done = 0;
    while (done < frames) {
        uint32_t batch = (frames - done < chunkFrames) ? (frames - done) : chunkFrames;
        const size_t want = static_cast<size_t>(batch) * frameBytes;
        const size_t got = std::fread(buf_, 1, want, f_);
        if (got < frameBytes) break;
        batch = static_cast<uint32_t>(got / frameBytes);
        for (uint32_t i = 0; i < batch; ++i) {
            const uint8_t* p = buf_ + static_cast<size_t>(i) * frameBytes;
            int32_t s = static_cast<int16_t>(rdU16(p));
            if (info_.channels == 2) {
                s = (s + static_cast<int16_t>(rdU16(p + 2))) / 2;
            }
            out[done + i] = static_cast<int16_t>(s);
        }
        done += batch;
        if (got < want) break;
    }
    left_ -= done;
    return done;
}

void WavPcmReader::release() {
    if (f_) { std::fclose(f_); f_ = nullptr; }
    delete[] buf_;
    buf_ = nullptr;
    left_ = 0;
}

void WavPcmReader::close() {
    release();
    info_ = WavInfo{};
}

bool wavReadInfo(const char* path, WavInfo& out) {
    WavPcmReader r;
    const bool ok = r.open(path);
    out = r.info();
    return ok;
}

bool wavReadMono16(const char* path, int16_t* out, uint32_t frames) {
    if (!out) return false;
    WavPcmReader r;
    if (!r.open(path)) return false;

    std::memset(out, 0, static_cast<size_t>(frames) * sizeof(int16_t));
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t got = r.readMono16(out + done, frames - done);
        if (got == 0) break;
        done += got;
    }
    return true;
}

} // namespace crosspad
