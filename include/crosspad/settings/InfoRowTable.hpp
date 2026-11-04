// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file InfoRowTable.hpp
 * @brief The rows a platform reports on Settings -> Info.
 *
 * ISettingsUI::getInfoRowCount() / getInfoRow() are asked on every redraw of
 * the page; the platform builds its rows once (some read the STM32 over I2C)
 * and serves them from here until invalidate(). Rows every platform shows the
 * same way -- one per installed app -- are added by addAppVersions().
 */

#include <cstddef>

#if defined(__GNUC__) || defined(__clang__)
#define CROSSPAD_INFO_ROW_PRINTF(fmtIdx, argIdx) __attribute__((format(printf, fmtIdx, argIdx)))
#else
#define CROSSPAD_INFO_ROW_PRINTF(fmtIdx, argIdx)
#endif

namespace crosspad {

class InfoRowTable {
public:
    static constexpr int kMaxRows = 32;
    /// A value with its NUL: about what fits beside a label on the 320 px row.
    static constexpr std::size_t kValueMax = 40;

    /// True when the rows are to be built: the first call, and the first one
    /// after invalidate(). The table is emptied then.
    bool beginBuild();
    void invalidate() { built_ = false; }

    /// Adds a row, printf-formatted and cut to kValueMax; ignored once the
    /// table is full. `label` is kept by pointer -- a string literal.
    void add(const char* label, const char* fmt, ...) CROSSPAD_INFO_ROW_PRINTF(3, 4);

    /// One row per installed app (appVersionGet() entries with an appId):
    /// "<commit>[ @<ref>][ *]", labelled with the component name.
    void addAppVersions();

    int count() const { return count_; }
    /// False for an index out of range.
    bool get(int idx, const char** label, const char** value) const;

private:
    struct Row {
        const char* label;
        char value[kValueMax];
    };
    Row rows_[kMaxRows] = {};
    int count_ = 0;
    bool built_ = false;
};

} // namespace crosspad

#undef CROSSPAD_INFO_ROW_PRINTF
