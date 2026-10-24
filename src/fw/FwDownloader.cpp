// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/fw/FwDownloader.hpp"
#include <cstdio>
#include <cstring>
#include <new>
#include "crosspad/fw/FwKit.hpp"

namespace crosspad {
namespace {

constexpr size_t kDirMax = kFwLibraryRootMax + 1 + kFwPackageNameMax;
constexpr size_t kPathMax = kDirMax + 1 + sizeof(FwManifestFile::name) + kFwPartSuffixMax;
constexpr size_t kHexLen = 2 * ISha256::kDigestLen;
constexpr int kStatusOk = 200;
constexpr int kStatusClientError = 400;
constexpr int kStatusRangeNotSatisfiable = 416;
constexpr char kMarkerBody[] = "1\n";
constexpr char kManifestName[] = "manifest.json";
constexpr char kSigName[] = "manifest.sig";
constexpr char kBinName[] = "CrossPad.bin";
constexpr char kAssetsName[] = "assets.bin";

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsIgnoreCase(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (lowerAscii(*a) != lowerAscii(*b)) return false;
    return *a == *b;
}

void copyName(char* out, size_t cap, const char* s) { std::snprintf(out, cap, "%s", s); }

/* Everything the catalog supplied that becomes part of a path or a URL. */
bool catalogNamesSafe(const char* kit, const FwCatalogVersion& v) {
    return fwKitIdValid(kit) && fwSafeName(v.version, sizeof v.version) && fwSafeName(v.tag, sizeof v.tag) &&
           fwSafeName(v.manifest, sizeof v.manifest) && fwSafeName(v.sig, sizeof v.sig) &&
           fwSafeName(v.bin.name, sizeof v.bin.name) && fwSafeName(v.assets.name, sizeof v.assets.name) &&
           (!v.icon.name[0] || fwSafeName(v.icon.name, sizeof v.icon.name));
}

FwDlEvent makeEvent(FwDlEventKind kind, const char* subject) {
    FwDlEvent e{};
    e.kind = kind;
    e.subject = subject;
    return e;
}

} // namespace

const char* fwDlResultName(FwDlResult r) {
    switch (r) {
        case FwDlResult::Ok:        return "ok";
        case FwDlResult::NoCard:    return "no_card";
        case FwDlResult::CardFull:  return "card_full";
        case FwDlResult::Network:   return "network";
        case FwDlResult::Http:      return "http";
        case FwDlResult::Signature: return "signature";
        case FwDlResult::Mismatch:  return "mismatch";
        case FwDlResult::Sha:       return "sha";
        case FwDlResult::Io:        return "io";
        case FwDlResult::Blocked:   return "blocked";
        case FwDlResult::NoMem:     return "no_mem";
    }
    return "?";
}

bool FwBlocklist::has(const char* kit, const char* version) const {
    for (size_t i = 0; i < count_; ++i)
        if (std::strcmp(items_[i].kit, kit) == 0 && std::strcmp(items_[i].version, version) == 0) return true;
    return false;
}

bool FwBlocklist::add(const char* kit, const char* version, FwBlockedVersion* forgotten, bool* dropped) {
    *dropped = false;
    if (has(kit, version)) return false;
    if (count_ == kFwBlocklistMax) {
        *forgotten = items_[0];
        std::memmove(&items_[0], &items_[1], (kFwBlocklistMax - 1) * sizeof items_[0]);
        --count_;
        *dropped = true;
    }
    FwBlockedVersion& b = items_[count_++];
    copyName(b.kit, sizeof b.kit, kit);
    copyName(b.version, sizeof b.version, version);
    return true;
}

struct FwDownloader::Buffers {
    uint8_t chunk[kFwDlChunk];
    char manifest[kFwDlManifestMax];
    uint8_t sig[kFwDlSigMax + 1];
    FwManifest parsed;
};

struct FwDownloader::Sink {
    FwDownloader* self;
    void* f;
    uint64_t have;
    uint64_t size;
    FwDlProgress* progress;
    uint32_t base;
    bool writeError;
    bool overflow;
};

FwDownloader::FwDownloader(IFwDownloadPlatform& platform, IFwFiles& files, IHttpStream& http, ISha256& sha,
                           const FwPackageLayout& layout)
    : platform_(platform), files_(files), http_(http), sha_(sha), layout_(layout) {}

void FwDownloader::event(FwDlEventKind kind, const char* subject) { platform_.onEvent(makeEvent(kind, subject)); }

/* False for a missing, empty or too-large file (a real manifest is < 1 kB). */
bool FwDownloader::readFile(const char* path, char* buf, size_t cap, size_t* len) {
    void* f = files_.open(path, FwFileMode::Read);
    if (!f) return false;
    *len = files_.read(f, buf, cap);
    files_.close(f);
    return *len > 0 && *len < cap;
}

bool FwDownloader::writeFile(const char* dir, const char* name, const void* data, size_t len) {
    char path[kPathMax];
    std::snprintf(path, sizeof path, "%s/%s", dir, name);
    void* f = files_.open(path, FwFileMode::Write);
    if (!f) return false;
    const bool wrote = files_.write(f, data, len) == len;
    return files_.close(f) && wrote;
}

/* The stored signature and manifest, checked again. The images themselves are
 * not hashed here: the install hashes CrossPad.bin against this manifest, and
 * assets.bin is trusted as written -- a card someone can write to is outside
 * what a download signature protects. */
bool FwDownloader::verifyDir(const char* dir, const char* kit, const char* version, Buffers* b) {
    char path[kPathMax];
    size_t mlen = 0, slen = 0;
    std::snprintf(path, sizeof path, "%s/%s", dir, kManifestName);
    if (!readFile(path, b->manifest, sizeof b->manifest, &mlen)) return false;
    std::snprintf(path, sizeof path, "%s/%s", dir, kSigName);
    if (!readFile(path, reinterpret_cast<char*>(b->sig), kFwDlSigMax, &slen)) return false;
    return platform_.verifyManifest(reinterpret_cast<const uint8_t*>(b->manifest), mlen, b->sig, slen) &&
           parseFwManifest(b->manifest, mlen, b->parsed, platform_.jsonAllocator()) &&
           fwManifestMatches(b->parsed, kit, version, platform_.boardRev());
}

bool FwDownloader::sinkCb(const uint8_t* data, size_t n, void* ctx) {
    auto* s = static_cast<Sink*>(ctx);
    if (s->have + n > s->size) {
        s->overflow = true;
        return false;
    }
    if (s->self->files_.write(s->f, data, n) != n) {
        s->writeError = true;
        return false;
    }
    s->self->sha_.update(data, n);
    s->have += n;
    s->progress->done.store(s->base + static_cast<uint32_t>(s->have));
    s->self->platform_.yieldIo();
    return true;
}

/* The bytes already in a file go into the hash first: a resumed file is
 * checked whole, never just the part that arrived this time. */
bool FwDownloader::hashPrefix(const char* path, uint64_t len, uint8_t* chunk) {
    void* f = files_.open(path, FwFileMode::Read);
    if (!f) return false;
    for (uint64_t left = len; left > 0;) {
        const size_t want = left < kFwDlChunk ? static_cast<size_t>(left) : kFwDlChunk;
        if (files_.read(f, chunk, want) != want) {
            files_.close(f);
            return false;
        }
        sha_.update(chunk, want);
        left -= want;
        platform_.yieldIo();
    }
    files_.close(f);
    return true;
}

/* Ends the hash begun by sha_.start(). */
bool FwDownloader::shaMatches(const char* want) {
    uint8_t digest[ISha256::kDigestLen];
    sha_.finish(digest);
    char hex[kHexLen + 1];
    for (size_t i = 0; i < ISha256::kDigestLen; ++i) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return equalsIgnoreCase(hex, want);
}

/* A file an earlier run already finished (renamed from .part): a later run
 * that resumes the other file must not fetch this one again. */
bool FwDownloader::fileInPlace(const char* path, const FwManifestFile& want, uint8_t* chunk) {
    if (files_.size(path) != want.size) return false;
    sha_.start();
    if (!hashPrefix(path, want.size, chunk)) return false;
    return shaMatches(want.sha256);
}

FwDlResult FwDownloader::downloadFile(const char* url, const char* finalPath, const FwManifestFile& want,
                                      uint32_t base, FwDlProgress& progress, uint8_t* chunk) {
    if (fileInPlace(finalPath, want, chunk)) {
        progress.done.store(base + want.size);
        return FwDlResult::Ok;
    }
    char part[kPathMax + kFwPartSuffixMax];
    std::snprintf(part, sizeof part, "%s%s", finalPath, layout_.partSuffix);
    for (int shaTry = 0; shaTry < kFwDlShaAttempts; ++shaTry) {
        sha_.start();
        uint64_t have = files_.size(part);
        if (have > want.size) {
            files_.remove(part);
            have = 0;
        }
        if (have > 0 && !hashPrefix(part, have, chunk)) {
            // A .part the card will not read back would fail every retry the same way.
            event(FwDlEventKind::PartUnreadable, part);
            files_.remove(part);
            have = 0;
            sha_.start();
        }
        progress.done.store(base + static_cast<uint32_t>(have));
        FwDlResult net = FwDlResult::Ok;
        for (int attempt = 0; have < want.size && attempt < kFwDlNetAttempts; ++attempt) {
            if (attempt > 0) platform_.sleepMs(kFwDlNetRetryDelayMs);
            void* f = files_.open(part, have ? FwFileMode::Append : FwFileMode::Write);
            if (!f) {
                net = FwDlResult::Io;
                break;
            }
            const uint64_t from = have;
            Sink s{this, f, have, want.size, &progress, base, false, false};
            HttpStreamReply reply;
            const bool complete = http_.getStream(url, from, want.size, sinkCb, &s, chunk, kFwDlChunk, &reply);
            if (!files_.close(f)) s.writeError = true;
            have = s.have;
            if (s.writeError) {
                net = FwDlResult::Io;
                break;
            }
            if (s.overflow) {   // more bytes than the manifest signed: not this file
                net = FwDlResult::Sha;
                break;
            }
            if (from > 0 && (reply.status == kStatusOk || reply.rangeMismatch)) {
                // The server ignored the range or answered another one: its bytes
                // do not continue this .part, so start the file over, never append.
                FwDlEvent e = makeEvent(FwDlEventKind::ResumeRefused, url);
                e.at = from;
                e.status = reply.status;
                e.otherRange = reply.rangeMismatch;
                platform_.onEvent(e);
                files_.remove(part);
                have = 0;
                sha_.start();
                progress.done.store(base);
                continue;
            }
            if (reply.status == kStatusRangeNotSatisfiable) {   // the .part is longer than the file it names
                FwDlEvent e = makeEvent(FwDlEventKind::RangeNotSatisfiable, url);
                e.at = from;
                platform_.onEvent(e);
                files_.remove(part);
                have = 0;
                sha_.start();
                progress.done.store(base);
                continue;
            }
            if (reply.status >= kStatusClientError) {
                net = FwDlResult::Http;
                break;
            }
            if (complete && have == want.size) break;
            FwDlEvent e = makeEvent(FwDlEventKind::Resuming, url);
            e.at = have;
            e.of = want.size;
            platform_.onEvent(e);
        }
        if (net == FwDlResult::Sha) {
            files_.remove(part);
            continue;
        }
        if (net != FwDlResult::Ok) return net;   // .part kept: the next attempt resumes
        if (have != want.size) return FwDlResult::Network;
        if (shaMatches(want.sha256)) {
            files_.remove(finalPath);   // FATFS rename refuses an existing destination
            return files_.rename(part, finalPath) ? FwDlResult::Ok : FwDlResult::Io;
        }
        FwDlEvent e = makeEvent(FwDlEventKind::ShaMismatch, finalPath);
        e.attempt = shaTry + 1;
        e.attempts = kFwDlShaAttempts;
        platform_.onEvent(e);
        files_.remove(part);
    }
    return FwDlResult::Sha;
}

/* The two slots' packages and anything pinned are never deleted by this module. */
bool FwDownloader::isProtected(const char* name, const char* dir, const char* slotA, const char* slotB) {
    if ((slotA && equalsIgnoreCase(name, slotA)) || (slotB && equalsIgnoreCase(name, slotB))) return true;
    char pin[kPathMax];
    std::snprintf(pin, sizeof pin, "%s/%s", dir, layout_.pinned);
    return files_.exists(pin);
}

bool FwDownloader::markDownloading(const char* dir) {
    return writeFile(dir, layout_.downloading, kMarkerBody, sizeof kMarkerBody - 1);
}

FwDlResult FwDownloader::runWith(Buffers* b, const char* baseUrl, const char* kit, const FwCatalogVersion& v,
                                 FwDlProgress& progress, const char* slotA, const char* slotB) {
    const uint8_t rev = platform_.boardRev();
    char name[kFwPackageNameMax];
    fwPackageName(kit, v.version, name, sizeof name);
    char dir[kDirMax];
    std::snprintf(dir, sizeof dir, "%s/%s", layout_.root, name);
    progress.total.store(v.bin.size + v.assets.size + v.icon.size);
    progress.done.store(0);

    if (!files_.makeDir(layout_.cardDir) || !files_.makeDir(layout_.root)) return FwDlResult::NoCard;
    char complete[kPathMax];
    std::snprintf(complete, sizeof complete, "%s/%s", dir, layout_.complete);
    const bool stale = files_.exists(complete);
    if (stale) {
        if (verifyDir(dir, kit, v.version, b)) {
            progress.done.store(progress.total.load());
            return FwDlResult::Ok;
        }
        event(FwDlEventKind::NoLongerVerifies, name);
    }

    // The signature comes before anything on the card changes: the catalog's
    // sizes are unsigned, so they must not decide what gets deleted.
    char url[kFwFileUrlMax];
    size_t mlen = 0, slen = 0;
    HttpStreamReply reply;
    if (!fwCatalogFileUrl(baseUrl, v.tag, v.manifest, url, sizeof url)) return FwDlResult::Mismatch;
    if (!http_.getBuffer(url, b->manifest, sizeof b->manifest, &mlen, &reply))
        return reply.status ? FwDlResult::Http : FwDlResult::Network;
    if (!fwCatalogFileUrl(baseUrl, v.tag, v.sig, url, sizeof url)) return FwDlResult::Mismatch;
    if (!http_.getBuffer(url, reinterpret_cast<char*>(b->sig), sizeof b->sig, &slen, &reply))
        return reply.status ? FwDlResult::Http : FwDlResult::Network;
    if (!platform_.verifyManifest(reinterpret_cast<const uint8_t*>(b->manifest), mlen, b->sig, slen)) {
        event(FwDlEventKind::BadSignature, name);
        platform_.block(kit, v.version);
        return FwDlResult::Signature;
    }
    const bool parsed = parseFwManifest(b->manifest, mlen, b->parsed, platform_.jsonAllocator());
    const FwManifestFile* bin = parsed ? fwManifestFile(b->parsed, kBinName) : nullptr;
    const FwManifestFile* assets = parsed ? fwManifestFile(b->parsed, kAssetsName) : nullptr;
    const FwManifestFile* icon = parsed ? fwManifestFile(b->parsed, kFwManifestIconName) : nullptr;
    // Signed like the image: the manifest lists it exactly when the catalog names one, at the same size.
    const bool iconMatches = icon ? (v.icon.name[0] && icon->size == v.icon.size) : !v.icon.name[0];
    // The catalog is unsigned, so its channel and tier are checked against the
    // signed ones: a beta build must not pass for a stable update, nor a
    // community build for an official one (that tier is the only warning).
    if (!parsed || !fwManifestMatches(b->parsed, kit, v.version, rev) || b->parsed.channel != v.channel ||
        b->parsed.tier != v.tier || !bin || !assets || bin->size != v.bin.size || assets->size != v.assets.size ||
        !iconMatches) {
        FwDlEvent e = makeEvent(FwDlEventKind::NotThisBoard, name);
        e.kit = kit;
        e.version = v.version;
        platform_.onEvent(e);
        return FwDlResult::Mismatch;
    }

    // The cleanup keeps this package (`name`) either way; running it before the
    // marker goes means a card that is full anyway refuses with the card unchanged.
    const uint64_t need = static_cast<uint64_t>(bin->size) + assets->size + (icon ? icon->size : 0) + mlen + slen;
    const uint64_t have = platform_.packageBytes(name);
    const FwDlResult room = platform_.cleanup(need > have ? need - have : 0, name, slotA, slotB);
    if (room != FwDlResult::Ok) return room;
    if (stale) {
        // Only the marker goes, and only now that the fresh manifest is signed: the
        // files stay until a download replaces each one after its SHA matches, so a
        // package a slot or the user holds is never emptied.
        if (!markDownloading(dir) || !files_.remove(complete)) return FwDlResult::Io;
    }
    if (!files_.makeDir(dir) || !markDownloading(dir)) return FwDlResult::Io;
    if (!writeFile(dir, kManifestName, b->manifest, mlen) || !writeFile(dir, kSigName, b->sig, slen))
        return FwDlResult::Io;

    struct Item {
        const FwManifestFile* file;
        const char* source;
    };
    Item items[kFwManifestFilesMax] = {{bin, v.bin.name}, {assets, v.assets.name}};
    size_t itemCount = 2;
    if (icon) items[itemCount++] = {icon, v.icon.name};
    uint32_t base = 0;
    for (size_t i = 0; i < itemCount; ++i) {
        const Item& it = items[i];
        char path[kPathMax];
        std::snprintf(path, sizeof path, "%s/%s", dir, it.file->name);
        if (!fwCatalogFileUrl(baseUrl, v.tag, it.source, url, sizeof url)) return FwDlResult::Mismatch;
        const FwDlResult r = downloadFile(url, path, *it.file, base, progress, b->chunk);
        if (r == FwDlResult::Sha) {
            platform_.block(kit, v.version);
            if (!isProtected(name, dir, slotA, slotB)) platform_.removePackage(name);
        }
        if (r != FwDlResult::Ok) return r;
        base += it.file->size;
    }
    if (!writeFile(dir, layout_.complete, kMarkerBody, sizeof kMarkerBody - 1)) return FwDlResult::Io;
    char marker[kPathMax];
    std::snprintf(marker, sizeof marker, "%s/%s", dir, layout_.downloading);
    files_.remove(marker);   // .complete is there: a leftover marker is ignored, so a failure changes nothing
    // Best effort: the package is whole, and a cleanup that cannot run changes nothing about that.
    platform_.cleanup(0, name, slotA, slotB);
    event(FwDlEventKind::Complete, name);
    return FwDlResult::Ok;
}

FwDlResult FwDownloader::run(const char* baseUrl, const char* kit, const FwCatalogVersion& v,
                             FwDlProgress& progress, const char* slotA, const char* slotB) {
    if (!catalogNamesSafe(kit, v)) {
        event(FwDlEventKind::UnsafeName, kit ? kit : "?");
        return FwDlResult::Mismatch;
    }
    if (platform_.isBlocked(kit, v.version)) return FwDlResult::Blocked;
    void* mem = platform_.allocBuffers(sizeof(Buffers));
    if (!mem) return FwDlResult::NoMem;
    const FwDlResult r = runWith(new (mem) Buffers, baseUrl, kit, v, progress, slotA, slotB);
    platform_.freeBuffers(mem);
    return r;
}

bool FwDownloader::verifyOnCard(const char* kit, const char* version) {
    if (!fwKitIdValid(kit) || !fwSafeName(version, kFwVersionMax)) return false;
    char name[kFwPackageNameMax];
    fwPackageName(kit, version, name, sizeof name);
    char dir[kDirMax];
    std::snprintf(dir, sizeof dir, "%s/%s", layout_.root, name);
    char complete[kPathMax];
    std::snprintf(complete, sizeof complete, "%s/%s", dir, layout_.complete);
    if (!files_.exists(complete)) return false;
    void* mem = platform_.allocBuffers(sizeof(Buffers));
    if (!mem) return false;
    const bool ok = verifyDir(dir, kit, version, new (mem) Buffers);
    platform_.freeBuffers(mem);
    return ok;
}

} // namespace crosspad
