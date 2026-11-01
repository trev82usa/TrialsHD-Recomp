#!/usr/bin/env python3
"""Finds code addresses referenced only from data (vtables, callback and handler tables) that the
recompiler did not register as functions. Calls through such pointers fail at runtime with
"Call to invalid or unregistered function".

A candidate is a 4-byte aligned value in .rdata/.data that points into the code section, at an
instruction boundary (right after blr, bctr, an unconditional branch, or padding), and is not
already a function start in the generated code.

Usage: find_data_code_ptrs.py default.pe generated/default/<project>_init.cpp > addresses.txt
       (one hex address per line; add each to [functions] as  0xADDR = { name = "sub_ADDR" })
"""
import re
import struct
import sys

BASE = 0x82000000
CODE = (0x820F0000, 0x826F7EC8)
DATA = [(0x82000600, 0x820CEE00), (0x82700000, 0x827D025C)]   # .rdata (before .pdata), .data

pe = open(sys.argv[1], 'rb').read()
def w(a): return struct.unpack('>I', pe[a - BASE:a - BASE + 4])[0]
known = {int(x, 16) for x in re.findall(r'\{ 0x([0-9A-Fa-f]+), ', open(sys.argv[2]).read())}

found = set()
for lo, hi in DATA:
    for a in range(lo, hi, 4):
        v = w(a)
        if not (CODE[0] <= v < CODE[1]) or v % 4 or v in known or w(v) == 0:
            continue
        prev = w(v - 4)
        at_boundary = prev in (0x4E800020, 0x4E800420, 0) or (prev >> 26 == 18 and not prev & 1)
        if at_boundary:
            found.add(v)

for v in sorted(found):
    print(f"{v:#x}")
print(f"{len(found)} unregistered code pointers", file=sys.stderr)
