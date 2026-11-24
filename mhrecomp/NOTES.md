# Mat Hoffman's Pro BMX 2 (PS2, SLUS-20335) static recomp — working notes

Layout (all inside `rea\Mat Hoffman's Pro BMX 2 (USA)\`): the `.iso`, `disc\`
(extracted with `tools/isoextract.py`), `mhrecomp\` (this project), `shots\`
(test screenshots). Runtime and tools were copied from the Dave Mirra 2 project
(`rea\Dave Mirra PS2\bmx2recomp`, skill `ps2-static-recomp`) on 2026-11-22 and
adapted. Not a git repo yet.

## Build / run
    sh regen.sh        # EE ELF -> gen/*.c (passes --extra extra_ee.txt --code 42bf00-432980)
    sh build.sh        # -> build/mh2.exe
    build/mh2.exe --turbo --no-window --frames 900 ../disc
    python -I tools/recomp_iop.py ../disc/PS2SOUND.IRX ps2sound 80000 gen_iop
`SECS=500 sh tools/iterate.sh 40 --turbo --no-window --frames N` adds missing
EE entries. Screenshots: `GS_SHOT=<absolute path, forward slashes>`
`GS_SHOT_EVERY=N` then `SHEET_DIV=2 python -I tools/sheet.py out.png COLS *.ppm`
(the game changes the working directory, so a relative GS_SHOT fails).

## State (2026-11-23, second session): playable start to finish
Boot movies (Activision O2, Rainbow, intro), title, main menu, rider select,
memory card saves (road trip and scrapbook: "Save ... was successful", files in
`disc\memcard\mc0\BASLUS-20335BMX*`), the road trip video, loading, and the
first level (Oklahoma City) all work in scripted runs. A full 2:00 run ends in
the results screen; "back to bus" reloads and a second run starts. Sound plays
(movies, title music, 387 key-ons in a run; checked by RMS on SPU_WAV, not by
ear). Real time in the window: 7200 frames, 53 over 17 ms, 0 audio underruns.
VU1 microcode is recompiled (22 programs in gen_vu/, VU_CHECK clean on the
menu route). Not yet tried: other levels and modes, in-game "save" on the
results screen, loading a save, multiplayer, park editor, hand play.

Scripted route (turbo, vblank numbers): `3600:start` skips the intro video,
`3900:start` leaves the title, then X every 250 frames from 4300 walks road
trip -> rider -> save prompts -> video -> Oklahoma City (riding by ~9500).
Holding `up` rides. With the movies skipped earlier: Start at 1700/1900/2100
reaches the main menu by ~2300.

## What differs from Dave Mirra 2 (all fixed in this tree)
- ELF has no section names: text = start of segment to the last `jr ra`
  before the first 64 KB block without one (recomp_ee.py, eedis.py). C++ static
  initialisers live at 0x42bf00-0x432980 (`--code`).
- recomp_ee.py: the zero register is now `0ull` in 64-bit expressions.
  `0 << 56` on an int is undefined, clang dropped the store, and the game's
  snprintf lost every `%s` (file names came out empty).
- recomp_ee.py: C++ thunks (`j fn; nop` padded to 16 bytes, reached only
  through vtables) count as entries when a data word points at them. Without
  this 35 entries were missing; Session and Free ride crashed on load with
  "call to unknown address 00119a40". Road trip, Session, Free ride and
  2-player Horse all reach Oklahoma City with it (2026-11-23).
- Stock Sony libcdvd: `runtime/cdvd.c` is cdvdfsv at RPC level (read, search,
  S commands). The game has its own file table at 0x3f0da0 (982 entries,
  `tools/filetable.py`), so it only uses sceCdRead. N command 9 (streaming,
  probably the PSS movies) is NOT implemented.
- loadfile version must read "2430" or libkernel refuses every module.
- libkernel 2.4 kernel patches: syscalls 0x5a/0x5b/0x83 ignored, 0xfc-0xff
  mapped to the HLE alarm calls (kernel.c).
- hw.c: a 64-bit store to GS CSR spilled into the PMODE slot (display off);
  64-bit CSR reads now apply FINISH from the graphics thread; a CSR.FINISH
  spin waits for the graphics thread (gst_sync) instead of burning emulated time.
- kernel.c: chained interrupts are delivered at once (was 8 per event, the
  rest at the next vblank). The game takes one VIF1 interrupt per texture
  (FLUSHA with the i bit, handler fn 2282b0 uploads the next texture), dozens a
  frame; without this each frame showed a partial scene with wrong textures.
- hw.c: timer overflow / compare interrupts (INTC 9+n). The game counts
  timer 0 overflows (fn 2112f0) for its clock. The frame limiter counts GS
  VSINT interrupts (INTC 0, fn 22f510).
- iop.c: thevent, ioman (disc files), sifcmd 19-21 (GetNextRequest /
  ExecRequest), libsd 21-27, sysmem 6-8, thbase 34; heap 0xb0000-0x1bffff.
  sif.c: IOP heap for the EE with real frees at 0x1c0000.

- cdvd.c: sceCdSt* streaming (N command 9) for the PSS movies and the garage
  TV; without it the title ignored Start (it waits for the stream).
- vu.c: the clipping flag has its 4-instruction latency (clip pipeline in
  vu_t, ticked per pair, carried across recompiled chunks). The game's VU1
  clipper does three CLIPs then three FCGETs to read the three states; with
  instant flags every triangle crossing the screen edge was dropped (grey
  wedges). MAC/status flags still have no latency.
- iop.c: IOP heap with real frees (first fit); PS2SOUND reallocates its
  stream buffers per level.
- VU1 recompile: `VU_RECORD=<abs dir>` run, copy to gen_vu/, add 0580 to every
  .pcs (a JR target the recorder does not see), `python -I tools/recomp_vu.py
  gen_vu/vu1_rec.h gen_vu/vu1_*.bin`, build (vu.c ~3 min). Other levels will
  bring new programs ("not recompiled, interpreting" in the log): record again.

## Launcher (2026-11-23)
`launcher/mh2launcher.py` (copy of the Dave Mirra 2 launcher, game names
changed): reads SLUS_203.35 and PS2SOUND.IRX from the user's image, runs the
recompilers (`--code 42bf00-432980`), rebuilds the VU1 snapshots from
`vu_recipe.json`, compiles into `work/`, puts mh2.exe + mh2.ini in `game/`,
and starts it with `--image <iso> <game folder>`. Root = the repository folder;
the disc image goes in `Mat Hoffman 2/` or is chosen on the Build tab.
`sh launcher/build_exe.sh` makes `Mat Hoffman 2 Launcher.exe` (PyInstaller).
After recording new VU1 programs: `python -I tools/mkvurecipe.py gen_vu
vu_recipe.json ../disc/SLUS_203.35` (all 22 programs are inside the ELF).
Developer build with an own clang: set MH2_CLANG. Update tab downloads need a
GitHub release of ADHD-TISM/Mat-Hoffman-2-Recomp (none yet).

## Debug aids added
- `ee_backtrace()` (cdvd.c): return addresses found on the guest stack.
- `recomp_ee.py --bbtrace ADDR`: logs every block of one function
  (`ee_bb` in main.c, `BB_FROM` / `BB_TO` frame range).
- HOOKS in recomp_ee.py: `ee_hook` logs a0-a3 and ra at function entry.

## Next steps
1. Hand play test by the user (keyboard: arrows, Enter = Start, K/Space = X).
2. Crawl the other levels and modes for missing EE entries and new VU1
   programs; test loading a save and the results-screen save.
3. Codex review findings still open (see memory use-codex-for-reviews): GS
   register reads above 0x12001000 alias the display registers; MSKPATH3 is
   not modelled (not needed so far: the game syncs textures with VIF1
   interrupts); cdvd.c image-type detection by size.
4. Leftovers from the old game: skip_intro movie names in pad.c, menu.c texts,
   widescreen hooks. Two VU1 programs seen in the level are still interpreted
   (f75b7e19549cb6fd, aaba90026769be9d): record and add them.
6. Known game names: 1dd4b0 fopen, 1dd6a0 fread, 1dd380 file table entry,
   1dd540 CD read, 36b940 snprintf, 36a950 printf core, 3a0d00 strlen,
   22ff60 end of frame / swap, 230870 display registers, 20f370 pad poll.
