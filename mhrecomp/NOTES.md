# Mat Hoffman's Pro BMX 2 (PS2, SLUS-20335) static recomp — working notes

Layout (all inside `rea\Mat Hoffman's Pro BMX 2 (USA)\`): the `.iso`, `disc\`
(extracted with `tools/isoextract.py`), `mhrecomp\` (this project), `shots\`
(test screenshots). Runtime and tools were copied from the Dave Mirra 2 project
(`rea\Dave Mirra PS2\bmx2recomp`, skill `ps2-static-recomp`) on 2026-11-22 and
adapted. Git repository: the game folder (remote ADHD-TISM/Mat-Hoffman-2-Recomp).

## Build / run
    sh regen.sh        # EE ELF -> gen/*.c (passes --extra extra_ee.txt --code 42bf00-432980)
    sh build.sh        # -> build/mh2.exe
    build/mh2.exe --turbo --no-window --frames 900 ../disc
    python -I tools/recomp_iop.py ../disc/PS2SOUND.IRX ps2sound 80000 gen_iop
`SECS=500 sh tools/iterate.sh 40 --turbo --no-window --frames N` adds missing
EE entries. Screenshots: `GS_SHOT=<absolute path, forward slashes>`
`GS_SHOT_EVERY=N` then `SHEET_DIV=2 python -I tools/sheet.py out.png COLS *.ppm`
(the game changes the working directory, so a relative GS_SHOT fails).

## State (2026-11-24)
Scripted runs: boot movies, title, menus, rider select, the road trip
videos. Oklahoma City in Road trip: a full 2:00 run, results, "back to bus"
and the next run. Session, Free ride and 2-player Horse load and ride
Oklahoma City. Chicago (Road trip, reached with MH2_UNLOCK) loads and rides.
Saves: road trip and results-screen saves, boot autoload restores the career
(see Saves). Sound plays (checked by RMS on SPU_WAV, not by ear). Real time:
Oklahoma City 53 slow frames in 7200; Chicago 45 in 12540 with 10 audio
underruns after the speed work. 41 VU1 programs recompiled, VU_CHECK clean.
Hand play by the user started 2026-11-23 (found 0x119a40 and 0x26eea0, both
fixed). Not yet tried: the other 6 cities, secret levels, park editor, most
of multiplayer. Widescreen has no 3D hook in this game (the option gives no
wider view).

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
the disc image goes in `Mat Hoffman's Pro BMX 2/` (the old name `Mat Hoffman 2/`
still works) or is chosen on the Build tab.
`sh launcher/build_exe.sh` makes `Mat Hoffman's Pro BMX 2 Launcher.exe` (PyInstaller).
After recording new VU1 programs: `python -I tools/mkvurecipe.py gen_vu
vu_recipe.json ../disc/SLUS_203.35` (all programs so far are inside the ELF).

## Speed (2026-11-23, third session)
Real time in Chicago, no window: 2389 frames over 17 ms and 347 audio
underruns -> 45 and 10. Three changes:
- Early FINISH (gsthread.c, hw.c): the game ends each frame with a GIF path 3
  packet writing FINISH and spins on CSR. That spin used to drain both
  graphics stages (gst_sync): the EE waited 55 s of ~100 s. The EE now counts
  the FINISH writes it queues on path 3 and answers the spin at once; the GS
  stage swallows its own post for those. GS_EARLY_FINISH=0 turns it off. The
  EE may now run up to the frames-ahead limit (2) ahead of the screen.
  It answers only a spin that follows a CSR FINISH clear (hw.c g_fin_armed),
  so a spin on another CSR bit cannot use up a queued FINISH. Repeatable
  runs: if the GS stage already reached the FINISH before the 4th read, the
  normal post is seen earlier; runs matched on this machine, but a much
  faster host could still drift.
- Texture cache (gs_gl.c): the game uploads its textures again every frame
  with the same bytes. A hash of the source blocks skips the decode and the
  GL upload when unchanged (214k decodes -> 11k on the Chicago route).
