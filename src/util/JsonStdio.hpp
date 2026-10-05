// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace crosspad {

/// ArduinoJson reads from anything with read()/readBytes() and writes to
/// anything with write(). These two give it a stdio FILE*, the one file API
/// shared by the device's VFS, the PC simulator and Arduino. Not <fstream>:
/// that links libstdc++'s locale and iostream machinery into the image, which
/// on the board costs flash and, page for page, PSRAM.

/// Reads through its own buffer, so the parser's one-character read() is not
/// a locked stdio call per byte. The file stays the caller's to close.
class JsonFileReader {
public:
    /// The reader lives on the caller's stack -- the kit loader's and the LVGL
    /// task's among them -- so the object stays under the 260 B a std::ifstream
    /// takes on the S3, the budget those stacks were sized with.
    static constexpr size_t kBufSize = 128;

    explicit JsonFileReader(FILE* f) : f_(f) {}

    int read() {
        if (pos_ == len_ && !fill()) return -1;
        return (unsigned char)buf_[pos_++];
    }

    size_t readBytes(char* out, size_t n) {
        size_t done = 0;
        while (done < n) {
            if (pos_ == len_ && !fill()) break;
            size_t k = len_ - pos_;
            if (k > n - done) k = n - done;
            memcpy(out + done, buf_ + pos_, k);
            pos_ += k;
            done += k;
        }
        return done;
    }

private:
    bool fill() {
        len_ = fread(buf_, 1, sizeof(buf_), f_);
        pos_ = 0;
        return len_ > 0;
    }

    FILE* f_;
    char buf_[kBufSize];
    size_t pos_ = 0;
    size_t len_ = 0;
};

/// stdio buffers the writes; a failed one sets the stream's error flag, which
/// closeWritten() reports.
struct JsonFileWriter {
    FILE* f;
    size_t write(uint8_t c) { return fputc((int)c, f) == EOF ? 0u : 1u; }
    size_t write(const uint8_t* s, size_t n) { return fwrite(s, 1, n, f); }
};

/// Closes a file written through JsonFileWriter. True only if every write and
/// the final flush reached the file -- a full card shows up on the flush.
inline bool closeWritten(FILE* f) {
    const bool ioError = ferror(f) != 0;
    return fclose(f) == 0 && !ioError;
}

} // namespace crosspad
