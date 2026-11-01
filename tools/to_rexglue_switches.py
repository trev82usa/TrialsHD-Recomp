#!/usr/bin/env python3
"""Converts XenonRecomp [[switch]] tables (keyed by the first instruction of the dispatch sequence)
into ReXGlue [[switch_tables]] entries (keyed by the bctr address).

Usage: to_rexglue_switches.py default.pe TrialsHD_switch_tables.toml > rexglue_switch_tables.toml
"""
import re
import struct
import sys

BASE = 0x82000000
pe = open(sys.argv[1], 'rb').read()
text = open(sys.argv[2]).read()

def w(a): return struct.unpack('>I', pe[a - BASE:a - BASE + 4])[0]

out = []
for m in re.finditer(r'base = 0x([0-9A-Fa-f]+)\nr = (\d+)\ndefault = 0x[0-9A-Fa-f]+\nlabels = \[\n(.*?)\]', text, re.S):
    start, reg = int(m.group(1), 16), int(m.group(2))
    labels = re.findall(r'0x[0-9A-Fa-f]+', m.group(3))
    bctr = next((a for a in range(start, start + 4 * 20, 4) if w(a) == 0x4E800420), None)
    if bctr is None:
        sys.exit(f"no bctr after {start:#x}")
    out.append(f"[[switch_tables]]\naddress = 0x{bctr:X}\nregister = {reg}\nlabels = [{', '.join(labels)}]\n")

print(f"# {len(out)} jump tables found by hand for Trials HD (converted from the XenonRecomp config)\n")
print("\n".join(out))
