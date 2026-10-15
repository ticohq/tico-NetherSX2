# tico-nethersx2: working notes

Goal: NetherSX2 for the Switch as a **tico module** (tico 0.8.0+), integrated
the way the official tico cores are: tico starts it, the game runs with tico's
in-game menu, and it returns to tico with `--resume`.

## Lineage

- Base: [NaGaa95/NetherSX2_nx](https://github.com/NaGaa95/NetherSX2_nx) (MIT),
  remote `upstream`. Its history is kept so upstream releases can be merged.
  It loads NetherSX2's Android `libemucore.so` (builds 4248 and 3668) in an
  Android-like environment, with GL (NVC0/Zink) and Vulkan (NVK) renderers.
- tico layer: [ticohq/tico-drasticDS](https://github.com/ticohq/tico-drasticDS),
  the same so-loader lineage by the same author, already turned into a tico
  module. Its `source/tico/` (entry, overlay, tico config, lang) is the model;
  overlay files are GPL-2.0-or-later (Azahar / ticoverse.com).
- Module format: <https://forum.ticoverse.com/topic/149/creating-tico-modules>.
  In-game menu behaviour: <https://forum.ticoverse.com/topic/186/the-in-game-menu>.

Read-only reference clones live in `_refs/` (git-ignored).

## Module

`tico/module/` is the module source; the build packs it as
`tico-nethersx2-module.zip` holding `nethersx2/` (extracts into
`sdmc:/tico/modules/`):

```
nethersx2/module.json
nethersx2/tico-nethersx2.nro
nethersx2/settings.json      settings screen, stored in tico/config/cores/nethersx2.jsonc
nethersx2/lang/<lang>.json   strings for settings.json
nethersx2/gamelists/ps2.dat.gz   Redump PS2 DAT
```

- System slug `ps2`, `naming: redump`, `disc_based`.
- BIOS: `sdmc:/tico/system/nethersx2/`, any 4 MiB file (`any_of_size`).
- Third-party modules get no TicoSecure session (the build secret is tico's
  only): no playtime, no account, no RetroAchievements token from tico. Saves
  and states still go to per-account folders through tico's paths.

## Architecture (target)

One NRO, `tico-nethersx2.nro`, built with `TICO=1`:

1. **Entry** (`source/tico/tico_entry.c`): argv is `rom title`; `usb://`
   tokens resolved; BIOS staged from tico's system folder; tico's jsonc
   settings mapped onto the host's prefs; exit queues
   `sdmc:/switch/tico/tico.nro --resume`. Started on its own: game list.
2. **Unified renderer**: GL and Vulkan in one binary (Horizon Mesa SDK from
   the `ghcr.io/autorunhq/switch-dev` image), chosen at run time from the
   settings. Upstream picks one at compile time (`GS_RENDERER`, ~45 sites).
3. **Core**: `libemucore.so` (4248 patched / 3668 classic) bundled in romfs at
   build time from the user's NetherSX2 APKs; fallback to the standalone
   NetherSX2_nx install's extracted copy.
4. **Overlay**: tico's in-game menu (Plus + Minus) drawn with ImGui inside the
   EGL/Vulkan present hooks; replaces upstream's OSD-text quick menu
   (L + R + Plus). Save/load state with thumbnails, auto slot with
   Continue Last Game, cheats, settings, per-game settings, reset/restart/exit.

## Phases

1. Repo, CI (switch-dev image), module skeleton, `TICO=1` build target.
2. Entry + paths + `--resume`; settings.json mapped from the launcher's options.
3. Runtime GL/VK selection in one NRO.
4. ImGui overlay ported from tico-drasticDS.
5. States with thumbnails, auto slot, per-game settings, cheats, game list.

## Build

`tools/build_tico.sh` in the switch-dev image (CI: `.github/workflows/build.yml`,
artifacts `tico-nethersx2_sd` = SD layout, `tico-nethersx2_debug` = ELF + map).
The cores and their data come from the upstream release NRO's RomFS
(`tools/extract_nro_romfs.py`, `UPSTREAM_RELEASE`, default 1.3.0), so no APK
is needed. `settings.json` and `lang/en.json` are generated from the upstream
launcher's option tables: rerun `tools/gen_tico_settings.py` after merging
upstream.

## State

- Phases 1 to 3 build in CI: one NRO, renderer from the settings (Vulkan,
  OpenGL NVC0, OpenGL Zink).
- SD layout: `/tico/modules/nethersx2/`; data in `/tico/system/nethersx2/`
  (BIOS at its top level, `resources/`, `cheats/`, `nethersx2.ini`,
  `tico.log`); memory cards and states in the account's folders from the
  launch template; Lossless.dll in `/tico/system/lsfg/`.
- tico's in-game menu (phase 4, first cut): Plus + Minus opens the overlay of
  tico-drasticDS (`source/tico/overlay`, ImGui), drawn before eglSwapBuffers
  (OpenGL, ImGui's GLES3 backend: the core uses VAOs) or in a render pass of
  its own before vkQueuePresentKHR (`source/tico/tico_vk_present.c`). Glue in
  `source/tico/tico_menu.cpp`; upstream's L + R + Plus menu is off in the
  tico build.
- States: overlay slots 1-5 are the core's slots 1-5, the auto slot is the
  core's slot 10 (files `<serial> (<CRC>).NN.p2s` in the account's states
  folder; the picture is `Screenshot.png` inside the zip). The auto save is
  written on Exit, Restart, HOME and closing; the core's own resume state is
  no longer written. `pauseVM`/`saveStateSlot`/`loadStateSlot` queue work on
  the core's VM thread.
- The core's `pauseVM` stops its presents (test 2), which froze the menu.
  The menu holds the game instead: while it is open the presenting thread
  stays in the present, copies the frame as the menu's background and
  presents the menu over it (GL: in eglSwapBuffers; Vulkan: own acquires,
  needs TRANSFER_SRC on the swapchain), which stalls the core; audio plays
  silence (`aaudio_set_muted`). Save/load requests run when it closes.
- Overlay slot N is the core's slot N-1 (NetherSX2 counts from 0); the auto
  slot is the core's resume state, slot -1 (`<serial> (<CRC>).resume.p2s`),
  written by `stopVMThreadLoop(save_resume_state=1)` on Exit, Restart and
  close. The core's save/load JNI calls queue a lambda on the VM thread
  (run in place when the VM loop is not up) that checks the VM state; the
  file name is built by the core's GetStateFilename (no slot offset).
- Vulkan hold: the swapchain gets one extra image (the core may already hold
  an acquired one) and the menu's acquire times out after 500 ms.
- Options with `restart` keep their launch values in the prefs while the game
  runs: the core applied a renderer change live and recreated its device.
- tico 0.8 seals its theme for third parties: the menu uses the module's
  `nethersx2_menu_theme` (default: the console's theme via setsys).
- Cheats: `sdmc:/tico/cheats/ps2/<game file name>.pnach` is copied to
  `<system>/cheats/<CRC>.pnach` with every cheat off at each start.
- Without TicoSecure the menu reads tico's display/general settings from the
  plain files when present (theme, Continue Last Game default "ask"); the
  avatar and nickname come from the account in tico's saves path.

## Hardware test 1 (2026-10-15): passed

Copy `SD_CARD/tico` to the card, a PS2 BIOS into `/tico/system/nethersx2/`,
a game into `/tico/roms/ps2/`; restart tico; start the game with tico in
full-memory mode (title override). Check: the game boots, Settings >
Emulator shows NetherSX2's tabs, L + R + Plus > Exit returns to tico.
Then Settings > Emulator > Video > Renderer: OpenGL (NVC0), and again with
OpenGL (Zink/NVK); each should boot.
Send back `/tico/system/nethersx2/tico.log` and any crash report.

Result: Silent Hill 3 (USA) boots on Vulkan with core 4248; PS2 shows under
Emulators and Consoles; the settings screen shows; Exit returns to tico with
no crash. tico passes the account folders as
`sdmc:/tico/saves/users/<uid>/ps2/` (same for states).
The OpenGL renderers were not reported.

## Hardware test 2 (2026-10-15): the in-game menu

Copy `SD_CARD/tico` over the previous one. With Silent Hill 3 on Vulkan:
1. Plus + Minus opens the menu; the same closes it. Does the game pause?
2. Save State to slot 1, then Load State: the slot shows a picture and time,
   and loading works.
3. Settings: change Resolution scale (applies at once); Renderer says Use
   Restart to apply; Restart relaunches the game.
4. Reset; then Exit Game returns to tico.
5. Start the game again: it offers to continue from the auto save.
6. Press HOME in game, return, then close the game from HOME.
7. Repeat 1-2 with Renderer: OpenGL (NVC0).
Send back `tico.log`, and a photo of the menu if anything looks wrong.

Result: the menu opened in the light theme (tico's is dark); the slot picture
showed; resolution scale and FPS display applied at once; changing the
renderer in game recreated the GS device and left the menu open but
invisible (controls lost); Restart crashed once with Silent Hill 3 but worked
with Shadow of the Colossus, where Continue Last Game was also offered (with
Silent Hill 3 it came during the boot logos). The log showed the core stops
presenting while paused.

## Hardware test 3 (2026-10-15)

1. Plus + Minus: the game freezes under the menu (no sound) and resumes on
   close; the menu follows the console theme (Settings > System > Interface
   overrides it).
2. Save to slot 1, load it; old test states now show one slot higher.
3. Renderer change in game: nothing changes until Restart.
4. Restart a few times with Silent Hill 3; Exit; Continue Last Game.
5. Repeat 1 with OpenGL (NVC0).
Send back `tico.log` (it now keeps every launch) and any
`/atmosphere/crash_reports` file from a crash.

Result: the menu follows the console theme; it holds and releases the game;
launch-only options now ask for Restart; Continue was offered. Exit crashed
at times and Restart sometimes; loading a state seemed to do nothing; the
slot list looked wrong (old states shifted by the new numbering). The log
showed the slot-10 auto save mostly not confirmed within 5 s, and one launch
(Shadow of the Colossus, Vulkan) stopped at the Continue prompt: the hold's
acquire waited forever.

## Hardware test 4 (2026-10-15)

1. Exit and Restart several times (both games, Vulkan and OpenGL).
2. Save to slot 1, then load it from a later point; check the picture.
3. Exit, start again: Continue loads where Exit left off.
Send back `tico.log` (states and shutdown steps are logged) and any
`/atmosphere/crash_reports` file.

Result: states save and load; slot pictures are right (a fresh save takes a
moment to show); Continue works but showed no picture; Exit and Restart
mostly work but now and then drop to the HOME menu (no Atmosphère report).

Why the HOME menu: crash.c ended the process with svcExitProcess on a real
fault, silently. Now `<system>/crash.log` gets the fault (pc, lr, far, esr)
and the frame chain as `host+0x...` (this NRO: addr2line on the CI's
`tico-nethersx2_debug` ELF of that build; kept in `_refs/elf/<run id>/`) or
`core+0x...` (libemucore.so); abort() from the core is logged too and goes
back to tico. Likely cause, fixed: the exit path set the overlay's hook
pointers to NULL while the core's thread could be calling them; the hooks
now stay set and `tico_overlay_stop()` stops the overlay under its lock.
Continue's picture: the resume state has none, so the frame the menu froze
for Exit/Restart is written beside it (`<serial> (<CRC>).resume.picture`).

## Hardware test 5 (2026-10-15)

1. Exit and Restart many times (both games, both renderers).
2. After Exit, Continue shows a picture.
3. On any drop to the HOME menu, send `/tico/system/nethersx2/crash.log`
   with `tico.log`.

Result: no crash in any test. Continue still showed no picture: the prompt
read the slot pictures on the input thread (OfferResume), where textures
cannot be made; it now reads them on its first drawn frame.

## Boost mode

`Wrapper/BoostMode` (System > Performance, off, read at launch):
`source/tico/tico_clocks.c`, a C port of tico-dolphin's Boost mode (CPU
1785 MHz, GPU 768 MHz; clock manager override through hoc:clk / sysclkOC /
sysclk when registered, else clkrst/pcv; re-applied after dock changes and
sleep; a faster clock is left alone; restored on exit). With it on, the
upstream startup FastLoad boost is cancelled (it lowers the GPU).

## ZIP (analysis, 2026-10-15)

The core opens ISO/BIN/MDF/NRG/IMG and the compressed CHD, CSO/ZSO and
indexed GZ itself; it has no ZIP disc reader (its unzip code only reads the
bundled cheat archives). Options:
- Extract to a cache, as tico-drasticDS does for NDS (libarchive): a PS2 ISO
  is 0.7-4.7 GB, so the first launch of a game would take minutes and the
  SD card would hold the image twice.
- Read a stored (uncompressed) ZIP entry in place through the file shims:
  no cost, but such ZIPs are rare and save no space.
- Recommended for users: CHD (random access, smaller than ZIP, native).
Not implemented.

## Hardware test 6 (pending)

1. Exit a game, start it again: Continue shows the picture.
2. System > Performance > Boost mode on, Restart: tico.log says
   `clocks: ...`; check the clocks with a status overlay; Exit puts them back.
