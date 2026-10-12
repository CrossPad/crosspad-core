#pragma once
// Advanced / rarely-touched values. Included after Configuration.h.

//===========================================================================
//========================= STM pad-tuning defaults =========================
//===========================================================================
// What a fresh Stm32Settings holds before anything is loaded. Every CrossPad
// revision so far is tuned to the same values, so core carries one set; each
// is #ifndef-guarded, so a platform whose hardware wants another default
// passes it with -D.
#ifndef BOARD_PAD_SENS_DEF
#  define BOARD_PAD_SENS_DEF          1
#endif
#ifndef BOARD_PAD_LR_RATIO_DEF
#  define BOARD_PAD_LR_RATIO_DEF      50
#endif
#ifndef BOARD_PAD_ANALOG_MODE_DEF
#  define BOARD_PAD_ANALOG_MODE_DEF   2      // ANALOG_AFTER_TOUCH
#endif
#ifndef BOARD_VEL_CURVE_DEF
#  define BOARD_VEL_CURVE_DEF         0      // VEL_LINEAR
#endif
#ifndef BOARD_VEL_FLOOR_DEF
#  define BOARD_VEL_FLOOR_DEF         1
#endif
#ifndef BOARD_CC_PRESS_BASE_DEF
#  define BOARD_CC_PRESS_BASE_DEF     16
#endif
#ifndef BOARD_CC_PAN_BASE_DEF
#  define BOARD_CC_PAN_BASE_DEF       48
#endif
#ifndef BOARD_ANALOG_MIN_MS_DEF
#  define BOARD_ANALOG_MIN_MS_DEF     16
#endif
#ifndef BOARD_ANALOG_DELTA_DEF
#  define BOARD_ANALOG_DELTA_DEF      2
#endif
#ifndef BOARD_PRESS_DEBOUNCE_MS_DEF
#  define BOARD_PRESS_DEBOUNCE_MS_DEF 6      // note-on (and velocity) window; the STM builds with 15
#endif
#ifndef BOARD_LCD_BRIGHTNESS_DEF
#  define BOARD_LCD_BRIGHTNESS_DEF    100
#endif
#ifndef BOARD_RGB_BRIGHTNESS_DEF
#  define BOARD_RGB_BRIGHTNESS_DEF    100
#endif
