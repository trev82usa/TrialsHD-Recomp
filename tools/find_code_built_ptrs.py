#!/usr/bin/env python3
"""Finds function addresses that the game builds in code (lis rX,hi then addi/ori rY,rX,lo) and
hands out as callbacks, but that the recompiler did not register as functions. Calls through them
fail at runtime with "Call to invalid or unregistered function".

Usage: find_code_built_ptrs.py default.pe generated/default/<project>_init.cpp > addresses.txt
"""
import re
import struct
import sys

BASE = 0x82000000
CODE = (0x820F0000, 0x826F7EC8)
WINDOW = 12   # instructions after a lis in which the matching addi/ori may appear

pe = open(sys.argv[1], 'rb').read()
def w(a): return struct.unpack('>I', pe[a - BASE:a - BASE + 4])[0]
known = {int(x, 16) for x in re.findall(r'\{ 0x([0-9A-Fa-f]+), ', open(sys.argv[2]).read())}

def looks_like_table(v):
    """Inline absolute jump tables (right after a bctr) hold code addresses, not instructions."""
    return 0x82000000 <= w(v) < 0x83000000

def is_switch_base(use):
    """The value feeds a computed jump (add ...; mtctr; bctr) shortly after it is built."""
    seq = [w(c) for c in range(use + 4, use + 28, 4)]
    return any(seq[i] >> 26 == 31 and (seq[i] >> 1) & 0x3FF == 467 and seq[i + 1] == 0x4E800420
               for i in range(len(seq) - 1))

def at_boundary(v):
    prev = w(v - 4)
    return prev in (0x4E800020, 0x4E800420, 0) or (prev >> 26 == 18 and not prev & 1)

found = {}
for a in range(CODE[0], CODE[1], 4):
    x = w(a)
    if x >> 26 != 15 or (x >> 16) & 31 != 0:          # lis rD,imm  (addis rD,0,imm)
        continue
    reg, hi = (x >> 21) & 31, (x & 0xFFFF) << 16
    for b in range(a + 4, a + 4 * WINDOW, 4):
        y = w(b)
        op, rd, ra = y >> 26, (y >> 21) & 31, (y >> 16) & 31
        if op == 14 and ra == reg:                      # addi rD,reg,lo
            lo = y & 0xFFFF
            v = (hi + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
        elif op == 24 and rd == reg:                    # ori rA,reg,lo  (rS is bits 21-25)
            v = hi | (y & 0xFFFF)
        else:
            # stop if the register is overwritten by something else
            if rd == reg and op not in (36, 37, 38, 44, 45, 47, 52, 54, 55, 62):   # not a store
                break
            continue
        if (CODE[0] <= v < CODE[1] and v % 4 == 0 and v not in known and w(v) != 0 and at_boundary(v)
                and not looks_like_table(v) and not is_switch_base(b)):
            found.setdefault(v, []).append(b)

for v in sorted(found):
    print(f"{v:#x}")
print(f"{len(found)} unregistered code addresses built in code", file=sys.stderr)
