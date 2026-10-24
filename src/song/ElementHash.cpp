// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/song/ElementHash.hpp"

#include <cinttypes>
#include <cmath>
#include <cstdio>

namespace crosspad {

namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime  = 1099511628211ull;
constexpr char     kSilent[]  = "silent";
constexpr char     kMissing[] = "missing";
/// Hundredths of a beat per minute: the finest tempo the bake is told apart by.
constexpr float    kBpmScale  = 100.0f;

struct Fnv {
    uint64_t h = kFnvOffset;
    void bytes(const void* v, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(v);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= kFnvPrime; }
    }
    template <typename T> void value(const T& v) { bytes(&v, sizeof v); }
    /// Followed by a terminator, so "ab"+"c" and "a"+"bc" differ.
    void text(const std::string& s) { bytes(s.data(), s.size()); value<uint8_t>(0); }
};

/// A file by name, size and time -- or as missing.
void fileStamp(Fnv& f, const ElementHashReader& rd, const std::string& path) {
    uint64_t bytes = 0;
    int64_t modified = 0;
    if (rd.info && rd.info(path, bytes, modified)) {
        f.value(bytes);
        f.value(modified);
    } else {
        f.text(kMissing);
    }
}

void notesOf(Fnv& f, const SeqPatternPtr& p) {
    if (!p) { f.value<uint8_t>(0); return; }
    f.value<uint8_t>(1);
    f.value(p->bars);
    for (const SeqNote& n : p->notes) {
        f.value(n.tick); f.value(n.len); f.value(n.pad); f.value(n.vel); f.value(n.perf);
    }
}

/// A kit or instrument: the file the song names, whole, and every file beside it.
void soundOf(Fnv& f, const ElementHashReader& rd, const AudioSong& song, const std::string& ref) {
    f.text(ref);
    if (ref.empty()) return;
    const std::string path = ref[0] == '/' ? ref : song.dir + "/" + ref;
    std::string bytes;
    if (rd.read && rd.read(path, bytes)) f.text(bytes);
    else f.text(kMissing);
    if (!rd.list) return;
    const size_t slash = path.rfind('/');
    rd.list(slash == std::string::npos ? std::string() : path.substr(0, slash), [&](const std::string& file) {
        f.text(file);
        fileStamp(f, rd, file);
    });
}

} // namespace

uint64_t element_content_hash(const AudioSong& song, const ElementHashReader& rd) {
    Fnv f;
    for (int t = 0; t < AudioSong::kTracks; ++t) {
        const SongTrack& tr = song.tracks[t];
        f.value(t);
        if (song.silenced(t)) { f.text(kSilent); continue; }
        f.value((uint8_t)tr.kind);
        f.value(tr.volume);
        f.value((uint8_t)tr.stretch);
        for (const SongClip& c : tr.clips) {
            f.text(c.file);
            if (!c.isElement() && tr.kind == TrackKind::Audio) fileStamp(f, rd, song.clipPath(c.file));
            f.value(c.bar); f.value(c.bars); f.value(c.offset); f.value(c.frames); f.value(c.from);
            notesOf(f, c.notes);
        }
        for (const SongClip& z : tr.freeze) {
            f.text(z.file);
            uint64_t bytes = 0;
            int64_t modified = 0;
            if (rd.info && rd.info(song.clipPath(z.file), bytes, modified)) f.value(bytes);
            else f.text(kMissing);
            f.value(z.bar); f.value(z.bars); f.value(z.offset);
        }
    }
    f.value(song.length);
    f.value(song.beatsPerBar);
    soundOf(f, rd, song, song.kit);
    soundOf(f, rd, song, song.instrument);
    return f.h;
}

uint64_t element_bake_key(uint64_t content, float bpm, uint32_t sampleRate) {
    Fnv f;
    f.value(content);
    f.value((int64_t)std::lround(bpm * kBpmScale));
    f.value(sampleRate);
    f.value(kRenderVersion);
    return f.h;
}

std::string element_bake_file(uint64_t key) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "B%016" PRIX64 ".wav", key);
    return buf;
}

} // namespace crosspad
