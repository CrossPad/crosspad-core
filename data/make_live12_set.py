#!/usr/bin/env python3
"""Make live12_set.xml.gz, the empty Live 12 Set a song export clones from.

It is Live's own File > New Live Set (Builtin/Templates/DefaultLiveSet.als)
with the two MIDI tracks and the second audio track taken out: one AudioTrack
to clone per song track, the two returns, Main. Live writes one element per
line, indented with tabs, so a track is cut out by its lines.

The gzip carries no name and no time, so the file changes only when the set
does. See crosspad/song/LiveSet.hpp for what is done with it.

    python3 make_live12_set.py "/Applications/Ableton Live 12 Suite.app"
"""

import gzip
import pathlib
import re
import sys

TEMPLATE = "Contents/App-Resources/Builtin/Templates/DefaultLiveSet.als"
OUT = pathlib.Path(__file__).with_name("live12_set.xml.gz")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    lines = gzip.decompress((pathlib.Path(sys.argv[1]) / TEMPLATE).read_bytes()).decode("utf-8").split("\n")
    kept, cut, audio = [], None, 0
    for line in lines:
        m = re.match(r"\t\t\t<(MidiTrack|AudioTrack) ", line)
        if cut is None and m:
            if m.group(1) == "AudioTrack":
                audio += 1
            if m.group(1) == "MidiTrack" or audio > 1:
                cut = m.group(1)
                continue
        if cut is not None:
            if line == f"\t\t\t</{cut}>":
                cut = None
            continue
        kept.append(line)
    xml = "\n".join(kept)
    if xml.count("<AudioTrack ") != 1 or "<MidiTrack " in xml or xml.count("<ReturnTrack ") != 2:
        print("unexpected DefaultLiveSet layout; nothing written", file=sys.stderr)
        return 1
    OUT.write_bytes(gzip.compress(xml.encode("utf-8"), compresslevel=9, mtime=0))
    print(f"{OUT.name}: {len(xml)} bytes of XML, {OUT.stat().st_size} gzipped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
