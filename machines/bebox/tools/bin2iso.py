#!/usr/bin/env python3
# Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
# SPDX-License-Identifier: GPL-2.0-or-later
"""Turn a MODE1/2352 CD dump (.cue and .bin) into a 2048-byte-sector image.

Every sector keeps its place, so track N of the CD starts at the same
block in the image. The track start blocks are printed, ready for the
scsi-cd "tracks" property.

usage: bin2iso.py disc.cue disc.iso
"""

import os
import re
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    cue, out = sys.argv[1], sys.argv[2]
    tracks = []
    binfile = None
    with open(cue, encoding="latin-1") as f:
        for line in f:
            m = re.match(r'\s*FILE\s+"(.*)"\s+BINARY', line)
            if m:
                if binfile:
                    sys.exit("only one FILE per .cue is handled")
                binfile = os.path.join(os.path.dirname(cue), m.group(1))
            m = re.match(r"\s*TRACK\s+(\d+)\s+(\S+)", line)
            if m and m.group(2) != "MODE1/2352":
                sys.exit("track %s is %s, only MODE1/2352 is handled"
                         % (m.group(1), m.group(2)))
            m = re.match(r"\s*INDEX\s+01\s+(\d+):(\d+):(\d+)", line)
            if m:
                mm, ss, ff = (int(x) for x in m.groups())
                tracks.append((mm * 60 + ss) * 75 + ff)
    if not binfile or not tracks:
        sys.exit("no FILE or no tracks in " + cue)

    size = os.path.getsize(binfile)
    if size % 2352:
        sys.exit("%s is not a whole number of 2352-byte sectors" % binfile)
    with open(binfile, "rb") as src, open(out, "wb") as dst:
        while True:
            buf = src.read(2352 * 512)
            if not buf:
                break
            dst.write(b"".join(buf[i + 16:i + 16 + 2048]
                               for i in range(0, len(buf), 2352)))
    print("tracks=" + ":".join(str(t) for t in tracks))


if __name__ == "__main__":
    main()
