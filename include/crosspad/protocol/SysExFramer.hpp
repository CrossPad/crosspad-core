#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include "crosspad/protocol/SysExProtocol.h"

namespace crosspad {

/**
 * @brief Portable SysEx byte framer — accumulates raw UART bytes into
 *        complete SysEx messages (F0 ... F7) and dispatches them.
 *
 * Feed raw bytes from any transport via feedBytes(). When a complete
 * SysEx frame with the CrossPad manufacturer ID is received, the
 * registered callback is invoked with the full message.
 */
class SysExFramer {
public:
    static constexpr size_t MAX_SYSEX_SIZE = 256;

    /// Called when a complete, validated SysEx message is ready
    using MessageCallback = std::function<void(const uint8_t* data, size_t len)>;

    SysExFramer() = default;

    void setMessageCallback(MessageCallback cb) { cb_ = std::move(cb); }

    /**
     * @brief Feed raw bytes from transport
     * @param data Raw byte buffer (may contain partial/multiple SysEx messages)
     * @param len Number of bytes
     */
    void feedBytes(const uint8_t* data, size_t len);

    /// Reset internal state (e.g. after transport error)
    void reset();

private:
    uint8_t buf_[MAX_SYSEX_SIZE] = {};
    size_t  idx_ = 0;
    bool    receiving_ = false;
    MessageCallback cb_;
};

} // namespace crosspad
