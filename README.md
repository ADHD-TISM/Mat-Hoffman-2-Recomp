# Mat Hoffman's Pro BMX 2 — PC recompilation

> **Early work in progress.** Expect bugs, crashes and missing effects.

A native Windows build of **Mat Hoffman's Pro BMX 2** (PlayStation 2, USA,
SLUS-20335). The game's own program is translated to C and compiled for the
PC. It is not an emulator: the game code runs natively, and a small runtime
stands in for the PS2 hardware. Graphics run on your GPU through OpenGL.

**No game data is included.** You need your own copy of the game. The launcher
reads the code from your disc image and builds the game on your PC.

## What works

Tested so far, mostly with scripted input:

- The intro movies, menus and the road trip videos.
- Oklahoma City in Road trip, Session, Free ride and 2-player Horse, and
  Chicago in Road trip: a full 2:00 run, the results screen and the next run.
- Saving and loading on a memory card (road trip and results-screen saves,
  automatic load at start).
- Music, sound effects and movie sound.
- Keyboard and Xbox-style gamepads.
- 60 fps in real time in the levels tried.

Not tested yet: the other road trip cities, the secret levels, the park
editor and most of multiplayer. Widescreen does not work in this game yet.
If the game reaches code that was not translated, it stops with a message
box (see Troubleshooting).

## Requirements

- Windows 10 or 11, 64-bit.
- A GPU with OpenGL 3.3.
- About 1 GB of free disk space.
- Your own disc image of **Mat Hoffman's Pro BMX 2, USA (SLUS-20335),
  version 1.00**: an `.iso`, or a `.bin` + `.cue`. Other regions do not work.

You do not need to install a compiler or Python. The launcher downloads its own
build tools into its folder and writes nothing outside it.

## Install and play

There is no release download yet. Until there is, run the launcher from a
copy of this repository: it then uses the `mhrecomp` folder next to it.

1. Clone or download this repository.
2. Build the launcher: `sh launcher/build_exe.sh` (needs Python 3 with
   PyInstaller). It writes `Mat Hoffman 2 Launcher.exe` to the repository
   folder. You can also run `python launcher/mh2launcher.py` directly.
3. Start the launcher once. It creates a folder called `Mat Hoffman 2` next to
   it. Copy your disc image into that folder.
4. Open the **Update** tab and click **Download**. This fetches the build
   tools (about 200 MB).
5. Open the **Build** tab and click **Build the game**. The first build takes
   about a minute and a half.
6. Open the **Play** tab and click **Play**.

After you pull new code, build again. Builds keep your settings and memory
card saves.

## Controls

The keyboard defaults are below. Change them in the launcher's **Settings** tab.

| PS2 | Keyboard |
|---|---|
| D-pad | Arrow keys |
| Left stick | W A S D |
| Right stick | Numpad 8 4 2 6 |
| Cross | K or Space |
| Circle | L |
| Square | J |
| Triangle | I |
| L1 / R1 | Q / E |
| L2 / R2 | 1 / 3 |
| L3 / R3 | Z / C |
| Start | Enter |
| Select | Backspace or Right Shift |

An Xbox-style gamepad works as the PS2 pad (A = Cross, B = Circle,
X = Square, Y = Triangle). A second gamepad is player 2. **F11** or
**Alt+Enter** toggles fullscreen.

In the game: press Start once to skip the intro movie, and Start again at the
title. On the results screen, Circle saves and Cross continues.

## Settings

The **Settings** tab edits `game\mh2.ini`: internal resolution, window size,
fullscreen, picture shape, picture filter, volume, vsync, gamepad vibration
and the key bindings. Delete `mh2.ini` to get the defaults back.

## Saves

The memory card is the folder `game\memcard\mc0`. Back it up to keep your saves.

## Troubleshooting

- **"is not a disc image" or "does not match":** check that the image is the
  USA version 1.00 (SLUS-20335) and is not compressed.
- **A message box says the game called code that was not recompiled:** please
  open an issue with the address from the message and what you were doing.
  The address is also written to `game\missing_entries.txt`.
- **The game closes without a message:** the launcher's log says why, and
  `game\mh2.log` holds the game's own messages. Attach its last lines to an
  issue.

## Building from source (developers)

The launcher does all of this for you. To work on the code directly:

- `mhrecomp/tools/recomp_ee.py`, `recomp_iop.py` and `recomp_vu.py` translate
  the game's main CPU, the sound driver (`PS2SOUND.IRX`) and the VU1 programs
  to C. `vu_recipe.json` says where each VU1 program is in the game's files,
  so they are rebuilt from your disc.
- `mhrecomp/runtime/` is the hardware runtime: GS on OpenGL (on worker
  threads), VIF/VU, IPU movies, SPU2 sound, CD streaming, pad, memory card,
  kernel.
- `mhrecomp/regen.sh` and `mhrecomp/build.sh` regenerate the code and build
  `mh2.exe` with clang. They expect the disc's files extracted to `disc\`
  next to `mhrecomp\` (`tools/isoextract.py`).
- `mhrecomp/NOTES.md` holds the research notes, test routes and current state.
- The launcher's source is `launcher/mh2launcher.py`
  (`python launcher/mh2launcher.py --help`). `launcher/build_exe.sh` builds
  `Mat Hoffman 2 Launcher.exe` with PyInstaller.

Never commit game data: no disc images, extracted files or generated code.
The `.gitignore` only lets source files in.

## License

No license has been chosen for this code yet. Nothing here grants any rights
to Mat Hoffman's Pro BMX 2 or its data, which belong to their owners. This
project is not affiliated with or endorsed by them.
