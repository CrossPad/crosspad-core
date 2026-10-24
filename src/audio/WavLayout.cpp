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

/// The engine's rules, or wavReadLayout()'s: `fmt ` and `data` in either
/// order and no limit on the chunk headers walked.
enum class Order : uint8_t { FmtFirst, Any };

bool walk(WavLayout& w, WavLayout::ReadAt read, void* ctx, uint32_t size, WavLayout::Chunk* list,
          uint8_t cap, Order order) {
    w = WavLayout();
    if (!read || size < 12) return false;
    const uint32_t fileBytes = size;
    w.fileBytes = size;
    uint8_t h[12];
    if (read(ctx, 0, h, 12) != 12) return false;
    if (std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0) return false;
    const uint32_t riff = rd32(h + 4);
    w.riffBytes = riff > UINT32_MAX - 8 ? UINT32_MAX : riff + 8;

    bool haveFmt = false, haveData = false;
    uint32_t pos = 12;
    for (int n = 0; (order == Order::Any || n < WavLayout::kMaxChunkHeaders) && pos <= fileBytes - 8 &&
                    fileBytes >= 8;
         ++n) {
        uint8_t ch[8];
        if (read(ctx, pos, ch, 8) != 8) break;
        const uint32_t len = rd32(ch + 4);
        const uint32_t avail = fileBytes - (pos + 8);
        if (std::memcmp(ch, "fmt ", 4) == 0 && (!haveData || order == Order::Any)) {
            uint8_t fm[16] = {};
            if (len < 16 || read(ctx, pos + 8, fm, 16) != 16) return false;
            w.format = rd16(fm);
            w.channels = rd16(fm + 2);
            w.rate = rd32(fm + 4);
            w.blockAlign = rd16(fm + 12);
            w.bits = rd16(fm + 14);
            haveFmt = true;
        } else if (std::memcmp(ch, "data", 4) == 0 && !haveData) {
            // the engine never reads a data chunk it cannot frame
            if (!haveFmt && order == Order::FmtFirst) return false;
            w.dataOffset = pos + 8;
            w.dataBytes = len < avail ? len : avail;
            haveData = true;
        } else if (list && w.chunkCount < cap) {
            WavLayout::Chunk& c = list[w.chunkCount++];
            std::memcpy(c.id, ch, 4);
            c.offset = pos + 8;
            c.size = len < avail ? len : avail;
            c.afterData = haveData;
        }
        if (haveFmt && haveData && !list) break;     // nothing after the data changes the layout
        if (len > avail) break;                      // runs past the file: nothing after it
        pos += 8 + len + (len & 1);                  // chunks are word aligned
        if (pos < 8 + len) break;                    // wrapped
    }
    if (!haveData || !haveFmt) return false;
    const uint32_t frameBytes = static_cast<uint32_t>(w.channels) * (w.bits / 8);
    w.frames = frameBytes ? w.dataBytes / frameBytes : 0;
    return true;
}

/// @p f's length for a walk, or false when it cannot be told.
bool fileLength(FILE* f, uint32_t& bytes) {
    if (!f || std::fseek(f, 0, SEEK_END) != 0) return false;
    const long end = std::ftell(f);
    if (end < 0) return false;
    bytes = static_cast<uint32_t>(end);
    return true;
}

} // namespace

bool WavLayout::parse(ReadAt read, void* ctx, uint32_t size, Chunk* list, uint8_t cap) {
    return walk(*this, read, ctx, size, list, cap, Order::FmtFirst);
}

bool WavLayout::parse(FILE* f, Chunk* list, uint8_t cap) {
    uint32_t bytes = 0;
    if (!fileLength(f, bytes)) { *this = WavLayout(); return false; }
    return parse(readFile, f, bytes, list, cap);
}

bool wavReadLayout(FILE* f, WavLayout& out, bool clampToFile) {
    if (!f) { out = WavLayout(); return false; }
    if (!clampToFile) {
        // Sizes as declared: a file as long as the format can describe, the
        // reads ending the walk where the file does.
        const bool ok = walk(out, readFile, f, UINT32_MAX, nullptr, 0, Order::Any);
        out.fileBytes = 0;
        return ok;
    }
    uint32_t bytes = 0;
    if (!fileLength(f, bytes)) { out = WavLayout(); return false; }
    return walk(out, readFile, f, bytes, nullptr, 0, Order::Any);
}

} // namespace crosspad
