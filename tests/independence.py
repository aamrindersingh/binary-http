#!/usr/bin/env python3
"""Measure how separate the two codecs actually are.

"A client that only works against your own server is an implementation,
not a protocol." The usual way to satisfy that is to share a codec
between the two sides, which satisfies nothing: one bug in a shared
encoder is invisible, because both ends agree about it.

So server/bframe.* and client/wire.* were written separately from
SPEC.md. This script exists because that is a claim, and a claim in a
README is worth less than a number anyone can reproduce.

Method, stated so the numbers mean something:

  - comments and blank lines are stripped first, since prose similarity
    is not code similarity and comments are where the two files are
    most likely to echo each other
  - lines are then compared whole and in order, with difflib
  - "longest common run" is the longest block of consecutive identical
    lines anywhere in the two sets, which is the number that would give
    a copy-paste away
  - "similarity" is difflib's ratio over those lines, where 0% is
    nothing in common and 100% is identical

Run: python3 tests/independence.py
"""
import difflib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = ["server/bframe.c", "server/bframe.h"]
CLIENT = ["client/wire.c", "client/wire.h"]

# A run this long, or a similarity this high, means the separation has
# rotted and someone has started sharing code between the two sides.
MAX_RUN = 8
MAX_SIMILARITY = 40.0


def code_lines(paths):
    out = []
    for p in paths:
        for ln in open(os.path.join(ROOT, p), errors="replace"):
            t = ln.strip()
            if not t or t.startswith(("/*", "*", "*/", "//")):
                continue
            out.append(t)
    return out


def main():
    a, b = code_lines(SERVER), code_lines(CLIENT)

    sa = set(os.listdir(os.path.join(ROOT, "server")))
    sb = set(os.listdir(os.path.join(ROOT, "client")))
    shared_names = sorted(sa & sb)

    crossed = []
    for d, other in (("server", "client"), ("client", "server")):
        for f in sorted(os.listdir(os.path.join(ROOT, d))):
            txt = open(os.path.join(ROOT, d, f), errors="replace").read()
            for m in re.finditer(r'#include\s+"([^"]+)"', txt):
                inc = m.group(1)
                if other in inc or ".." in inc:
                    crossed.append("%s/%s includes %s" % (d, f, inc))

    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    best = max(sm.get_matching_blocks(), key=lambda x: x.size)
    similarity = sm.ratio() * 100

    print()
    print("  server codec: %-28s %3d code lines" % (" ".join(SERVER), len(a)))
    print("  client codec: %-28s %3d code lines" % (" ".join(CLIENT), len(b)))
    print()
    print("  identical filenames across the two dirs:  %s"
          % (", ".join(shared_names) or "none"))
    print("  either including from the other:          %s"
          % (", ".join(crossed) or "none"))
    print("  longest common run:                       %d lines"
          % best.size)
    for ln in a[best.a:best.a + best.size]:
        print("                                              %s" % ln)
    print("  overall line similarity:                  %.1f%%" % similarity)
    print()

    problems = []
    if shared_names:
        problems.append("the two directories share filenames")
    if crossed:
        problems.append("one side includes the other")
    if best.size > MAX_RUN:
        problems.append("a common run of %d lines exceeds the %d allowed"
                        % (best.size, MAX_RUN))
    if similarity > MAX_SIMILARITY:
        problems.append("similarity of %.1f%% exceeds the %.0f%% allowed"
                        % (similarity, MAX_SIMILARITY))

    if problems:
        for p in problems:
            print("  FAIL: %s" % p)
        print()
        return 1

    print("  the two codecs are independent by every check above")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
