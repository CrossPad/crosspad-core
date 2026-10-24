// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/settings/InfoRowTable.hpp"

#include "crosspad/app/AppVersions.hpp"

#include <cstdarg>
#include <cstdio>

namespace crosspad {

bool InfoRowTable::beginBuild() {
    if (built_) return false;
    built_ = true;
    count_ = 0;
    return true;
}

void InfoRowTable::add(const char* label, const char* fmt, ...) {
    if (count_ >= kMaxRows) return;
    Row& row = rows_[count_];
    row.label = label;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(row.value, sizeof(row.value), fmt, ap);
    va_end(ap);
    count_++;
}

void InfoRowTable::addAppVersions() {
    for (std::size_t i = 0; i < appVersionCount(); ++i) {
        const AppVersion* v = appVersionGet(i);
        if (!v || !v->appId || !v->appId[0]) continue;  // core/gui: the platform's own rows
        const bool hasRef = v->ref && v->ref[0];
        add(v->component, "%s%s%s%s", v->commit, hasRef ? " @" : "", hasRef ? v->ref : "",
            v->dirty ? " *" : "");
    }
}

bool InfoRowTable::get(int idx, const char** label, const char** value) const {
    if (idx < 0 || idx >= count_) return false;
    *label = rows_[idx].label;
    *value = rows_[idx].value;
    return true;
}

} // namespace crosspad
