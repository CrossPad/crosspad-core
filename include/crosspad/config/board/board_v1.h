#pragma once
// Optimal initial values — rev 1. Baseline == current literals.
#define BOARD_PAD_SENS_DEF        1
#define BOARD_PAD_LR_RATIO_DEF    50
#define BOARD_PAD_ANALOG_MODE_DEF 2      // ANALOG_AFTER_TOUCH
#define BOARD_VEL_CURVE_DEF       0      // VEL_LINEAR
#define BOARD_VEL_FLOOR_DEF       1
#define BOARD_CC_PRESS_BASE_DEF   16
#define BOARD_CC_PAN_BASE_DEF     48
#define BOARD_ANALOG_MIN_MS_DEF   16
#define BOARD_ANALOG_DELTA_DEF    2
#define BOARD_PRESS_DEBOUNCE_MS_DEF 6    // note-on (and velocity) window; the STM builds with 15
#define BOARD_LCD_BRIGHTNESS_DEF  100
#define BOARD_RGB_BRIGHTNESS_DEF  100
