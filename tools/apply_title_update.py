#!/usr/bin/env python3
"""Apply an Xbox 360 title update to a game's default.xex, for static recompilation.

A title update (TU) is a delta patch (default.xexp) against the game's original default.xex.
A static recomp can't load new code at runtime, so the update has to be applied *before*
running `rexglue codegen`. This tool does exactly what the console does when it loads a TU,
and writes the result as a plain (unencrypted, uncompressed) default.xex that ReXGlue reads
like any other.

Usage:
    apply_title_update.py <base default.xex> <TU package or default.xexp> <output default.xex>
        [--runtime <path to librexruntime.so / rexruntime.dll>]

The TU can be the package file exactly as downloaded (an STFS "LIVE"/"CON " file) or the
default.xexp already extracted from it. Updates are cumulative: use the newest one made for
your copy (same Media ID and base version; the tool checks and tells you if it doesn't match).

The LZX delta decompression comes from the ReXGlue runtime library (it's the same code the
emulator core uses), so point --runtime at the librexruntime.so / rexruntime.dll that ships
with any ReXGlue build if it isn't found automatically.

Needs: Python 3.8+ and the `cryptography` package (pip install cryptography).
"""
import argparse
import ctypes
import hashlib
import os
import platform
import struct
import sys
from pathlib import Path

try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
except ImportError:
    sys.exit("This tool needs the 'cryptography' package: pip install cryptography")

RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")  # public, from Xenia
DEVKIT_KEY = bytes(16)

OPT_FILE_FORMAT = 0x000003FF
OPT_DELTA_PATCH = 0x000005FF
OPT_EXEC_INFO = 0x00040006


# --------------------------------------------------------------------------------------------
# Small helpers
# --------------------------------------------------------------------------------------------
def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def u16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def aes_ecb_dec(key, data):
    d = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
    return d.update(data) + d.finalize()


def aes_cbc_dec(key, data):
    data = data[: len(data) & ~0xF]
    d = Cipher(algorithms.AES(key), modes.CBC(bytes(16))).decryptor()
    return d.update(data) + d.finalize()


def fmt_version(v):
    return f"{v >> 28}.{(v >> 24) & 15}.{(v >> 8) & 0xFFFF}.{v & 0xFF}"


class Xex:
    """Read-only view of an XEX2 file's headers."""

    def __init__(self, data: bytes, name: str):
        if data[:4] != b"XEX2":
            sys.exit(f"{name}: not an XEX2 file")
        self.data = data
        self.name = name
        self.module_flags, self.header_size, _, self.sec_off, n = struct.unpack_from(">IIIII", data, 4)
        self.opt = {}
        for i in range(n):
            k, v = struct.unpack_from(">II", data, 24 + i * 8)
            self.opt[k] = v

    @property
    def is_patch(self):
        return bool(self.module_flags & 0x30)  # XEX_MODULE_PATCH_DELTA | PATCH_FULL

    def opt_ptr(self, key):
        return self.opt.get(key)

    def exec_info(self):
        p = self.opt_ptr(OPT_EXEC_INFO)
        if p is None:
            return None
        media, ver, base, tid = struct.unpack_from(">IIII", self.data, p)
        return dict(media_id=media, version=ver, base_version=base, title_id=tid)


# --------------------------------------------------------------------------------------------
# Inputs
# --------------------------------------------------------------------------------------------
def read_xexp(path: Path) -> bytes:
    """Return default.xexp bytes from either a raw .xexp or a TU STFS package."""
    data = path.read_bytes()
    if data[:4] == b"XEX2":
        return data
    if data[:4] not in (b"LIVE", b"PIRS", b"CON "):
        sys.exit(f"{path}: neither an XEX2 patch nor an STFS package")
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import stfs_extract  # tools/stfs_extract.py

    files = stfs_extract.read_package(str(path))
    for name, blob in files.items():
        if name.lower().endswith(".xexp"):
            print(f"Using {name} from the update package ({len(blob)} bytes)")
            extra = [n for n in files if n != name]
            if extra:
                print("Note: the package also contains data files; extract them with "
                      "stfs_extract.py into the game's update folder:", ", ".join(extra))
            return blob
    sys.exit(f"{path}: no .xexp inside this package (is it a title update?)")


