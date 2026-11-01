#!/usr/bin/env python3
"""Decrypt + decompress a retail XEX2 (basic/no compression) to its raw PE image.
Usage: xex_unpack.py default.xex default.pe"""
import sys, struct
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
RETAIL_KEY = bytes.fromhex('20B185A59D28FDC340583FBB0896BF91')   # public, from Xenia

d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'XEX2'
pe_off, sec_off, n = struct.unpack('>I4xII', d[8:24])
hdrs = dict(struct.unpack('>II', d[24+i*8:32+i*8]) for i in range(n))
ff = hdrs[0x3FF]
ffsize, enc, comp = struct.unpack('>IHH', d[ff:ff+8])
image_size = struct.unpack('>I', d[sec_off+4:sec_off+8])[0]
payload = d[pe_off:]
if enc == 1:
    fk = d[sec_off+0x150:sec_off+0x160]
    ecb = Cipher(algorithms.AES(RETAIL_KEY), modes.ECB()).decryptor()
    sk = ecb.update(fk) + ecb.finalize()
    payload = payload[:len(payload) & ~0xF]
    cbc = Cipher(algorithms.AES(sk), modes.CBC(b'\0'*16)).decryptor()
    payload = cbc.update(payload) + cbc.finalize()
if comp == 0:
    img = payload
elif comp == 1:
    out, p = bytearray(), 0
    for i in range((ffsize - 8) // 8):
        ds, zs = struct.unpack('>II', d[ff+8+i*8:ff+16+i*8])
        out += payload[p:p+ds]; out += b'\0' * zs; p += ds
    img = bytes(out)
else:
    sys.exit(f'compression type {comp} (LZX/delta) not supported')
img = img[:image_size].ljust(image_size, b'\0')
assert img[:2] == b'MZ', 'decryption failed (no MZ header)'
open(sys.argv[2], 'wb').write(img)
print(f'wrote {sys.argv[2]}: {len(img):#x} bytes')
