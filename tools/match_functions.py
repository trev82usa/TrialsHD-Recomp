#!/usr/bin/env python3
"""Match functions between two versions of an Xbox 360 executable (e.g. before/after a title
update), so hooks and config entries written for one version can be moved to the other.

    match_functions.py <old image.pe> <new image.pe> [--map out.tsv] [addr ...]

Images are the raw PE images (tools/xex_unpack.py turns a default.xex into one). Every function
listed in each image's .pdata table is compared:

  1. exact code match, ignoring branch/call targets              (unique on both sides)
  2. match ignoring all 16-bit immediates too (addresses, offsets) (unique on both sides)
  3. call-graph propagation: if f matched g, the n-th call in f and the n-th call in g are the
     same function (when both have the same number of calls and the callee's shape agrees)
  4. neighbourhood: an unmatched run between two matched functions, with the same number of
     functions of the same sizes on both sides, is paired in order

With addresses on the command line it prints where each one went (an address inside a function
maps to the same offset in its match, when the function didn't change size).
"""
import argparse
import hashlib
import struct
import sys
from collections import defaultdict

BASE = 0x82000000


def sections(img):
    pe = struct.unpack_from("<I", img, 0x3C)[0]
    n = struct.unpack_from("<H", img, pe + 6)[0]
    opt = struct.unpack_from("<H", img, pe + 20)[0]
    out = {}
    for i in range(n):
        o = pe + 24 + opt + i * 40
        name = img[o:o + 8].rstrip(b"\0").decode("latin1")
        va, size = struct.unpack_from("<II", img, o + 12)[0], struct.unpack_from("<I", img, o + 8)[0]
        out.setdefault(name, (va, size))
    return out


