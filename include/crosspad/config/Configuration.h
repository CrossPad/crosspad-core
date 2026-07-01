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

// Full STM per-pad tuning editor (LVGL screen). Auto-dropped unless level==FULL.
#ifndef FEAT_PAD_EDITOR
#  define FEAT_PAD_EDITOR
#endif
