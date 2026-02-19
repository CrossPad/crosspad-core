#pragma once

namespace crosspad {

// Helper class to list colors and ease conversion.
// Remember to update the list of colors in both places as well as the count.
class ColorList
{
    static constexpr char colorList[67] = {
        "The same\n" // used for "no change" on pressed color
        "green\n"
        "blue\n"
        "red\n"
        "yellow\n"
        "orange\n"
        "purple\n"
        "pink\n"
        "brown\n"
        "white\n"
        "off"
    };

    static constexpr int colorListValues[10] = {
        // skip "the same" option
        0x20FF10, // green
        0x1020FF, // blue
        0xFF1020, // red
        0xFFFF10, // yellow
        0xFF8000, // orange
        0x8020FF, // purple
        0xFF20FF, // pink
        0x804020, // brown
        0xF0F0F0, // white, slightly dimmed
        0x000000, // off
    };

    public:

    // get the list of colors split by new line
    // @return `const char *` list of colors
    const char * Get() {return colorList + 9;}
    const char * GetPressed() {return colorList;}
    uint GetCount() {return sizeof(colorListValues) / sizeof(colorListValues[0]);}

    // get the color by index
    // @param `int` index - index of the color
    // @return `int` color value in hex
    int Get(int index) {
        if (index < 0 || index >= (int)GetCount()) return 0xFFFFFF; // white
        return colorListValues[index];
    }
};

} // namespace crosspad