class Image:
    def __init__(self, path):
        self.img = open(path, "rb").read()
        sec = sections(self.img)
        pva, psize = sec[".pdata"]
        self.funcs = []  # (start, size_bytes)
        seen = set()
        for i in range(psize // 8):
            a, b = struct.unpack_from(">II", self.img, pva + i * 8)
            n = (b >> 8) & 0x3FFFFF
            if a and n and a not in seen:
                seen.add(a)
                self.funcs.append((a, n * 4))
        self.funcs.sort()
        self.starts = [f[0] for f in self.funcs]
        self.size = dict(self.funcs)

    def words(self, start, size):
        o = start - BASE
        return struct.unpack_from(">%dI" % (size // 4), self.img, o)

    def containing(self, addr):
        import bisect
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0:
            s, n = self.funcs[i]
            if s <= addr < s + n:
                return s
        return None

    def calls(self, start):
        """Targets of 'bl' instructions in order."""
        out = []
        for k, w in enumerate(self.words(start, self.size[start])):
            if (w >> 26) == 18 and (w & 3) == 1:  # bl (relative, link)
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                out.append((start + k * 4 + li) & 0xFFFFFFFF)
        return out


IMM_OPS = {14, 15, 24, 25, 26, 27, 28, 29} | set(range(32, 56)) | {58, 62}


def norm(words, loose):
    out = []
    for w in words:
        op = w >> 26
        if op == 18:
            w &= 0xFC000003
        elif op == 16:
            w &= 0xFFFF0003
        elif loose and op in IMM_OPS:
            w &= 0xFFFF0000
        out.append(w)
    return hashlib.sha1(struct.pack(">%dI" % len(out), *out)).digest()


def match(old, new):
    m = {}  # old start -> new start
    rev = {}

    def add(a, b):
        if a in m or b in rev:
            return False
        m[a] = b
        rev[b] = a
        return True

    for loose in (False, True):
        ho, hn = defaultdict(list), defaultdict(list)
        for s, n in old.funcs:
            if s not in m:
                ho[(norm(old.words(s, n), loose), n)].append(s)
        for s, n in new.funcs:
            if s not in rev:
                hn[(norm(new.words(s, n), loose), n)].append(s)
        for k, a in ho.items():
            b = hn.get(k)
            if b and len(a) == 1 and len(b) == 1:
                add(a[0], b[0])

    # propagate through calls and neighbours until nothing changes
    while True:
        before = len(m)
        for a, b in list(m.items()):
            ca, cb = old.calls(a), new.calls(b)
            if len(ca) != len(cb):
                continue
            for x, y in zip(ca, cb):
                if x in old.size and y in new.size and old.size[x] == new.size[y]:
                    add(x, y)
        # neighbourhood runs
        oi = {s: i for i, s in enumerate(old.starts)}
        ni = {s: i for i, s in enumerate(new.starts)}
        anchors = sorted((oi[a], ni[b]) for a, b in m.items())
        for (o1, n1), (o2, n2) in zip(anchors, anchors[1:]):
            if o2 - o1 == n2 - n1 and o2 - o1 > 1:
                for k in range(1, o2 - o1):
                    a, b = old.starts[o1 + k], new.starts[n1 + k]
                    if a not in m and b not in rev and old.size[a] == new.size[b]:
                        add(a, b)
        if len(m) == before:
            break
    return m


def find_by_pattern(old, new, addr, length=64):
    """For code not in .pdata (small leaf routines): find the same instructions in the new
    image, ignoring branch targets and immediates. Returns a unique new address or None."""
    n = length // 4
    pat = [w for w in old.words(addr, length)]
    def nw(w):
        op = w >> 26
        if op == 18: return w & 0xFC000003
        if op == 16: return w & 0xFFFF0003
        if op in IMM_OPS: return w & 0xFFFF0000
        return w
    want = [nw(w) for w in pat]
    va, size = sections(new.img)[".text"]
    allw = struct.unpack_from(">%dI" % (size // 4), new.img, va)
    first = want[0]
    hits = []
    for i in range(len(allw) - n):
        if nw(allw[i]) == first and all(nw(allw[i + k]) == want[k] for k in range(1, n)):
            hits.append(BASE + va + i * 4)
            if len(hits) > 1:
                break
    return hits[0] if len(hits) == 1 else None


def by_neighbours(old, new, m, addr):
    """Code between two matched functions: if the gap is the same size in both images, the
    address keeps its offset from the function before it."""
    import bisect
    i = bisect.bisect_right(old.starts, addr) - 1
    j = i + 1
    while i >= 0 and old.starts[i] not in m:
        i -= 1
    while j < len(old.starts) and old.starts[j] not in m:
        j += 1
    if i < 0 or j >= len(old.starts):
        return None
    f, g = old.starts[i], old.starts[j]
    if g - f == m[g] - m[f]:
        return m[f] + (addr - f)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("old")
    ap.add_argument("new")
    ap.add_argument("addrs", nargs="*")
    ap.add_argument("--map", help="write old<TAB>new<TAB>same_size for every matched function")
    a = ap.parse_args()
    old, new = Image(a.old), Image(a.new)
    m = match(old, new)
    print(f"functions: old {len(old.funcs)}, new {len(new.funcs)}, matched {len(m)} "
          f"({100 * len(m) / len(old.funcs):.1f}%)", file=sys.stderr)
    if a.map:
        with open(a.map, "w") as f:
            for x, y in sorted(m.items()):
                f.write(f"{x:08X}\t{y:08X}\t{int(old.size[x] == new.size[y])}\n")
    for s in a.addrs:
        addr = int(s.replace("sub_", "0x"), 16)
        fs = old.containing(addr)
        if fs is None:
            hit = by_neighbours(old, new, m, addr) or find_by_pattern(old, new, addr)
            print(f"{addr:08X} -> {hit:08X}  (by position)" if hit else f"{addr:08X}  not in .pdata and no unique pattern match")
        elif fs not in m:
            print(f"{addr:08X}  (in {fs:08X})  NO MATCH")
        else:
            nb = m[fs]
            same = old.size[fs] == new.size[nb]
            changed = "" if norm(old.words(fs, old.size[fs]), False) == norm(new.words(nb, new.size[nb]), False) else "  code changed"
            print(f"{addr:08X} -> {nb + (addr - fs):08X}  ({'same size' if same else 'SIZE CHANGED'}){changed}")


if __name__ == "__main__":
    main()
