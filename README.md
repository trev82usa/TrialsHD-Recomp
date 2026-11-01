# Trials HD Recomp

A native Windows port of **Trials HD** (Xbox Live Arcade, 2009) made by static recompilation with
[ReXGlue](https://github.com/rexglue/rexglue-sdk). The game's Xbox 360 PowerPC code is translated
to C++ and compiled into a normal Windows executable, with Xbox 360 graphics and audio handled by
ReXGlue's runtime (D3D12).

> **No game data is included anywhere in this project.** You need your own legally obtained copy
> of Trials HD. Please don't post game files, or links to them, in issues or pull requests.
>
> Not affiliated with or endorsed by Ubisoft, RedLynx or Microsoft.

## Status

- Boots, menus, full game (not the trial), tracks play with audio
- Holds 60 FPS at any resolution on a mid-range PC (tested: Radeon RX 6750 XT)
- Short hitch when a track loads; some engine threads use more CPU than they should

## Download and play

1. Download **TrialsHD.rar** from the [latest release](../../releases/latest) and extract it.
2. Get your game files: `default.xex` and the `data` folder from your copy of Trials HD. The XBLA
   package is an Xbox 360 STFS (LIVE) file; extract it with
   `python tools/stfs_extract.py <your Trials HD package file> extracted`.
3. Copy `default.xex` and `data` into the `assets` folder next to `trialshd.exe`.
4. Run `trialshd.exe`.

Requirements: 64-bit Windows 10/11, a DirectX 12 graphics card, a CPU with SSE4.2 (2009 or later),
and the [Microsoft Visual C++ Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe)
(install it if Windows says `MSVCP140.dll` or `VCRUNTIME140.dll` is missing).

In game: **F3** shows the FPS overlay, **F4** opens settings. Options live in `trialshd.toml`.
If it crashes, `crash.txt` and the newest file in `logs` show what happened; include them in an issue.

Saves and the shader cache are stored in your Windows user folder, not next to the exe.

## Building it yourself (no local tools needed)

GitHub Actions can build the exe for you in a **private** copy of this repository (the build needs
your game's `default.xex`).

1. Create a **private** repository from this one: use **Use this template → Create a new
   repository** and set it to *Private* (a fork of a public repository is always public, so don't fork).
2. Get `default.xex` out of your Trials HD package. The package is an Xbox 360 STFS (LIVE) file;
   extract it with:
   ```
   python tools/stfs_extract.py <your Trials HD package file> extracted
   ```
3. In your private copy, add the executable and push:
   ```
   git add -f private/default.xex
   git commit -m "Add my default.xex"
   git push
   ```
4. Open **Actions → Build TrialsHD.exe** and wait for the run to finish (about 30–40 minutes).
   Download the **TrialsHD-windows** artifact, then follow steps 3–4 above.

In this public repository the workflow has no game files and just reports that there is nothing
to build.

## What's in here

| Path | Purpose |
|---|---|
| `rexglue/trialshd_manifest.toml` | ReXGlue project definition |
| `rexglue/trialshd_config.toml` | Fixes for this game: function entries the analysis missed, setjmp/longjmp, codegen speed options |
| `rexglue/trialshd.toml` | Runtime settings shipped next to the exe |
| `rexglue/src/` | App code: game-file discovery, full-game licence, crash reports, FPS counter, sampling profiler, timer resolution |
| `.github/workflows/build.yml` | Windows build: SDK download, `rexglue codegen`, compile, package |
| `tools/` | STFS extractor, XEX unpacker, and the scanners used to find function entries |

## Notes for developers

Things this game needed beyond ReXGlue's automatic analysis:

- **setjmp/longjmp** (MSVC CRT, used by libpng) must be redirected to native implementations,
  otherwise `longjmp` returns into the wrong frame.
- **Function pointers built in code** (`lis`/`addi` pairs) and stored in data tables point at
  functions the analysis never sees; `tools/find_data_code_ptrs.py` and
  `tools/find_code_built_ptrs.py` find them for the config.
- **`non_argument_as_local` breaks this game:** the stack probe and exception funclets receive
  values in `r12`. The other `*_as_local` options are safe and are what made it full speed.
- **Windows timer resolution:** without `timeBeginPeriod(1)` the game's short sleeps round up to
  15.6 ms and it runs at ~30 FPS.
- **Host occlusion queries** stall the CPU on every query; they're disabled in `trialshd.toml`.

## Credits and licence

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (BSD-3-Clause), built on
  [Xenia](https://github.com/xenia-project/xenia) (BSD-3-Clause). See `NOTICE.md`.
- Trials HD is © RedLynx / Ubisoft, published on Xbox 360 by Microsoft. This project is not
  affiliated with or endorsed by them.
- The code in this repository is released under the MIT licence (`LICENSE`).
