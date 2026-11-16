#!/usr/bin/env python3
"""
Game-file installer for the ReXGlue ports (settings for this game below the imports).

Takes your own Trials HD package file (the one from Content/0000000000000000/5841095A/
000D0000/ on your Xbox 360 storage), extracts default.xex, ArcadeInfo.xml and the data folder
into the game's assets folder, and checks that default.xex is the version this build needs.

If your copy is an older version, also pick the title update (either the title update package
from 5841095A/000B0000/, or a default.xexp file) and xextool.exe; the installer applies the update
to default.xex for you and keeps the original as default.xex.orig.

Double-click to open the window (needs Python 3 with Tk; on Windows use
TrialsHD-Installer.exe, which needs nothing). No window available? Command line:
  install_game.py --package <file> [--update <TU package or .xexp>] [--xextool <xextool>]
                  [--dest <assets folder>]
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading

# --- per-game settings (the same installer is used by the other ports) ---
GAME = "Trials HD"
TITLE_ID = 0x5841095A
REQUIRED_VERSION = 258           # default.xex version this build was recompiled from
DATA_DIR = "data"                # the game's data folder inside the package
PROGRAM = "trialshd"            # the game's program name (.exe on Windows)
INSTALLER_EXE = "TrialsHD-Installer.exe"


def here():
    if getattr(sys, 'frozen', False):          # PyInstaller exe
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))


sys.path.insert(0, here())
sys.path.insert(0, os.path.join(here(), 'tools'))
from stfs_extract import STFS  # noqa: E402  (shipped next to this script)


def default_dest():
    base = here()
    if os.path.basename(base) == 'tools':     # running from the repository
        base = os.path.dirname(base)
    return os.path.join(base, 'assets')


def vstr(v):
    return f"{v >> 28}.{(v >> 24) & 15}.{(v >> 8) & 0xFFFF}.{v & 255}"


def xex_info(path):
    """(title_id, version) from the XEX header (plain text, no decryption)."""
    with open(path, 'rb') as f:
        d = f.read(0x10000)
    if d[:4] != b'XEX2':
        return None, None
    n = struct.unpack('>I', d[20:24])[0]
    for i in range(n):
        key, val = struct.unpack('>II', d[24 + i * 8:32 + i * 8])
        if key == 0x00040006:
            ver = struct.unpack('>I', d[val + 4:val + 8])[0]
            tid = struct.unpack('>I', d[val + 12:val + 16])[0]
            return tid, ver
    return None, None


def extract_package(pkg, out, log, wanted=None):
    s = STFS(pkg)
    log(f"Package: title {s.title_id:08X}, {s.alloc_blocks} blocks")
    ents = s.entries()
    files = [e for e in ents if not e['is_dir']]
    total = sum(e['size'] for e in files) or 1
    done = 0
    written = []
    for e in ents:
        rel = s.path_of(ents, e)
        if wanted and not wanted(rel):
            continue
        p = os.path.join(out, *rel.split('/'))
        if e['is_dir']:
            os.makedirs(p, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(p), exist_ok=True)
        remaining = e['size']
        with open(p, 'wb') as fo:
            for b in s.chain(e['start'], e['blocks'], e['consecutive']):
                fo.write(s.read_block(b)[:min(remaining, 0x1000)])
                remaining -= 0x1000
                if remaining <= 0:
                    break
        done += e['size']
        written.append(rel)
        log(f"  {rel}  ({done * 100 // total}%)", progress=True)
    return s, written


def find_xextool(given):
    if given:
        return given
    for name in ('xextool.exe', 'xextool', 'XexTool.exe'):
        for d in (here(), os.path.join(here(), 'tools')):
            if os.path.isfile(os.path.join(d, name)):
                return os.path.join(d, name)
        w = shutil.which(name)
        if w:
            return w
    return None


def apply_update(dest, update, xextool, log):
    xex = os.path.join(dest, 'default.xex')
    with tempfile.TemporaryDirectory() as tmp:
        if update.lower().endswith('.xexp'):
            xexp = update
        else:
            log("Reading the title update package...")
            _, written = extract_package(update, tmp, log, wanted=lambda r: True)
            cands = [w for w in written if w.lower().endswith('.xexp')]
            pick = [w for w in cands if os.path.basename(w).lower() == 'default.xexp'] or cands
            if not pick:
                raise RuntimeError("That title update package has no default.xexp in it.")
            xexp = os.path.join(tmp, *pick[0].split('/'))
        tool = find_xextool(xextool)
        if not tool:
            raise RuntimeError(
                "Applying the title update needs xextool (xorloser's XexTool). Put xextool.exe "
                "next to this installer, or pick it in the XexTool box, and press Install again.")
        patched = os.path.join(tmp, 'default_patched.xex')
        cmd = [tool, '-p', xexp, '-o', patched, xex]
        if os.name != 'nt' and tool.lower().endswith('.exe') and shutil.which('wine'):
            cmd = ['wine'] + cmd
        log("Applying the title update: " + ' '.join(os.path.basename(c) for c in cmd))
        r = subprocess.run(cmd, capture_output=True, text=True)
        for line in (r.stdout + r.stderr).splitlines()[-8:]:
            log("  xextool: " + line)
        if r.returncode != 0 or not os.path.isfile(patched):
            raise RuntimeError("xextool could not apply the update (see the lines above).")
        backup = xex + '.orig'
        if not os.path.exists(backup):
            shutil.copy2(xex, backup)
        shutil.copy2(patched, xex)
        log(f"Updated default.xex (original kept as {os.path.basename(backup)}).")


def install(package, dest, update, xextool, log):
    if not package or not os.path.isfile(package):
        raise RuntimeError(f"Pick your {GAME} package file first.")
    os.makedirs(dest, exist_ok=True)
    log(f"Extracting into {dest}")
    s, _ = extract_package(package, dest, log)
    if s.title_id != TITLE_ID:
        raise RuntimeError(f"That package is title {s.title_id:08X}, not {GAME} "
                           f"({TITLE_ID:08X}). If it's the title update, put it in the second box.")
    xex = os.path.join(dest, 'default.xex')
    if not os.path.isfile(xex):
        raise RuntimeError("The package didn't contain default.xex.")
    if not os.path.isdir(os.path.join(dest, DATA_DIR)):
        raise RuntimeError(f"The package didn't contain the {DATA_DIR} folder.")
    tid, ver = xex_info(xex)
    log(f"default.xex version {vstr(ver or 0)} (needed: {vstr(REQUIRED_VERSION)})")
    if ver != REQUIRED_VERSION:
        if not update:
            raise RuntimeError(
                f"Your copy is version {vstr(ver or 0)}; this build needs {vstr(REQUIRED_VERSION)}, "
                f"the updated release. Pick your title update (the package from "
                f"{TITLE_ID:08X}/000B0000/, or a default.xexp file) in the second box and press "
                "Install again.")
        apply_update(dest, update, xextool, log)
        tid, ver = xex_info(xex)
        log(f"default.xex is now version {vstr(ver or 0)}")
        if ver != REQUIRED_VERSION:
            raise RuntimeError(
                f"After the update default.xex is version {vstr(ver or 0)}, still not "
                f"{vstr(REQUIRED_VERSION)}. This title update is a different one from the "
                "version the port was built from.")
    log(f"All done. Start the game with {PROGRAM} (Linux) or {PROGRAM}.exe (Windows).")


# ---------------------------------------------------------------------------------------------

def run_gui():
    import tkinter as tk
    from tkinter import filedialog, messagebox, scrolledtext

    root = tk.Tk()
    root.title(f"{GAME} - install game files")
    root.minsize(640, 420)
    vars_ = {k: tk.StringVar() for k in ('package', 'update', 'xextool', 'dest')}
    vars_['dest'].set(default_dest())
    vars_['xextool'].set(find_xextool(None) or '')

    rows = [
        ('package', f"1. Game package (from {TITLE_ID:08X}/000D0000/)", 'file'),
        ('update', "2. Title update - only if asked (package from 000B0000/ or .xexp)", 'file'),
        ('xextool', "3. xextool.exe - only needed with a title update", 'file'),
        ('dest', "Install into (the game's assets folder)", 'dir'),
    ]
    for r, (key, label, kind) in enumerate(rows):
        tk.Label(root, text=label, anchor='w').grid(row=r * 2, column=0, columnspan=2,
                                                    sticky='we', padx=8, pady=(8, 0))
        tk.Entry(root, textvariable=vars_[key]).grid(row=r * 2 + 1, column=0, sticky='we', padx=8)

        def browse(key=key, kind=kind):
            p = (filedialog.askdirectory() if kind == 'dir'
                 else filedialog.askopenfilename(title="Choose a file"))
            if p:
                vars_[key].set(p)
        tk.Button(root, text="Browse...", command=browse).grid(row=r * 2 + 1, column=1, padx=8)
    root.columnconfigure(0, weight=1)

    logbox = scrolledtext.ScrolledText(root, height=12, state='disabled')
    logbox.grid(row=9, column=0, columnspan=2, sticky='nsew', padx=8, pady=8)
    root.rowconfigure(9, weight=1)
    btn = tk.Button(root, text="Install", width=16)
    btn.grid(row=10, column=0, columnspan=2, pady=(0, 10))

    def log(msg, progress=False):
        def put():
            logbox.configure(state='normal')
            if progress and logbox.get('end-2l', 'end-1c').startswith('  '):
                logbox.delete('end-2l', 'end-1c')
            logbox.insert('end', msg + '\n')
            logbox.see('end')
            logbox.configure(state='disabled')
        root.after(0, put)

    def work():
        try:
            install(vars_['package'].get().strip(), vars_['dest'].get().strip(),
                    vars_['update'].get().strip() or None, vars_['xextool'].get().strip() or None, log)
            root.after(0, lambda: messagebox.showinfo(GAME, "Game files installed."))
        except Exception as e:  # show every problem in plain words
            msg = str(e)
            log("Problem: " + msg)
            root.after(0, lambda: messagebox.showerror(GAME, msg))
        finally:
            root.after(0, lambda: btn.configure(state='normal'))

    def start():
        btn.configure(state='disabled')
        threading.Thread(target=work, daemon=True).start()
    btn.configure(command=start)
    root.mainloop()


def main():
    ap = argparse.ArgumentParser(description=f"Install {GAME} game files.")
    ap.add_argument('--package')
    ap.add_argument('--update')
    ap.add_argument('--xextool')
    ap.add_argument('--dest', default=default_dest())
    a = ap.parse_args()
    if not a.package:
        try:
            run_gui()
            return
        except ImportError:  # no Tk: ask on the command line
            print("(No window toolkit found; on Debian/Ubuntu: sudo apt install python3-tk)")
            a.package = input(f"Path to your {GAME} package file: ").strip().strip('"')
            a.update = input("Title update file (Enter to skip): ").strip().strip('"') or None

    def log(msg, progress=False):
        print(msg)
    try:
        install(a.package, a.dest, a.update, a.xextool, log)
    except Exception as e:
        print("Problem: " + str(e))
        sys.exit(1)


if __name__ == '__main__':
    main()