def load_base_image(x: Xex) -> (bytearray, bytes):
    """Decrypt/decompress the base XEX image. Returns (image, session_key)."""
    d = x.data
    ff = x.opt_ptr(OPT_FILE_FORMAT)
    ffsize, enc, comp = struct.unpack_from(">IHH", d, ff)
    image_size = u32(d, x.sec_off + 4)
    payload = d[x.header_size:]
    session_key = aes_ecb_dec(RETAIL_KEY, d[x.sec_off + 0x150 : x.sec_off + 0x160])
    if enc == 1:
        payload = aes_cbc_dec(session_key, payload)
    if comp == 0:
        img = bytearray(payload)
    elif comp == 1:
        img, p = bytearray(), 0
        for i in range((ffsize - 8) // 8):
            ds, zs = struct.unpack_from(">II", d, ff + 8 + i * 8)
            img += payload[p : p + ds]
            img += bytes(zs)
            p += ds
    else:
        sys.exit(f"{x.name}: LZX-compressed base XEX isn't supported yet (compression type {comp}). "
                 "Use an XEX tool to decompress it first.")
    img = img[:image_size]
    img += bytes(image_size - len(img))
    if img[:2] != b"MZ":
        sys.exit(f"{x.name}: decryption failed (no MZ header) - is this a retail XEX?")
    return img, session_key


# --------------------------------------------------------------------------------------------
# LZX delta (from the ReXGlue runtime)
# --------------------------------------------------------------------------------------------
def find_runtime(explicit):
    names = ["rexruntime.dll"] if platform.system() == "Windows" else ["librexruntime.so"]
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    here = Path(__file__).resolve().parent
    for base in [Path.cwd(), here, here.parent, here.parent / "TrialsHD"]:
        for n in names:
            candidates.append(base / n)
    for c in candidates:
        if c.is_file():
            return c
    sys.exit("Couldn't find the ReXGlue runtime library (librexruntime.so / rexruntime.dll). "
             "Pass it with --runtime, e.g. --runtime /path/to/TrialsHD/librexruntime.so")


def load_lzxdelta(runtime_path):
    lib = ctypes.CDLL(str(runtime_path))
    # int lzxdelta_apply_patch(rex::xex2_delta_patch*, size_t, uint32_t window_size, void* dest)
    for sym in ("_Z20lzxdelta_apply_patchPN3rex16xex2_delta_patchEmjPv",          # gcc/clang
                "?lzxdelta_apply_patch@@YAHPEAUxex2_delta_patch@rex@@_KIPEAX@Z"):  # msvc
        try:
            f = getattr(lib, sym)
            break
        except AttributeError:
            f = None
    if f is None:
        sys.exit(f"{runtime_path}: lzxdelta_apply_patch not found (ReXGlue SDK 0.9 or newer needed)")
    f.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_void_p]
    f.restype = ctypes.c_int

    def apply(patch: bytes, window: int, dest: bytearray):
        pbuf = ctypes.create_string_buffer(bytes(patch), len(patch) + 16)
        dbuf = (ctypes.c_char * len(dest)).from_buffer(dest)
        r = f(ctypes.addressof(pbuf), len(patch), window, ctypes.addressof(dbuf))
        del dbuf
        return r

    return apply


