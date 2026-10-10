#pragma once
#include <cstddef>

/// Commands over the WiFi remote (MQTT topic cp/{serial}/req and /rsp):
/// "<id> <line>". The id is chosen by the browser and echoed by the board, so
/// replies match requests when several are in flight.
namespace crosspad {

constexpr size_t kRemoteIdMax = 8;         ///< id: [0-9a-z]{1,8}
constexpr size_t kRemoteCommandMax = 512;  ///< a CDC text verb with its arguments
constexpr size_t kRemoteReplyMax = 4096;   ///< reply lines, "\r\n"-separated

struct RemoteRequest {
    char id[kRemoteIdMax + 1];
    char command[kRemoteCommandMax + 1];
};

/// payload is not NUL-terminated (an MQTT PUBLISH body). False on anything but
/// "<id> <printable ASCII command>" within the limits; out is then unspecified.
bool parseRemoteRequest(const char* payload, size_t len, RemoteRequest& out);

/// Writes "<id> <reply>" into out (NUL-terminated) and returns its length. A
/// reply that does not fit kRemoteReplyMax or cap is cut after its last whole
/// line that fits and ends with "ERR truncated\r\n". cap == 0 writes nothing.
size_t formatRemoteReply(const char* id, const char* reply, char* out, size_t cap);

}  // namespace crosspad
