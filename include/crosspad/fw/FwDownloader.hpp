// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ArduinoJson.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "crosspad/fw/FwCatalog.hpp"
#include "crosspad/fw/FwManifest.hpp"
#include "crosspad/fw/FwPackage.hpp"
#include "crosspad/net/IHttpStream.hpp"
#include "crosspad/util/ISha256.hpp"

namespace crosspad {

/// One catalog version -> <root>/<kit>-<version>/ on the card: the cleanup for
/// its size; manifest.json and manifest.sig first, the signature checked by
/// the platform; the manifest matched to the catalog entry and this board;
/// then each file as <file>.part, resumed with Range and SHA-256-checked
/// against the signed manifest; .complete last. A bad signature or a second
/// SHA mismatch blocks the version until reboot. Blocking, card- and
/// network-heavy: a job thread of its own.

enum class FwDlResult : uint8_t {
    Ok, NoCard, CardFull, Network, Http, Signature, Mismatch, Sha, Io, Blocked, NoMem
};
/// "ok" "no_card" "card_full" "network" "http" "signature" "mismatch" "sha" "io" "blocked" "no_mem"
const char* fwDlResultName(FwDlResult r);

struct FwDlProgress {
    std::atomic<uint32_t> done{0};
    std::atomic<uint32_t> total{0};
};

constexpr size_t kFwDlChunk = 4096;
/// A real manifest is < 1 kB.
constexpr size_t kFwDlManifestMax = 4096;
/// A DER ECDSA P-256 signature is at most 72 bytes.
constexpr size_t kFwDlSigMax = 128;
/// The first request and two resumes.
constexpr int kFwDlNetAttempts = 3;
constexpr uint32_t kFwDlNetRetryDelayMs = 2000;
/// One fresh retry, then the version is blocked.
constexpr int kFwDlShaAttempts = 2;

/// The versions refused until reboot. Small on purpose: when it is full the
/// oldest entry is forgotten, because a refusal that could never be lifted
/// would be worse than one more download attempt of a version blocked long
/// ago. Not thread-safe: the platform serialises access.
constexpr size_t kFwBlocklistMax = 8;
struct FwBlockedVersion {
    char kit[kFwKitMax];
    char version[kFwVersionMax];
};
class FwBlocklist {
public:
    bool has(const char* kit, const char* version) const;
    /// False when the version was listed already. When the list was full,
    /// *dropped is set and the entry forgotten for it is copied to *forgotten.
    bool add(const char* kit, const char* version, FwBlockedVersion* forgotten, bool* dropped);

private:
    FwBlockedVersion items_[kFwBlocklistMax] = {};
    size_t count_ = 0;
};

/// Where a package lives and what its marker files are called. A file in a
/// package is <root>/<package>/<name>; the markers and the part suffix are
/// short names (at most sizeof(FwManifestFile::name) and kFwPartSuffixMax
/// bytes incl. NUL), the root at most kFwLibraryRootMax.
constexpr size_t kFwLibraryRootMax = 32;
constexpr size_t kFwPartSuffixMax = 8;
struct FwPackageLayout {
    const char* cardDir;       ///< created before the root (its parent)
    const char* root;
    const char* complete;      ///< written last: the package verified whole
    const char* downloading;   ///< written before the first file, removed after .complete
    const char* pinned;        ///< a package the user keeps
    const char* partSuffix;    ///< a file still arriving
};

enum class FwFileMode : uint8_t { Read, Write, Append };

/// The card, as far as a download touches it. Paths are whole.
class IFwFiles {
public:
    virtual ~IFwFiles() = default;
    /// An open file, or nullptr.
    virtual void* open(const char* path, FwFileMode mode) = 0;
    virtual size_t read(void* f, void* buf, size_t n) = 0;
    virtual size_t write(void* f, const void* data, size_t n) = 0;
    /// False when what was written did not reach the card.
    virtual bool close(void* f) = 0;
    /// 0 for a file that is not there.
    virtual uint64_t size(const char* path) = 0;
    virtual bool exists(const char* path) = 0;
    /// True when the directory is there afterwards, made now or before.
    virtual bool makeDir(const char* path) = 0;
    /// True when the file was removed.
    virtual bool remove(const char* path) = 0;
    /// Never over an existing file: the caller removes the destination first.
    virtual bool rename(const char* from, const char* to) = 0;
};

/// What the downloader reports beside its result, for the platform's log.
enum class FwDlEventKind : uint8_t {
    UnsafeName,          ///< subject: the kit ("?" for none) -- a catalog name unsafe for a path or URL
    PartUnreadable,      ///< subject: the .part -- the card will not read it back, the file starts over
    ResumeRefused,       ///< subject: the URL; at, status, otherRange -- a resume answered with other bytes
    RangeNotSatisfiable, ///< subject: the URL; at -- 416, the file starts over
    Resuming,            ///< subject: the URL; at, of -- the body ended early
    ShaMismatch,         ///< subject: the file; attempt (1-based), attempts
    NoLongerVerifies,    ///< subject: the package -- complete, but downloaded again
    BadSignature,        ///< subject: the package
    NotThisBoard,        ///< subject: the package; kit, version -- the signed manifest names something else
    Complete,            ///< subject: the package
};
struct FwDlEvent {
    FwDlEventKind kind;
    const char* subject;
    const char* kit;
    const char* version;
    uint64_t at;
    uint32_t of;
    int status;
    bool otherRange;
    int attempt;
    int attempts;
};

/// What only the platform can do for a download.
class IFwDownloadPlatform {
public:
    virtual ~IFwDownloadPlatform() = default;
    /// The download's buffers (~9 kB), held for one run.
    virtual void* allocBuffers(size_t bytes) = 0;
    virtual void freeBuffers(void* p) = 0;
    /// `sig` over the exact bytes of `manifest` verifies with a trusted key.
    virtual bool verifyManifest(const uint8_t* manifest, size_t manifestLen, const uint8_t* sig, size_t sigLen) = 0;
    /// The running board's revision, which a manifest must name.
    virtual uint8_t boardRev() = 0;
    virtual ArduinoJson::Allocator* jsonAllocator() { return nullptr; }
    /// Bytes of the regular files in the package directory; 0 when it is not there.
    virtual uint64_t packageBytes(const char* name) = 0;
    /// Deletes the package directory and its files.
    virtual void removePackage(const char* name) = 0;
    /// Room for `incoming` more bytes in the library, never deleting `keep`,
    /// slotA, slotB or a pinned package: Ok, CardFull, NoMem, NoCard or Io.
    virtual FwDlResult cleanup(uint64_t incoming, const char* keep, const char* slotA, const char* slotB) = 0;
    virtual bool isBlocked(const char* kit, const char* version) = 0;
    virtual void block(const char* kit, const char* version) = 0;
    /// Between pieces of card work: whatever else must read the card in time goes first.
    virtual void yieldIo() = 0;
    virtual void sleepMs(uint32_t ms) = 0;
    virtual void onEvent(const FwDlEvent& e) = 0;
};

class FwDownloader {
public:
    FwDownloader(IFwDownloadPlatform& platform, IFwFiles& files, IHttpStream& http, ISha256& sha,
                 const FwPackageLayout& layout);

