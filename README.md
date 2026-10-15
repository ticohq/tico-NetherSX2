<picture>  
<source media="(prefers-color-scheme: dark)" srcset="https://i.imgur.com/8qsV6MH.png">  
<source media="(prefers-color-scheme: light)" srcset="https://i.imgur.com/4cpzGnB.png">  
<img src="https://i.imgur.com/8qsV6MH.png" width="200">  
</picture>  

*Part of the Tico ecosystem* — https://www.ticoverse.com

**NetherSX2** is a PlayStation 2 emulator for Android, from the AetherSX2 and PCSX2 lineage, known for running PS2 games well on ARM hardware.

This port is exclusive to Tico. It loads the original Android ARM64 NetherSX2 core, patches it, and runs it natively on the Nintendo Switch inside a minimal Android-like environment, adapted to work with the Tico frontend and runtime.

----------

## Summary

This port focuses on making NetherSX2 fit naturally within Tico, rather than behaving as a separate application.

It adds:

-   Tico's in-game menu (Plus + Minus), with the game paused underneath
-   Save and load states with a picture of each slot, plus an auto save to continue where you left off
-   Every NetherSX2 option in Tico's settings, with per-game settings editable in game or from Tico
-   Vulkan and OpenGL (native and Zink) rendering in one module, chosen in the settings
-   Both NetherSX2 cores, Patched (4248) and Classic (3668)
-   Memory cards and states kept per Tico account
-   PNACH cheats from Tico's cheat folder, LSFG frame generation and a Boost mode for the CPU and GPU clocks
-   A Redump game list, and games on SD or USB drives in ISO, CHD, CSO, ZSO, GZ, BIN, MDF, NRG or IMG

----------

## Installation

1. Extract `tico-nethersx2-module.zip` into `sdmc:/tico/modules/` and restart Tico.
2. Put your own PS2 BIOS dump in `sdmc:/tico/system/nethersx2/`.
3. Put your games in `sdmc:/tico/roms/ps2/`.

Cheats go in `sdmc:/tico/cheats/ps2/`, named like the game's file with a `.pnach` extension. Frame generation uses the `Lossless.dll` shared by Tico's emulators, in `sdmc:/tico/system/lsfg/`.

The emulator needs the full memory of a game: start Tico through an installed title (hold **R**) or a forwarder, not from the album.

----------

## Building

`tools/build_tico.sh` builds `tico-nethersx2.nro` and the module zip inside the `ghcr.io/autorunhq/switch-dev` image (see `.github/workflows/build.yml`). The NetherSX2 cores and their data are taken from the release of [NaGaa95/NetherSX2_nx](https://github.com/NaGaa95/NetherSX2_nx); none of them are stored in this repository. `NOTES.md` describes how the port works.

----------

## Credits

This port is built on top of the NetherSX2 emulator and the work of many others.

- **AetherSX2 and PCSX2 developers** — for the emulator
- **NetherSX2 maintainers** — for the patched Android builds
- **NaGaa95** — for the original NetherSX2 Switch port this is based on
- **fgsfds** — for the Switch so-loader groundwork
- **TheOfficialFloW** — for the original Android so-loader lineage
- **Dantiicu** — for the Switch Vulkan driver
- **PancakeTAS** — for LSFG-VK
- **Slluxx** — for IconGrabber

----------

## Legal

This project has no affiliation with the AetherSX2, NetherSX2 or PCSX2 developers. No BIOS, game image, `Lossless.dll` or emulator core is distributed in this repository; you must supply your own BIOS dump and legally obtained game images. We do not condone piracy.

Unless noted otherwise, the wrapper source is under the MIT License (see `LICENSE`). The in-game menu under `source/tico/overlay` is GPL-2.0-or-later, and the vendored LSFG-VK subset under `third_party/lsfg-vk` is GPL-3.0-or-later.

----------

## A Note

A lot of work in this scene disappears over time — not because it lacked value, but because it was never shared.

If you are building something, consider releasing it. Even small contributions can help others move forward.

----------

## Support

If you enjoy NaGaa95's work on the original port and want to support them:

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)
