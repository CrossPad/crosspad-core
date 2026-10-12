// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file AppName.hpp
 * @brief Which app a start request names (board APP_START, simulator
 *        app_start): the launcher name, ASCII case ignored.
 */

namespace crosspad::control {

/// True when @p requested names the app called @p appName. Either null: false.
bool appNameMatches(const char* appName, const char* requested);

} // namespace crosspad::control
