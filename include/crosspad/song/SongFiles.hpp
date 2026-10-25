// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace crosspad {

/// File and folder copies for the song worker (an element's folder, a song
/// forked by Copy). Worker only: they wait on the card, so nothing on the
/// LVGL thread calls them. @p yield() runs between pieces so the streamer
/// is not starved.

/// @p src to @p dst in 4 kB pieces, through "<dst>.part" and a rename, with
/// two descriptors open at most (the card has a handful). Nothing is left at
/// @p dst or under ".part" when it fails.
bool song_copy_file(const std::string& src, const std::string& dst, const std::function<void()>& yield);

/// @p srcDir and everything in it to @p dstDir. @p keep gets each entry's
/// path relative to @p srcDir ("clips/.freeze") and says whether it is
/// copied, a folder with everything in it. A folder's names are read and its
/// handle closed before any of it is copied or descended into.
bool song_copy_tree(const std::string& srcDir, const std::string& dstDir,
                    const std::function<bool(const std::string& rel)>& keep,
                    const std::function<void()>& yield);

/// The folders in @p dir, by name, sorted. Empty when @p dir cannot be read.
std::vector<std::string> song_list_dirs(const std::string& dir);

} // namespace crosspad
