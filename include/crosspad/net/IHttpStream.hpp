// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace crosspad {

/// What a request answered, as far as a caller decides anything from it.
struct HttpStreamReply {
    int status = 0;              ///< HTTP status, 0 = no response (DNS, TLS, socket)
    bool rangeMismatch = false;  ///< a 206 whose range is not the one asked for (getStream)
};

/// HTTP GETs into the caller's buffers: a bounded body, or a ranged body
/// streamed through a sink in pieces. Unlike IHttpClient nothing is
/// allocated per request and a body never has to fit in memory, which is what
/// a multi-megabyte firmware file on a board needs. Blocking.
class IHttpStream {
public:
    /// Returns false to stop the transfer.
    using Sink = bool (*)(const uint8_t* data, size_t n, void* ctx);

    virtual ~IHttpStream() = default;

    /// The whole 200 body into buf (NUL-terminated, at most cap-1 bytes).
    /// False on any other status, a transport error or a body that does not fit.
    virtual bool getBuffer(const char* url, char* buf, size_t cap, size_t* len, HttpStreamReply* out) = 0;

    /// The body from `offset` on ("Range: bytes=<offset>-") through `sink`, in
    /// pieces of at most chunkLen read into `chunk`. True only when the body
    /// arrived complete. A server that answers a resume (offset > 0) with 200
    /// sent the whole file: then nothing reaches the sink, out->status is 200
    /// and the result is false, so a partial file is never followed by a whole
    /// one. Likewise a 206 whose Content-Range does not start at `offset`, or
    /// whose total is not `expectedTotal` (0 = not checked): nothing reaches
    /// the sink and out->rangeMismatch is set. 416 and every other status are
    /// left in out->status.
    virtual bool getStream(const char* url, uint64_t offset, uint64_t expectedTotal, Sink sink, void* ctx,
                           uint8_t* chunk, size_t chunkLen, HttpStreamReply* out) = 0;
};

} // namespace crosspad
