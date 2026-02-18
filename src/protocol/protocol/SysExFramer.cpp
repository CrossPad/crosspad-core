#include "crosspad/protocol/SysExFramer.hpp"

namespace crosspad {

void SysExFramer::feedBytes(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        if (b == CROSSPAD_SYSEX_START) {
            receiving_ = true;
            idx_ = 0;
            buf_[idx_++] = b;
        }
        else if (b == CROSSPAD_SYSEX_END && receiving_) {
            buf_[idx_++] = b;
            receiving_ = false;

            // Validate: minimum length and manufacturer ID
            if (idx_ >= 4 && buf_[1] == CROSSPAD_SYSEX_MANUFACTURER) {
                if (cb_) {
                    cb_(buf_, idx_);
                }
            }
            idx_ = 0;
        }
        else if (receiving_) {
            if (idx_ < MAX_SYSEX_SIZE - 1) {
                buf_[idx_++] = b;
            } else {
                // Overflow — discard frame
                receiving_ = false;
                idx_ = 0;
            }
        }
    }
}

void SysExFramer::reset() {
    receiving_ = false;
    idx_ = 0;
}

} // namespace crosspad
