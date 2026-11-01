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
2. Get your game files (`default.xex` and the `data` folder) from your own copy of Trials HD.
   See [Getting your game files](#getting-your-game-files) below.
3. Copy `default.xex` and `data` into the `assets` folder next to `trialshd.exe`.
4. Run `trialshd.exe`.

Requirements: 64-bit Windows 10/11, a DirectX 12 graphics card, a CPU with SSE4.2 (2009 or later),
and the [Microsoft Visual C++ Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe)
(install it if Windows says `MSVCP140.dll` or `VCRUNTIME140.dll` is missing).

In game: **F3** shows the FPS overlay, **F4** opens settings. Options live in `trialshd.toml`.
If it crashes, `crash.txt` and the newest file in `logs` show what happened; include them in an issue.

Saves and the shader cache are stored in your Windows user folder, not next to the exe.

## Getting your game files

This project doesn't include or link to the game. You need **your own copy of Trials HD**, the
Xbox Live Arcade game bought on an Xbox 360 (title ID `5841095A`). The port always runs as the
full game, so please only use it with a copy you've paid for, not the free trial.

### 1. Copy the game package off your Xbox 360

Xbox Live Arcade games are stored as one package file (an STFS "LIVE" file) on the console.

1. On the Xbox 360, go to **Settings → System → Storage**, select a USB drive and choose
   **Configure Now** to set it up as an Xbox storage device. Then copy **Trials HD** to it under
   **Games and Apps**.
2. Plug the USB drive into your PC. Windows can't read the Xbox format directly, so open it with an
   Xbox 360 storage tool such as **Velocity** or **Horizon**.
3. Browse to `Content/0000000000000000/5841095A/000D0000/` and copy out the file inside. It has a
   long hexadecimal name and no extension; that's the game package.

If your console has a hard drive that you can connect to the PC, the same tools can read it and the
package is in the same folder.

### 2. Extract the files

**Easiest:** open the package file in **Velocity** or **Horizon** (the same tools as above), which
show its contents, and extract everything to a folder.

Disc-image tools such as extract-xiso won't work: Trials HD is a download, stored as an STFS
package rather than a disc image (ISO).

**Or with the included script:** you need [Python 3](https://www.python.org/downloads/) (no extra
packages). Download this repository (**Code → Download ZIP**), open a terminal in its folder and run:

```
python tools/stfs_extract.py "<path to the package file>" extracted
```

This checks every block of the package and writes the contents to the `extracted` folder. Either
way you end up with:

```
extracted/
  default.xex      <- the game executable
  data/            <- all the game data
  ArcadeInfo.xml   (not needed)
```

Copy `default.xex` and `data` into the `assets` folder next to `trialshd.exe`.

## Building it yourself (no local tools needed)

GitHub Actions can build the exe for you in a **private** copy of this repository (the build needs
your game's `default.xex`).

1. Create a **private** repository from this one: use **Use this template → Create a new
   repository** and set it to *Private* (a fork of a public repository is always public, so don't fork).
2. Get `default.xex` as described in [Getting your game files](#getting-your-game-files).
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
