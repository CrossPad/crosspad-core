#pragma once
#include "../boards.h"

#if !defined(CROSSPAD_BOARD)
#  error "CROSSPAD_BOARD undefined — include Configuration.h before board_profile.h"
#endif

#if   CROSSPAD_BOARD == BOARD_CROSSPAD_V1
#  include "board_v1.h"
#elif CROSSPAD_BOARD == BOARD_CROSSPAD_V2
#  include "board_v2.h"
#elif CROSSPAD_BOARD == BOARD_CROSSPAD_PC
#  include "board_pc.h"
#else
#  error "Unknown CROSSPAD_BOARD value"
#endif
