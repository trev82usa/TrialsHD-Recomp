#!/usr/bin/env python3
"""Adds explicit function boundaries to TrialsHD.toml for functions that XenonAnalyse cut short at a
jump table's bctr (reported by XenonRecomp as "Switch case ... is trying to jump outside function").

The function keeps the start XenonAnalyse found; its end is extended past the furthest case label,
then repeatedly past any forward branch target, up to the next blr or unconditional branch.

Usage: fix_switch_bounds.py default.pe recomp.log ppc/ppc_func_mapping.cpp config/TrialsHD.toml config/TrialsHD_switch_tables.toml
"""
import bisect
import re
import struct
import sys

pe_path, log_path, mapping_path, config_path, switches_path = sys.argv[1:6]
BASE = 0x82000000
pe = open(pe_path, 'rb').read()
def w(a): return struct.unpack('>I', pe[a - BASE:a - BASE + 4])[0]

def branch_target(a, x):
    op = x >> 26
    if op == 18 and not x & 1:
        li = x & 0x3FFFFFC
        return a + (li - 0x4000000 if li & 0x2000000 else li)
    if op == 16 and not x & 1:
        bd = x & 0xFFFC
        return a + (bd - 0x10000 if bd & 0x8000 else bd)
    return None

log = open(log_path).read()
bad = sorted({int(x, 16) for x in re.findall(r'Switch case at ([0-9A-F]+) is trying to jump outside', log)})
tables = {}
for m in re.finditer(r'base = 0x([0-9A-F]+)\nr = \d+\ndefault = 0x[0-9A-F]+\nlabels = \[\n(.*?)\]',
                     open(switches_path).read(), re.S):
    tables[int(m.group(1), 16)] = [int(x, 16) for x in re.findall(r'0x([0-9A-F]+)', m.group(2))]
starts = sorted(int(x, 16) for x in re.findall(r'\{ 0x([0-9A-F]+), sub_', open(mapping_path).read()))
config = open(config_path).read()
known = {int(x, 16) for x in re.findall(r'address = 0x([0-9A-F]+)', config)}

entries = []
def add_bound(start, bctr, labels):
    global config
    table_lo, table_hi = bctr + 4, bctr + 4 + 4 * len(labels)   # inline absolute tables follow bctr
    end = max(labels)
    while True:
        e = end
        while w(e) != 0x4E800020 and (w(e) >> 26) != 18:   # to the next blr / unconditional b
            e += 4
        grew = False
        for a in range(start, e + 4, 4):
            if table_lo <= a < table_hi:
                continue
            t = branch_target(a, w(a))
            if t and e < t < e + 0x800 and not ((w(a) >> 26) == 18 and t in starts):
                end, grew = t, True
        if not grew:
            break
    size = e + 4 - start
    existing = re.search(r"\{ address = 0x%X, size = 0x([0-9A-F]+) \}" % start, config)
    if existing:
        if int(existing.group(1), 16) < size:   # enlarge an entry added for another reason
            config = config.replace(existing.group(0), "{ address = 0x%X, size = 0x%X }" % (start, size))
            print(f"enlarged {start:#x} to {size:#x}", file=sys.stderr)
        return
    if start in known:
        return
    entries.append(f"    {{ address = 0x{start:X}, size = 0x{size:X} }}, # leaf with jump table at {bctr:#X}")
    known.add(start)
    print(f"bctr {bctr:#x}: function {start:#x} size {size:#x}", file=sys.stderr)

# Which generated functions actually contain an out-of-range switch: read them from the output.
import glob, os
ppc_dir = os.path.dirname(mapping_path)
offenders = {}   # function start -> bctr addresses inside it
for path in glob.glob(os.path.join(ppc_dir, 'ppc_recomp.*.cpp')):
    current = None
    for line in open(path):
        m = re.match(r'PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\)', line)
        if m:
            current = int(m.group(1), 16)
        elif '// ERROR: 0x' in line and current is not None:
            offenders.setdefault(current, set())
for start in sorted(offenders):
    inside = [b for b in bad if b >= start]
    if not inside:
        continue
    bctr = min(inside)   # the first out-of-range switch after this function's start
    add_bound(start, bctr, tables[max(b for b in tables if b < bctr)])

if entries:
    idx = config.rindex(']')
    config = config[:idx] + "\n".join(entries) + "\n" + config[idx:]
open(config_path, 'w').write(config)
print(f"added {len(entries)} function boundaries", file=sys.stderr)
