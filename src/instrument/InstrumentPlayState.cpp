// SPDX-License-Identifier: MIT
#include "crosspad/instrument/InstrumentPlayState.hpp"

namespace crosspad {

/* A function-local static, not a file-scope object: this is constructed on the
 * first use, so it cannot depend on the order the translation units'
 * constructors run in -- the trap CLAUDE.md records for registration on this
 * platform. */
InstrumentPlayState& instrumentPlayState()
{
    static InstrumentPlayState state;
    return state;
}

void instrumentPlayStatePublished()
{
    ++instrumentPlayState().generation;
}

} // namespace crosspad
