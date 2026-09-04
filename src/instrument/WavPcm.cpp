// SPDX-License-Identifier: MIT
#include "crosspad/instrument/WavPcm.hpp"

#include <cstdio>
#include <cstring>
#include <new>

namespace crosspad {

namespace {

constexpr uint32_t kChunkBytes = 4096;
constexpr uint16_t kWaveFormatPcm = 0x0001;
constexpr uint16_t kWaveFormatExtensible = 0xFFFE;

uint32_t rdU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t rdU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

/**
 * @brief Total bytes in @p f, leaving the stream rewound. -1 when unknown.
 *
 * A file whose length does not fit in a `long` cannot be seeked with fseek()
 * anyway, and ftell() reports that as a failure -- such a file is refused here
 * rather than parsed with a size the rest of the reader cannot express.
 */
long fileSize(FILE* f) {
    if (std::fseek(f, 0, SEEK_END) != 0) return -1;
    const long n = std::ftell(f);
    if (n < 0) return -1;
    if (std::fseek(f, 0, SEEK_SET) != 0) return -1;
    return n;
}

/// Walks the chunk list. On success `dataOffset`/`dataBytes` locate the PCM.
bool parseHeader(FILE* f, WavInfo& info, long& dataOffset, uint32_t& dataBytes,
                 bool& formatSupported) {
    formatSupported = false;
    const long fileBytes = fileSize(f);
    if (fileBytes < 12) return false;
    uint8_t hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12) return false;
    if (std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) return false;

    uint16_t audioFormat = 0;
    bool haveFmt = false, haveData = false;
    dataOffset = 0;
    dataBytes = 0;

    uint8_t ch[8];
    while (std::fread(ch, 1, 8, f) == 8) {
        const long pos = std::ftell(f);           // first byte of the chunk body
        if (pos < 0 || pos > fileBytes) break;

        // The declared size comes straight off disk and is not trusted: a
        // truncated or hostile file happily claims ~4 GB, which then overflows
        // the `long` fseek() takes (32-bit on some hosts) and makes `frames`
        // describe data that is not there. Clamping to the bytes actually left
        // is what keeps every seek below inside the file and inside `long` --
        // both operands of the subtraction are already longs.
        uint64_t size = rdU32(ch + 4);
        const uint64_t remain = static_cast<uint64_t>(fileBytes - pos);
        if (size > remain) size = remain;

        if (std::memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            const uint32_t want = (size < 16) ? static_cast<uint32_t>(size) : 16u;
            if (std::fread(fmt, 1, want, f) != want) return false;
            if (want < 16) return false;
            audioFormat   = rdU16(fmt + 0);
            info.channels = rdU16(fmt + 2);
            info.sampleRate = rdU32(fmt + 4);
            info.bits     = rdU16(fmt + 14);
            haveFmt = true;
            if (size > want && std::fseek(f, static_cast<long>(size - want), SEEK_CUR) != 0) return false;
        } else if (std::memcmp(ch, "data", 4) == 0) {
            dataOffset = pos;
            dataBytes = static_cast<uint32_t>(size);
            haveData = true;
            if (std::fseek(f, static_cast<long>(size), SEEK_CUR) != 0) break;
        } else {
            if (std::fseek(f, static_cast<long>(size), SEEK_CUR) != 0) break;
        }
        if (size & 1u) {           // RIFF pads odd-sized chunks to even
            if (std::fseek(f, 1, SEEK_CUR) != 0) break;
        }
        if (haveFmt && haveData) break;
    }
    if (!haveFmt || !haveData) return false;

    const uint16_t frameBytes = static_cast<uint16_t>(info.channels * (info.bits / 8));
    info.frames = (frameBytes > 0) ? (dataBytes / frameBytes) : 0;

    formatSupported = (audioFormat == kWaveFormatPcm || audioFormat == kWaveFormatExtensible)
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
