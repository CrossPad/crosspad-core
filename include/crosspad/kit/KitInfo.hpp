#pragma once
#include <cstdint>
#include <string>
#include <crosspad/pad/Pad.hpp>

#ifndef KIT_PADS
#define KIT_PADS 16
#endif

namespace crosspad {

struct KitInfo {
    enum Type : uint8_t {
        Normal  = 0,
        NoAudio = 1,
        Custom  = 2
    };

    std::string path;
    std::string name        = "Default Kit";
    std::string author      = "CrossPad";
    std::string description;
    std::string version;
    std::string created_at;
    Type        type          = Normal;
    int         total_samples = 0;
    bool        parsed        = false;

    Pad pads[KIT_PADS];

    KitInfo() = default;
    explicit KitInfo(const std::string& kitPath) : path(kitPath) {}

    std::string getDetails() const {
        return name + " by " + author + ", " + description + ", " + version
             + ", " + created_at + ", " + std::to_string(total_samples)
             + " samples, " + (parsed ? "parsed" : "not parsed");
    }
};

} // namespace crosspad