- Row functions gs_read_row / gs_write_row (gs_mem.c) for T4/T8/CT16:
  uploads, decodes, local->local and target uploads no longer call the
  per-pixel functions (output checked identical, 16 frames).
- hw.c: the CSR spin rule no longer depends on whether the graphics thread is
  idle (it made scripted runs drift with host speed); two runs now match.

## Level crawl (2026-11-23)
`MH2_UNLOCK=1` (testing only; hook on fn 125040 in main.c) zeroes the fame
each road trip city needs. Route to Chicago: Start at 1700/1900/2100, X at
2600/2800 (road trip, rider), `right` at 3100, X every 200 frames from 3500,
`up` from 9100. Other cities stay locked on a fresh save (they need the
previous city's progress); free ride levels too. Found and fixed:
- recomp_ee.py: C++ thunks reached only through vtables (`j fn; nop`
  padded) and an empty `jr ra` right after a tail jump are entries now (42
  were missing; Session/Free ride crashed at 0x119a40, Chicago at 0x3232a0).
- 19 more VU1 programs (41 in gen_vu).
- Known, harmless so far: "vif1: unknown command ad6bad6b" once, at X on
  the results screen (also before these changes); libmtap version warning.
Developer build with an own clang: set MH2_CLANG. Update tab downloads need a
GitHub release of ADHD-TISM/Mat-Hoffman-2-Recomp (none yet).

## Saves (2026-11-23)
Tested on an empty card in work/mctest (run dir with SLUS_203.35, --image):
- Boot autoload: "Loading data...", reads BASLUS-20335BMX (3412 + 4-byte
  checksum) and C0 (8088 + 4), probes C1-C8. Works.
- Road trip start creates both saves; the results screen saves with circle
  (then X = yes on "overwrite the previously saved data").
- Round trip: run 1 set two career words (C0 +0x658 = 13, +0x12f8 = 11).
  After a boot with that card the game's career block at EE 0x8dd914 holds
  13 and 11 (0 with an empty card), and a run without riding saved them
  back unchanged.
- mc.c GetDir: flags != 0 ("next") now continues the previous listing; it
  used to restart. The game only uses single calls so far.
Fresh-card route: Start 3600/3900, X every 250 from 4300 to 11800 (the ride
starts on an X at the challenge screen, ~frame 17300), then up held with X
and circle taps; results ~25000.

## Debug aids added
- `ee_backtrace()` (cdvd.c): return addresses found on the guest stack.
- `recomp_ee.py --bbtrace ADDR`: logs every block of one function
  (`ee_bb` in main.c, `BB_FROM` / `BB_TO` frame range).
- HOOKS in recomp_ee.py: `ee_hook` logs a0-a3 and ra at function entry.

## Next steps
1. Hand play test by the user (keyboard: arrows, Enter = Start, K/Space = X).
2. Reach the other 6 road trip cities and the 4 secret levels (Austin,
   Daytona, SF, halfpipe, Hawaii): needs their unlock flags found (only
   Chicago opens with MH2_UNLOCK). Saves: done, see Saves.
3. Codex review findings still open (see memory use-codex-for-reviews): GS
   register reads above 0x12001000 alias the display registers; MSKPATH3 is
   not modelled (not needed so far: the game syncs textures with VIF1
   interrupts); cdvd.c image-type detection by size.
4. Leftovers from the old game: skip_intro movie names in pad.c, menu.c texts,
   widescreen hooks.
5. Done 2026-11-23: runtime/vu_core.h holds the VU types, register files,
   inline helpers and VU_PAIR macros; recomp_vu.py writes one
   gen_vu/vu1_rec_<hash>.c per program (vu1_rec.h keeps the table). A full
   launcher build (translate + compile) is 1 min 17 s, was ~9 min.
6. Known game names: 1dd4b0 fopen, 1dd6a0 fread, 1dd380 file table entry,
   1dd540 CD read, 36b940 snprintf, 36a950 printf core, 3a0d00 strlen,
   22ff60 end of frame / swap, 230870 display registers, 20f370 pad poll.
