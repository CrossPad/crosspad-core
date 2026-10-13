// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/transport/SequencerTransport.hpp"
#include "crosspad/transport/BlockSongTransport.hpp"

namespace crosspad {

SequencerTransport* sequencerTransport()
{
    SequencerController* sq = getPlatformServices().sequencer;
    if (!sq) return nullptr;
    /* Bound on the first call, when the platform has published the
     * controller; the controller is a static of the platform's own, so the
     * reference outlives every claim. */
    static SequencerTransport instance(*sq);
    return &instance;
}

BlockSongTransport* blockSongTransport()
{
    SequencerController* sq = getPlatformServices().sequencer;
    if (!sq) return nullptr;
    static BlockSongTransport instance(*sq);
    return &instance;
}

} // namespace crosspad
