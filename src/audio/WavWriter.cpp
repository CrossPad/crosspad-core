// SPDX-License-Identifier: MIT
#include "crosspad/audio/WavWriter.hpp"

#include <cstring>

namespace crosspad {

namespace {
void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
} // namespace

bool WavWriter::open(const char* path, uint32_t sampleRate, uint16_t channels)
{
    close();
    if (!path || !sampleRate || !channels) return false;
    file_ = std::fopen(path, "wb");
    if (!file_) return false;
    sampleRate_ = sampleRate;
    channels_   = channels;
    frames_     = 0;
    if (!writeHeader(0)) {
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    return true;
}

bool WavWriter::reserve(uint32_t frames)
{
    if (!file_) return false;
    if (frames <= frames_) return true;
    /* A byte at the last position makes the filesystem allocate everything
     * up to it; the data in between is overwritten by write(). */
    const long bytes = 44L + (long)frames * channels_ * 2L;
    const long here  = 44L + (long)frames_ * channels_ * 2L;
    return std::fseek(file_, bytes - 1, SEEK_SET) == 0 && std::fputc(0, file_) != EOF &&
           std::fflush(file_) == 0 && std::fseek(file_, here, SEEK_SET) == 0;
}

bool WavWriter::write(const int16_t* interleaved, uint32_t frames)
{
    if (!file_ || !frames) return file_ != nullptr;
    /* Little-endian PCM is what the targets hold in memory already, so the
     * samples go out as they are. */
    const size_t n = (size_t)frames * channels_;
    if (std::fwrite(interleaved, sizeof(int16_t), n, file_) != n) return false;
    frames_ += frames;
    return true;
}

bool WavWriter::close()
{
    if (!file_) return true;
    const uint32_t dataBytes = frames_ * channels_ * 2u;
    bool ok = std::fseek(file_, 0, SEEK_SET) == 0 && writeHeader(dataBytes);
    ok = (std::fclose(file_) == 0) && ok;
    file_ = nullptr;
    return ok;
}

bool WavWriter::writeHeader(uint32_t dataBytes)
{
    uint8_t h[44];
    std::memcpy(h + 0, "RIFF", 4);
    put32(h + 4, 36u + dataBytes);
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4);
    put32(h + 16, 16);                                   // PCM fmt chunk
    put16(h + 20, 1);                                    // PCM
    put16(h + 22, channels_);
    put32(h + 24, sampleRate_);
    put32(h + 28, sampleRate_ * channels_ * 2u);         // byte rate
    put16(h + 32, (uint16_t)(channels_ * 2u));           // block align
    put16(h + 34, 16);                                   // bits
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, dataBytes);
    return std::fwrite(h, 1, sizeof(h), file_) == sizeof(h);
}

} // namespace crosspad