    /// slotA / slotB: the package names in the two slots, which the cleanup
    /// keeps (compared case-insensitively, as FATFS does). Every name taken
    /// from the catalog (kit, version, tag, file names) is checked here
    /// before it reaches a path or a URL; one that fails is Mismatch.
    FwDlResult run(const char* baseUrl, const char* kit, const FwCatalogVersion& v, FwDlProgress& progress,
                   const char* slotA, const char* slotB);

    /// .complete is there and the manifest on the card still verifies and matches.
    bool verifyOnCard(const char* kit, const char* version);

private:
    struct Buffers;
    struct Sink;
    static bool sinkCb(const uint8_t* data, size_t n, void* ctx);

    FwDlResult runWith(Buffers* b, const char* baseUrl, const char* kit, const FwCatalogVersion& v,
                       FwDlProgress& progress, const char* slotA, const char* slotB);
    FwDlResult downloadFile(const char* url, const char* finalPath, const FwManifestFile& want, uint32_t base,
                            FwDlProgress& progress, uint8_t* chunk);
    bool fileInPlace(const char* path, const FwManifestFile& want, uint8_t* chunk);
    bool hashPrefix(const char* path, uint64_t len, uint8_t* chunk);
    bool shaMatches(const char* want);
    bool verifyDir(const char* dir, const char* kit, const char* version, Buffers* b);
    bool readFile(const char* path, char* buf, size_t cap, size_t* len);
    bool writeFile(const char* dir, const char* name, const void* data, size_t len);
    bool markDownloading(const char* dir);
    bool isProtected(const char* name, const char* dir, const char* slotA, const char* slotB);
    void event(FwDlEventKind kind, const char* subject);

    IFwDownloadPlatform& platform_;
    IFwFiles& files_;
    IHttpStream& http_;
    ISha256& sha_;
    const FwPackageLayout& layout_;
};

} // namespace crosspad
