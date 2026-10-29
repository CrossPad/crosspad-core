// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace crosspad {

/// The platform's kit browser, opened by an app for a kit of its own.
///
/// The browser comes up over the running app, which is back when it closes,
/// picked or not. A kit chosen there is handed to @p picked and **not**
/// loaded: whoever asked decides what plays it -- a song's kit track keeps
/// it, and the kits' engine loads it while that track is the live one
/// (AudioSong::liveKit). The running app is torn down while the browser is up, so
/// @p picked must not touch its widgets; it runs before the app comes back.
/// The list the browser opens on: Any is the one holding the loaded kit.
enum class KitList : uint8_t { Any = 0, Perf, Drums, Slice };

class IKitPicker {
public:
    using PickedFn = void (*)(const std::string& kitPath, void* ctx);

    virtual ~IKitPicker() = default;

    /// LVGL thread. False when the browser cannot open now (it is up, or no
    /// app is in front to come back to).
    virtual bool open(PickedFn picked, void* ctx) = 0;
    /// The same, on the list @p list -- Arrange's DRUMS opens on the drum
    /// kits. A browser that cannot choose its list opens as open() does.
    virtual bool open(PickedFn picked, void* ctx, KitList list)
    {
        (void)list;
        return open(picked, ctx);
    }
};

} // namespace crosspad
