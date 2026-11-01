#!/usr/bin/env python3
"""Finds the offset-style jump tables XenonAnalyse misses in Trials HD and prints them as
XenonRecomp [[switch]] entries.

This compiler computes jump targets as  base + table[index] (or table[index] << shift), with
NOPs scheduled between the address halves, in three forms:
  A  lis; rlwinm; addi; lhzx; lis; addi; nop; add; mtctr; bctr        (16-bit offsets)
  B  lis; addi; lbzx; rlwinm; lis; nop; addi; add; mtctr; bctr        (8-bit offsets << shift)
  C  lis; addi; lbzx; lis; nop; addi; nop; add; mtctr; bctr           (8-bit offsets)

Usage: find_offset_switches.py default.pe [existing_switch_tables.toml]  > extra.toml
"""
import re
import struct
import sys

BASE = 0x82000000
pe = open(sys.argv[1], 'rb').read()
existing = set()
if len(sys.argv) > 2:
    existing = {int(x, 16) for x in re.findall(r'base = 0x([0-9A-Fa-f]+)', open(sys.argv[2]).read())}

def w(a): return struct.unpack('>I', pe[a - BASE:a - BASE + 4])[0]
def op(x): return x >> 26
def rd(x): return (x >> 21) & 31
def ra(x): return (x >> 16) & 31
def rb(x): return (x >> 11) & 31
def simm(x): return (x & 0xFFFF) - 0x10000 if x & 0x8000 else x & 0xFFFF
def xo(x): return (x >> 1) & 0x3FF

NOP = 0x60000000
def is_lis(x): return op(x) == 15 and ra(x) == 0
def is_addi(x): return op(x) == 14
def is_rlwinm(x): return op(x) == 21
def is_x(x, n): return op(x) == 31 and xo(x) == n
LBZX, LHZX, ADD = 87, 279, 266

def hi_lo(lis, addi): return ((w(lis) & 0xFFFF) << 16) + simm(w(addi)) & 0xFFFFFFFF

def find_bound(bctr, idx_reg):
    """cmplwi crN,idx,max (then bgt crN to the default) somewhere before the table, following
    register copies (mr idx,src) that happen between the check and the jump."""
    window = []
    for a in range(bctr - 4, bctr - 4 * 64, -4):
        if w(a) == 0x4E800020:                                  # blr: left the function
            break
        window.append(w(a))
    # registers holding the same value as the index (mr rA,rS = or rA,rS,rS), in either direction
    regs = {idx_reg}
    changed = True
    while changed:
        changed = False
        for x in window:
            if op(x) == 31 and xo(x) == 444 and rd(x) == rb(x):
                dst, src = ra(x), rd(x)
                if (dst in regs) != (src in regs):
                    regs |= {dst, src}
                    changed = True
    for x in window:                                            # nearest cmplwi on any of them
        if op(x) == 10 and ra(x) in regs:
            return (x & 0xFFFF) + 1
    return None

out = []
problems = 0
for bctr in range(0x820F0000, 0x8269EE00, 4):
    if w(bctr) != 0x4E800420 or not (op(w(bctr - 4)) == 31 and xo(w(bctr - 4)) == 467):   # bctr after mtctr
        continue
    seq = [w(bctr - 4 * k) for k in range(9, 0, -1)]   # 9 instructions before bctr, oldest first
    form = None
    if is_lis(seq[0]) and is_rlwinm(seq[1]) and is_addi(seq[2]) and is_x(seq[3], LHZX) and is_lis(seq[4]) \
            and is_addi(seq[5]) and seq[6] == NOP and is_x(seq[7], ADD):
        form, start = 'A', bctr - 36
        table = hi_lo(start, start + 8)
        idx = rd(seq[1])                 # rlwinm r0, idx, 1, ... : source register is the index
        idx = (seq[1] >> 21) & 31
        target_base = hi_lo(start + 16, start + 20)
        read = lambda i: struct.unpack('>H', pe[table - BASE + 2 * i:table - BASE + 2 * i + 2])[0]
        shift = 0
    elif is_lis(seq[0]) and is_addi(seq[1]) and is_x(seq[2], LBZX) and is_rlwinm(seq[3]) and is_lis(seq[4]) \
            and seq[5] == NOP and is_addi(seq[6]) and is_x(seq[7], ADD):
        form, start = 'B', bctr - 36
        table = hi_lo(start, start + 4)
        idx = rb(seq[2])
        shift = (seq[3] >> 11) & 31
        target_base = hi_lo(start + 16, start + 24)
        read = lambda i: pe[table - BASE + i]
    elif is_lis(seq[0]) and is_addi(seq[1]) and is_x(seq[2], LBZX) and is_lis(seq[3]) and seq[4] == NOP \
            and is_addi(seq[5]) and seq[6] == NOP and is_x(seq[7], ADD):
        form, start = 'C', bctr - 36
        table = hi_lo(start, start + 4)
        idx = rb(seq[2])
        shift = 0
        target_base = hi_lo(start + 12, start + 20)
        read = lambda i: pe[table - BASE + i]
    if not form:
        continue
    if form == 'A':
        idx = (seq[1] >> 21) & 31   # rlwinm rA,rS,...: rS is bits 21-25
    if start in existing:
        continue

    count = find_bound(bctr, idx)
    if count is None:
        # Fallback: count entries while the targets stay aligned and close to the jump
        count = 0
        while count < 256:
            t = (target_base + (read(count) << shift)) & 0xFFFFFFFF
            if t % 4 or abs(t - bctr) > 0x4000:
                break
            count += 1
        print(f"# note: form {form} switch at {bctr:#x} has no nearby bounds check; inferred {count} entries "
              f"from the table data", file=sys.stderr)
        if count == 0:
            problems += 1
            continue
    labels = [(target_base + (read(i) << shift)) & 0xFFFFFFFF for i in range(count)]
    bad = [l for l in labels if not (0x820F0000 <= l < 0x8269EE00) or l % 4]
    if bad:
        print(f"# WARNING: form {form} switch at {bctr:#x} has invalid targets {[hex(b) for b in bad]}", file=sys.stderr)
        problems += 1
        continue
    out.append(f"[[switch]]\nbase = 0x{start:X}\nr = {idx}\ndefault = 0x0\nlabels = [\n" +
               "".join(f"    0x{l:X},\n" for l in labels) + "]\n")

print(f"# ---- OFFSET JUMPTABLES WITH SCHEDULED NOPS (tools/find_offset_switches.py): {len(out)} ----")
print("\n".join(out))
print(f"found {len(out)} tables, {problems} problems", file=sys.stderr)
