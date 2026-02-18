#pragma once

namespace crosspad {

class HslColor {
public:
    HslColor(float h, float s, float l) : h(h), s(s), l(l) {}
    float h;
    float s;
    float l;
    bool operator==(const HslColor& other) const {
        return h == other.h && s == other.s && l == other.l;
    }
};

} // namespace crosspad
