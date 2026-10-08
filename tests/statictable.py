#!/usr/bin/env python3
"""Check that the spec and all four implementations agree on SPEC 4's table.

The static table is the one piece of shared constant in this protocol.
Every other agreement between the two sides is behavioural and shows up
as a failed request, but a table that disagrees by one index fails
silently: a header still decodes, to the wrong name. Two implementations
that both got it wrong the same way would also look fine.

So this reads the table out of SPEC.md and out of each implementation
and compares all five, by index, in order.

  SPEC.md             the normative source
  server/bframe.c     the C server's copy
  client/wire.c       the C client's copy, written separately
  tests/rawframe.py   the Python client
  tests/pyserve.py    the Python server

Run: python3 tests/statictable.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(rel):
    with open(os.path.join(ROOT, rel), errors="replace") as fh:
        return fh.read()


def from_spec():
    """The markdown table in SPEC 4, read by index so column layout
    cannot change the answer."""
    txt = read("SPEC.md")
    m = re.search(r"\|\s*Index\s*\|.*?(?=\n\n)", txt, re.S)
    if not m:
        return None, "no Index table found in SPEC.md"
    out = {}
    for line in m.group(0).split("\n")[2:]:
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) >= 2 and re.fullmatch(r"\d+", cells[0]):
            out[int(cells[0])] = cells[1].strip("`")
    return [out[i] for i in sorted(out)], None


def from_braces(rel, pattern):
    """A C initialiser or a Python list, in source order, skipping the
    NULL or None placeholder that makes the arrays 1-based."""
    txt = read(rel)
    m = re.search(pattern, txt, re.S)
    if not m:
        return None, "no table found in " + rel
    body = m.group(1)
    items = re.findall(r'"([^"]*)"|\b(NULL|None)\b', body)
    names = [a for a, b in items if not b]
    return names, None


SOURCES = [
    ("SPEC.md", from_spec, None),
    ("server/bframe.c", from_braces,
     r"static_table\[[^\]]*\]\s*=\s*\{(.*?)\};"),
    ("client/wire.c", from_braces,
     r"static_table\[[^\]]*\]\s*=\s*\{(.*?)\};"),
    ("tests/rawframe.py", from_braces, r"STATIC\s*=\s*\[(.*?)\]"),
    ("tests/pyserve.py", from_braces, r"STATIC\s*=\s*\[(.*?)\]"),
]


def main():
    tables, errors = {}, []
    for name, fn, arg in SOURCES:
        got, err = fn() if arg is None else fn(name, arg)
        if err:
            errors.append(err)
        else:
            tables[name] = got

    if errors:
        for e in errors:
            print("  ERROR: %s" % e)
        return 1

    ref_name = "SPEC.md"
    ref = tables[ref_name]

    print()
    print("  %-20s %d names" % (ref_name, len(ref)))
    for i, n in enumerate(ref, 1):
        print("    %2d  %s" % (i, n))
    print()

    bad = False
    for name, got in tables.items():
        if name == ref_name:
            continue
        if got == ref:
            print("  %-20s agrees" % name)
        else:
            bad = True
            print("  %-20s DISAGREES" % name)
            for i in range(max(len(ref), len(got))):
                a = ref[i] if i < len(ref) else "(missing)"
                b = got[i] if i < len(got) else "(missing)"
                if a != b:
                    print("      index %d: spec says %s, this says %s"
                          % (i + 1, a, b))

    if len(ref) != 10:
        bad = True
        print("  the table must hold exactly ten names, found %d" % len(ref))

    print()
    if bad:
        print("  FAIL: the implementations do not share one table")
        return 1
    print("  all four implementations match the spec, index for index")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
