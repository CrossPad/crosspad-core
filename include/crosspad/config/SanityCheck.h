// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Compile-time coherence guards. Included last.
#if defined(FEAT_PAD_EDITOR) && (CROSSPAD_PAD_CONFIG_LEVEL != CROSSPAD_PAD_CONFIG_FULL)
// Not an error: the editor is simply compiled out at GLOBAL level. This guard
// documents intent; the editor's own #if enforces it. No #error here.
#endif

#if CROSSPAD_PAD_CONFIG_LEVEL != CROSSPAD_PAD_CONFIG_FULL && \
    CROSSPAD_PAD_CONFIG_LEVEL != CROSSPAD_PAD_CONFIG_GLOBAL
#  error "CROSSPAD_PAD_CONFIG_LEVEL must be FULL(2) or GLOBAL(1)"
#endif

// Boolean feature flags are 1/0 values. A typo like -DFEAT_PAD_EDITOR=yes must
// fail loudly rather than silently evaluating to 0 and dropping the feature.
#if (FEAT_PAD_EDITOR != 0) && (FEAT_PAD_EDITOR != 1)
#  error "FEAT_PAD_EDITOR must be 0 or 1"
#endif
#if (FEAT_SETTINGS_BACKUP != 0) && (FEAT_SETTINGS_BACKUP != 1)
#  error "FEAT_SETTINGS_BACKUP must be 0 or 1"
#endif
#if (FEAT_PERF_STATS != 0) && (FEAT_PERF_STATS != 1)
#  error "FEAT_PERF_STATS must be 0 or 1"
#endif
