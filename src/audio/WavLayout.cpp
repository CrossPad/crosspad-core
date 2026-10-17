// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/audio/WavLayout.hpp"

#include <cstring>

namespace crosspad {

namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

uint32_t readFile(void* ctx, uint32_t offset, void* dst, uint32_t bytes) {
    FILE* f = static_cast<FILE*>(ctx);
    if (std::fseek(f, static_cast<long>(offset), SEEK_SET) != 0) return 0;
    return static_cast<uint32_t>(std::fread(dst, 1, bytes, f));
}

} // namespace

bool WavLayout::parse(ReadAt read, void* ctx, uint32_t size, Chunk* list, uint8_t cap) {
    *this = WavLayout();
    if (!read || size < 12) return false;
    fileBytes = size;
    uint8_t h[12];
    if (read(ctx, 0, h, 12) != 12) return false;
    if (std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0) return false;
    const uint32_t riff = rd32(h + 4);
    riffBytes = riff > UINT32_MAX - 8 ? UINT32_MAX : riff + 8;

    bool haveFmt = false, haveData = false;
    uint32_t pos = 12;
    for (int n = 0; n < kMaxChunkHeaders && pos <= fileBytes - 8 && fileBytes >= 8; ++n) {
        uint8_t ch[8];
        if (read(ctx, pos, ch, 8) != 8) break;
        const uint32_t len = rd32(ch + 4);
        const uint32_t avail = fileBytes - (pos + 8);
        if (std::memcmp(ch, "fmt ", 4) == 0 && !haveData) {
            uint8_t fm[16] = {};
            if (len < 16 || read(ctx, pos + 8, fm, 16) != 16) return false;
            format = rd16(fm);
            channels = rd16(fm + 2);
            rate = rd32(fm + 4);
            bits = rd16(fm + 14);
            haveFmt = true;
        } else if (std::memcmp(ch, "data", 4) == 0 && !haveData) {
            if (!haveFmt) return false;              // the engine never reads a data chunk it cannot frame
            dataOffset = pos + 8;
            dataBytes = len < avail ? len : avail;
            haveData = true;
        } else if (list && chunkCount < cap) {
            Chunk& c = list[chunkCount++];
            std::memcpy(c.id, ch, 4);
            c.offset = pos + 8;
            c.size = len < avail ? len : avail;
            c.afterData = haveData;
        }
        if (len > avail) break;                      // runs past the file: nothing after it
        pos += 8 + len + (len & 1);                  // chunks are word aligned
        if (pos < 8 + len) break;                    // wrapped
    }
    if (!haveData) return false;
    const uint32_t frameBytes = static_cast<uint32_t>(channels) * (bits / 8);
    frames = frameBytes ? dataBytes / frameBytes : 0;
    return true;
}

bool WavLayout::parse(FILE* f, Chunk* list, uint8_t cap) {
    if (!f || std::fseek(f, 0, SEEK_END) != 0) { *this = WavLayout(); return false; }
    const long end = std::ftell(f);
    if (end < 0) { *this = WavLayout(); return false; }
    return parse(readFile, f, static_cast<uint32_t>(end), list, cap);
}

} // namespace crosspad