# --------------------------------------------------------------------------------------------
# The patch (port of rex::runtime::XexModule::ApplyPatch)
# --------------------------------------------------------------------------------------------
def apply_patch(base: Xex, patch: Xex, lzx_apply):
    pd = patch.opt_ptr(OPT_DELTA_PATCH)
    if pd is None or not patch.is_patch:
        sys.exit(f"{patch.name}: not a delta patch")
    P = patch.data
    (_, target_ver, source_ver) = struct.unpack_from(">III", P, pd)
    digest_source = P[pd + 0x0C : pd + 0x20]
    image_key_source = P[pd + 0x20 : pd + 0x30]
    (size_of_target_headers, hdr_src_off, hdr_src_size, hdr_tgt_off,
     img_src_off, img_src_size, img_tgt_off) = struct.unpack_from(">7I", P, pd + 0x30)
    info_off = pd + 0x4C

    # Does the update belong to this base?
    base_digest = hashlib.sha1(base.data[base.sec_off + 8 : base.sec_off + 0x108]).digest()
    bi = base.exec_info() or {}
    print(f"Base:   title {bi.get('title_id', 0):08X}  media {bi.get('media_id', 0):08X}  "
          f"version {fmt_version(bi.get('version', 0))}")
    print(f"Update: from version {fmt_version(source_ver)} to {fmt_version(target_ver)}")
    if base_digest != digest_source:
        print("WARNING: this update was made for a different default.xex (signature doesn't "
              "match). It will most likely fail - check the Media ID and base version.")

    # ---- headers
    header = bytearray(base.data[: base.header_size])
    orig_image_size = u32(header, base.sec_off + 4)
    target_size = size_of_target_headers or (hdr_tgt_off + hdr_src_size)
    if hdr_src_off > len(header) or hdr_src_size > len(header) - hdr_src_off:
        sys.exit("Header patch source is outside the base headers")
    if target_size > len(header):
        header += bytes(target_size - len(header))
    if hdr_src_off:
        header[hdr_tgt_off : hdr_tgt_off + hdr_src_size] = header[hdr_src_off : hdr_src_off + hdr_src_size]
    if target_size < len(header):
        header[target_size:] = bytes(len(header) - target_size)

    ff = patch.opt_ptr(OPT_FILE_FORMAT)
    _, p_enc, p_comp = struct.unpack_from(">IHH", P, ff)
    if p_comp != 3:  # XEX_COMPRESSION_DELTA
        sys.exit(f"Unexpected patch compression type {p_comp}")
    window = u32(P, ff + 8)
    first_block = (u32(P, ff + 12), P[ff + 16 : ff + 36])

    info_comp_len = u16(P, info_off + 10)
    r = lzx_apply(P[info_off : info_off + info_comp_len + 0xC], window, header)
    if r:
        sys.exit(f"Header patch failed (LZX error {r})")
    del header[target_size:]

    new = Xex(bytes(header), "patched headers")
    new_image_size = u32(header, new.sec_off + 4)

    # ---- keys
    _, base_session = load_base_image(base)
    new_base_session = aes_ecb_dec(RETAIL_KEY, bytes(header[new.sec_off + 0x150 : new.sec_off + 0x160]))
    patch_session = aes_ecb_dec(new_base_session, P[patch.sec_off + 0x150 : patch.sec_off + 0x160])
    if aes_ecb_dec(new_base_session, image_key_source) != base_session:
        sys.exit("The update's image key doesn't match this default.xex - wrong base game version?")

    # ---- image
    image, _ = load_base_image(base)
    if new_image_size > len(image):
        image += bytes(new_image_size - len(image))
    payload = P[patch.header_size :]
    if p_enc == 1:
        payload = aes_cbc_dec(patch_session, payload)

    if img_src_off:
        image[img_tgt_off : img_tgt_off + img_src_size] = image[img_src_off : img_src_off + img_src_size]
    image_target_size = img_tgt_off + img_src_size
    if image_target_size < orig_image_size:
        image[image_target_size:orig_image_size] = bytes(orig_image_size - image_target_size)

    p = 0
    block_size, block_hash = first_block
    blocks = 0
    while block_size:
        block = payload[p : p + block_size]
        if len(block) < block_size or hashlib.sha1(block).digest() != block_hash:
            sys.exit(f"Patch block {blocks} hash mismatch (wrong key or damaged file)")
        next_size = u32(block, 0)
        next_hash = block[4:24]
        r = lzx_apply(block[24:], window, image)
        if r:
            sys.exit(f"Image patch block {blocks} failed (LZX error {r})")
        p += block_size
        block_size, block_hash = next_size, next_hash
        blocks += 1
    del image[new_image_size:]
    if image[:2] != b"MZ":
        sys.exit("Patched image has no MZ header - the update didn't apply correctly")
    print(f"Applied {blocks} patch blocks; image {orig_image_size:#x} -> {new_image_size:#x} bytes")
    return header, image


def write_plain_xex(header: bytearray, image: bytearray, out: Path):
    """Write headers + raw image as an unencrypted, uncompressed ('basic', one block) XEX2."""
    x = Xex(bytes(header), "patched")
    ff = x.opt_ptr(OPT_FILE_FORMAT)
    old_size = u32(header, ff)
    if old_size < 16:
        sys.exit("File-format header too small to rewrite")
    struct.pack_into(">IHHII", header, ff, 16, 0, 1, len(image), 0)  # no encryption, basic
    header[ff + 16 : ff + old_size] = bytes(old_size - 16)
    struct.pack_into(">I", header, 8, len(header))  # PE data offset = right after the headers
    struct.pack_into(">I", header, 4, u32(header, 4) & ~0x30)  # no longer a patch module
    out.write_bytes(bytes(header) + bytes(image))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("base", type=Path, help="the game's original default.xex")
    ap.add_argument("update", type=Path, help="title update package (as downloaded) or default.xexp")
    ap.add_argument("output", type=Path, help="where to write the updated default.xex")
    ap.add_argument("--runtime", help="path to librexruntime.so / rexruntime.dll")
    a = ap.parse_args()

    base = Xex(a.base.read_bytes(), str(a.base))
    if base.is_patch:
        sys.exit(f"{a.base}: this is a patch, not a base game executable")
    patch = Xex(read_xexp(a.update), str(a.update))
    lzx = load_lzxdelta(find_runtime(a.runtime))
    header, image = apply_patch(base, patch, lzx)
    write_plain_xex(header, image, a.output)
    ni = Xex(a.output.read_bytes(), str(a.output)).exec_info() or {}
    print(f"Wrote {a.output}: title {ni.get('title_id', 0):08X}, version "
          f"{fmt_version(ni.get('version', 0))}. Point rexglue codegen at this file.")


if __name__ == "__main__":
    main()
