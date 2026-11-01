#!/usr/bin/env python3
"""Minimal Xbox 360 STFS (CON/LIVE/PIRS) extractor with SHA-1 block verification.
Usage: stfs_extract.py <package> <outdir> [--list]
"""
import sys, os, struct, hashlib

def u24le(b, o): return b[o] | (b[o+1] << 8) | (b[o+2] << 16)
def u24be(b, o): return (b[o] << 16) | (b[o+1] << 8) | b[o+2]

class STFS:
    def __init__(self, path):
        self.f = open(path, 'rb')
        hdr = self.read(0, 0x1000)
        self.magic = hdr[:4]
        assert self.magic in (b'CON ', b'LIVE', b'PIRS'), self.magic
        self.header_size = struct.unpack('>I', hdr[0x340:0x344])[0]
        self.content_type = struct.unpack('>I', hdr[0x344:0x348])[0]
        self.title_id = struct.unpack('>I', hdr[0x360:0x364])[0]
        vd = hdr[0x379:0x379+0x24]
        self.block_sep = vd[2]
        self.ft_block_count = struct.unpack('<H', vd[3:5])[0]
        self.ft_block = u24le(vd, 5)
        self.top_hash = vd[8:0x1C]
        self.alloc_blocks = struct.unpack('>I', vd[0x1C:0x20])[0]
        self.sex = (~self.block_sep) & 1          # 0 = female, 1 = male
        self.step = (0xAB, 0x718F) if self.sex == 0 else (0xAC, 0x723A)
        self.base = (self.header_size + 0xFFF) & 0xFFFFF000
        n = self.alloc_blocks
        self.top_level = 0 if n <= 0xAA else (1 if n <= 0x70E4 else 2)
        tb = [0, self.step[0], self.step[1]][self.top_level]
        self.top_addr = (tb << 12) + self.base + ((self.block_sep & 2) << 0xB)
        self.top = self.read(self.top_addr, 0x1000)
        self._check_top()

    def read(self, off, n):
        self.f.seek(off); return self.f.read(n)

    def data_block(self, b):
        s = self.sex
        r = (((b + 0xAA) // 0xAA) << s) + b
        if b < 0xAA: return r
        if b < 0x70E4: return r + (((b + 0x70E4) // 0x70E4) << s)
        return (1 << s) + r + (((b + 0x70E4) // 0x70E4) << s)

    def l0_block(self, b):
        if b < 0xAA: return 0
        n = (b // 0xAA) * self.step[0] + (((b // 0x70E4) + 1) << self.sex)
        if b // 0x70E4 == 0: return n
        return n + (1 << self.sex)

    def l1_block(self, b):
        if b < 0x70E4: return self.step[0]
        return (1 << self.sex) + (b // 0x70E4) * self.step[1]

    def block_addr(self, b): return (self.data_block(b) << 12) + self.base

    def hash_entry_addr(self, b):
        a = (self.l0_block(b) << 12) + self.base + (b % 0xAA) * 0x18
        if self.top_level == 0:
            a += (self.block_sep & 2) << 0xB
        elif self.top_level == 1:
            a += (self.top[(b // 0xAA) * 0x18 + 0x14] & 0x40) << 6
        else:
            l1off = (self.top[(b // 0x70E4) * 0x18 + 0x14] & 0x40) << 6
            pos = (self.l1_block(b) << 12) + self.base + l1off + ((b // 0xAA) % 0xAA) * 0x18
            a += (self.read(pos + 0x14, 1)[0] & 0x40) << 6
        return a

    def hash_entry(self, b):
        e = self.read(self.hash_entry_addr(b), 0x18)
        return e[:0x14], e[0x14], u24be(e, 0x15)

    def _check_top(self):
        if hashlib.sha1(self.top).digest() != self.top_hash:
            print('warning: top hash table SHA-1 mismatch (layout guess may be wrong)')

    def read_block(self, b, verify=True):
        data = self.read(self.block_addr(b), 0x1000)
        if verify:
            h, _, _ = self.hash_entry(b)
            if hashlib.sha1(data).digest() != h:
                raise ValueError(f'SHA-1 mismatch on block {b:#x}')
        return data

    def chain(self, start, count, consecutive):
        b = start
        for i in range(count):
            yield b
            if consecutive: b += 1
            else: b = self.hash_entry(b)[2]

    def entries(self):
        raw = b''.join(self.read_block(b) for b in self.chain(self.ft_block, self.ft_block_count, False))
        out = []
        for i in range(0, len(raw), 0x40):
            e = raw[i:i+0x40]
            nl = e[0x28] & 0x3F
            if nl == 0: continue
            out.append(dict(
                index=i // 0x40,
                name=e[:nl].decode('ascii', 'replace'),
                is_dir=bool(e[0x28] & 0x80),
                consecutive=bool(e[0x28] & 0x40),
                blocks=u24le(e, 0x29),
                start=u24le(e, 0x2F),
                parent=struct.unpack('>h', e[0x32:0x34])[0],
                size=struct.unpack('>I', e[0x34:0x38])[0]))
        return out

    def path_of(self, ents, e):
        byidx = {x['index']: x for x in ents}
        parts = [e['name']]; p = e['parent']
        while p != -1 and p in byidx:
            parts.append(byidx[p]['name']); p = byidx[p]['parent']
        return '/'.join(reversed(parts))

def main():
    pkg, out = sys.argv[1], sys.argv[2]
    s = STFS(pkg)
    print(f"magic={s.magic.decode()} title={s.title_id:08X} type={s.content_type:08X} "
          f"hdr={s.header_size:#x} sex={'male' if s.sex else 'female'} blocks={s.alloc_blocks} "
          f"toplevel={s.top_level} filetable@{s.ft_block:#x}x{s.ft_block_count}")
    ents = s.entries()
    for e in ents:
        p = s.path_of(ents, e)
        print(f"{'D' if e['is_dir'] else 'F'} {e['size']:>11} {e['blocks']:>6} {'C' if e['consecutive'] else 'H'} {p}")
    if '--list' in sys.argv: return
    for e in ents:
        p = os.path.join(out, s.path_of(ents, e))
        if e['is_dir']:
            os.makedirs(p, exist_ok=True); continue
        os.makedirs(os.path.dirname(p), exist_ok=True)
        remaining = e['size']
        with open(p, 'wb') as fo:
            for b in s.chain(e['start'], e['blocks'], e['consecutive']):
                d = s.read_block(b)
                fo.write(d[:min(remaining, 0x1000)]); remaining -= 0x1000
                if remaining <= 0: break
    print('done')

if __name__ == '__main__':
    main()
