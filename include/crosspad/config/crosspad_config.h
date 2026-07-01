#pragma once
// Single include point for all CrossPad compile-time config. Order matters:
// boards → Configuration → Configuration_adv → board profile → sanity checks.
#include "boards.h"
#include "Configuration.h"
#include "Configuration_adv.h"
#include "board/board_profile.h"
#include "SanityCheck.h"
