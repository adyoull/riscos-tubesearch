#!/usr/bin/env python3
"""Writes src/alphabets.h: the RISC OS desktop alphabets TubeSearch can
show text in (OS_Byte 71), as the Unicode character of each of their
codes 0xA0-0xFF (0: none). Made from Python's ISO 8859 codecs.
Part of TubeSearch. MIT licence."""
import sys
ALPHABETS = [  # RISC OS number, name, codec
    (101, "Latin1", "iso8859_1"), (102, "Latin2", "iso8859_2"), (103, "Latin3", "iso8859_3"),
    (104, "Latin4", "iso8859_4"), (105, "Cyrillic", "iso8859_5"), (106, "Arabic", "iso8859_6"),
    (107, "Greek", "iso8859_7"), (108, "Hebrew", "iso8859_8"), (109, "Latin5", "iso8859_9"),
    (110, "Welsh", None), (112, "Latin9", "iso8859_15"), (113, "Latin6", "iso8859_10"),
    (114, "Latin7", "iso8859_13"), (115, "Latin8", "iso8859_14"), (116, "Latin10", "iso8859_16"),
]
# Welsh (ISO-IR-182): Latin-1 with these codes changed
WELSH = {0xA8: 0x1E80, 0xAA: 0x1E82, 0xAC: 0x1EF2, 0xAF: 0x0178, 0xB8: 0x1E81, 0xBA: 0x1E83,
         0xBD: 0x1E84, 0xBE: 0x1E85, 0xBF: 0x1EF3, 0xD0: 0x0174, 0xDE: 0x0176, 0xF0: 0x0175, 0xFE: 0x0177}
out = ["/* alphabets.h - made by tools/mkalphabets.py; don't edit. */",
       "static const struct { int number; const char *name; unsigned short hi[96]; } alphabets[] = {"]
for num, name, codec in ALPHABETS:
    row = []
    for b in range(0xA0, 0x100):
        if codec is None:
            u = WELSH.get(b, b)
        else:
            try:
                u = ord(bytes([b]).decode(codec))
            except UnicodeDecodeError:
                u = 0
        row.append("0x%04X" % u)
    out.append("  { %d, \"%s\", {" % (num, name))
    for i in range(0, 96, 12):
        out.append("    " + ", ".join(row[i:i + 12]) + ",")
    out.append("  } },")
out.append("};")
open(sys.argv[1] if len(sys.argv) > 1 else "src/alphabets.h", "w").write("\n".join(out) + "\n")
