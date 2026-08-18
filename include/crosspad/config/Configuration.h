#pragma once
#include "boards.h"

//===========================================================================
//============================ Board selection ==============================
//===========================================================================
// Which PCB revision this firmware targets. On ESP-IDF this is overridden by
// the Kconfig board-revision choice (CMake passes -DCROSSPAD_BOARD), so editing
// here only affects PC / Arduino builds. PC sim / host tests default to PC.
#ifndef CROSSPAD_BOARD
#  define CROSSPAD_BOARD BOARD_CROSSPAD_PC
#endif

//===========================================================================
//========================== Pad configuration depth ========================
//===========================================================================
// FULL  = full 16-element per-pad arrays + per-pad editors (heaviest).
// GLOBAL = one value applied to all 16 pads (lighter FW, fewer NVS keys, less RAM).
// Override in both directions from the build system with -DCROSSPAD_PAD_CONFIG_LEVEL=1|2.
#define CROSSPAD_PAD_CONFIG_FULL   2
#define CROSSPAD_PAD_CONFIG_GLOBAL 1
#ifndef CROSSPAD_PAD_CONFIG_LEVEL
#  define CROSSPAD_PAD_CONFIG_LEVEL CROSSPAD_PAD_CONFIG_FULL
#endif
// Derived convenience flag for readability in consumer code.
#if CROSSPAD_PAD_CONFIG_LEVEL == CROSSPAD_PAD_CONFIG_FULL
#  ifndef PAD_CONFIG_PER_PAD
#    define PAD_CONFIG_PER_PAD
#  endif
#endif

//===========================================================================
//============================== Feature toggles ============================
//===========================================================================
// Comment a line out to drop the feature (Marlin convention). Each is
// #ifndef-guarded so the build system can force it on with -D.

// Feature toggles are 1/0 values rather than Marlin's bare presence, because a
// bare #define cannot be switched OFF from the build system — and the whole
// point of the #ifndef guard is that a profile can force a flag either way with
// -DFEAT_X=0. Editing the value here still works exactly like commenting a
// Marlin line out.

// Full STM per-pad tuning editor (LVGL screen). Auto-dropped unless level==FULL.
#ifndef FEAT_PAD_EDITOR
#  define FEAT_PAD_EDITOR 1
#endif

// Settings -> Backup (SD): export/import of the settings and pad-tuning JSON
// files. Drop on units with no card slot, or to save flash.
#ifndef FEAT_SETTINGS_BACKUP
#  define FEAT_SETTINGS_BACKUP 1
#endif

// Settings -> System -> Perf Stats: FPS/CPU/memory overlays in the status bar.
#ifndef FEAT_PERF_STATS
#  define FEAT_PERF_STATS 1
#endif
