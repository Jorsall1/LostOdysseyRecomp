<div align="center">

<img src="assets/lost-odyssey-recomp.png" alt="Lost Odyssey Recomp logo" width="112">

# Lost Odyssey Recomp for Nintendo Switch 2

**Lost Odyssey (Xbox 360), recompiled to run natively on the Nintendo Switch 2.**

[![Release](https://img.shields.io/github/v/release/Jorsall1/LostOdysseyRecomp?include_prereleases&label=release)](https://github.com/Jorsall1/LostOdysseyRecomp/releases)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue)](LICENSE)
![Nintendo Switch 2](https://img.shields.io/badge/Nintendo%20Switch%202-homebrew-E60012)
![Vulkan](https://img.shields.io/badge/graphics-Vulkan%20(OunceVK)-A41E22)

### [Download](https://github.com/Jorsall1/LostOdysseyRecomp/releases) · [Report a problem](https://github.com/Jorsall1/LostOdysseyRecomp/issues)

Switch 2 port by **[Jorsall1](https://github.com/Jorsall1)** and **Claude** ([Anthropic](https://www.anthropic.com)), built on the [Switch port](https://github.com/Ad0ky/LostOdysseyRecomp) by **Ad0ky** and **depende3000**, which is based on [freefrank's Lost Odyssey Recomp](https://github.com/freefrank/LostOdysseyRecomp).

Free and open source: if you paid for this, ask for a refund.

</div>

## What this is

A homebrew build of Lost Odyssey Recomp for the Nintendo Switch 2, running under [kaiHEN](https://github.com/ouncebrew/kaihen). The game's Xbox 360 code is statically recompiled to native ARM64. Its graphics run on Vulkan through [OunceVK](https://github.com/ouncebrew/ouncevk), the Switch 2's Mesa NVK driver, and the port is built with [libounce](https://github.com/ouncebrew/libounce) from the [ounce devkit](https://github.com/ouncebrew/ounce-devkit).

You need your own Lost Odyssey discs. This project contains no game files.

The game plays through fields, towns, battles and menus, and Settings changes apply while playing. Report crashes, missing effects or anything that looks or sounds different from the Xbox 360 ([how](#reporting-a-problem)).

The original Switch (Switch 1) version, with its deko3d back end, lives in [Ad0ky/LostOdysseyRecomp](https://github.com/Ad0ky/LostOdysseyRecomp). Use that one on a Switch 1 with Atmosphère.

## Performance

The Switch 2 version targets **60 FPS** (the Switch 1 version runs at 30). Dynamic resolution keeps the frame rate: it lowers the 3D scene's height when the GPU runs out of time and raises it again when there's room. FSR 1 scales the scene to the screen.

Measured in handheld mode (GPU 561 MHz, CPU 1101 MHz) with the default settings:

| Scene | Frame rate | Scene resolution |
| :--- | ---: | ---: |
| Fields, inns and lighter areas | **60 FPS** | up to 1920×1080 |
| Uhra's Main Street (busiest town) | **52–59 FPS** | 576 rows (the minimum) |
| Fixed 1080p, dynamic resolution off | 51–57 FPS | 1920×1080 |

Docked, dynamic resolution runs from 720p up to 1440p. Docked performance hasn't been benchmarked yet.

## Getting started

You need:

- a **Nintendo Switch 2** running **[kaiHEN](https://github.com/ouncebrew/kaihen)** with the Homebrew Menu;
- a **microSD card** with about **25 GB** free (the game data alone is about 21 GB);
- your **Lost Odyssey discs** (Asian multilingual or USA/Europe edition, as ISOs, Games on Demand data or folders; don't mix editions);
- a **Windows or Linux PC**, to extract the discs once with the [PC version](https://github.com/freefrank/LostOdysseyRecomp/releases/latest);
- the **[Switch 2 release ZIP](https://github.com/Jorsall1/LostOdysseyRecomp/releases/latest)** (the game) and the **shader files** from the [Switch release](https://github.com/Ad0ky/LostOdysseyRecomp/releases/latest) (see step 3).

### 1. Extract your discs on a PC

The console can't import discs itself. The PC version's importer does it once; the [installation guide's Step 1](docs/INSTALL-SWITCH.md#step-1-extract-your-discs-on-a-pc) has the details and the supported disc IDs.

1. Download the [PC version](https://github.com/freefrank/LostOdysseyRecomp/releases/latest) and start it. The importer opens when no game is found.
2. Choose **Files** or **Folder**, select your disc images or dumps, check what it found, and confirm. Import all four discs; only disc 1 is needed to start.
3. When it finishes, the game is in a `game` folder with `disc1`–`disc4`. Each disc folder holds `default.xex`, `LO.fpi` and the `.fpd` archives.

You don't need to play the PC version.

### 2. Copy the game to the SD card

1. Extract the Switch 2 release ZIP to the **root** of the SD card. It creates `switch/LostOdysseyRecomp/` with `LostOdysseyRecomp.nro` and `env.txt`.
2. Copy the whole `disc1`…`disc4` folders into `sdmc:/switch/LostOdysseyRecomp/game/`.

### 3. Add the shader files

From the [Switch release ZIP](https://github.com/Ad0ky/LostOdysseyRecomp/releases/latest) (Ad0ky/LostOdysseyRecomp), copy only `shaders/portable_vk.lospv` and `shaders/pipelines_corpus.bin` into `sdmc:/switch/LostOdysseyRecomp/shaders/`. Don't copy that release's `.nro`, `env.txt` or `cache/` folder: they are for the Switch 1 build.

### SD card layout

```
sdmc:/
└── switch/
    └── LostOdysseyRecomp/
        ├── LostOdysseyRecomp.nro        the game (Switch 2 release ZIP)
        ├── env.txt                      extra settings (Switch 2 release ZIP)
        ├── game/                        your discs (PC importer)
        │   ├── disc1/                   default.xex, LO.fpi, LO.fpd, xenon_*.fpd
        │   ├── disc2/
        │   ├── disc3/
        │   └── disc4/
        ├── shaders/                     from the Switch release ZIP
        │   ├── portable_vk.lospv
        │   └── pipelines_corpus.bin
        │
        │   created by the game on first start:
        ├── config/settings.ini          Settings menu choices
        ├── save/                        your saves
        ├── cache/shaders/               shader and GPU driver caches (pipeline_cache_vk.bin)
        ├── mods/
        ├── state/logs/                  runtime-*.log
        ├── stderr.log
        ├── early_boot.log
        └── exit.log
```

The PC importer stores files that are the same on several discs only once. On the exFAT card they take their full size again, so the card needs the full ~21 GB.

### 4. Start the game

Start the Homebrew Menu **through title takeover**: hold **R** while starting any installed game, then choose **Lost Odyssey Recomp**.

On the first launch the GPU driver compiles the game's pipelines in the background. Expect some stutter for the first few minutes. Later launches load them from the driver cache.

To quit, press **HOME** and close the game with **X**. kaiHEN's R hook keeps working afterwards.

## Settings

On the title screen, **Y** opens **Settings**. In-game, Settings is in the game's own menu. Settings are saved in `config/settings.ini`.

| Option | What it does |
| :--- | :--- |
| **Render resolution** | The height the 3D scene is rendered at before it's scaled to the screen, set separately for handheld and docked. Automatic is 1080p handheld and 1440p docked. With dynamic resolution on, this row is called **Maximum resolution**. |
| **Dynamic resolution** | On by default. Lowers the scene's resolution step by step when the GPU can't finish a frame in time, and raises it again when there's room. It holds still on light screens (menus) and during loads. |
| **Minimum resolution** | How low dynamic resolution may go. Automatic is 576p handheld and 720p docked. |
| **FSR** | AMD FSR 1: scales the scene to the screen with sharper edges than plain scaling. On by default. |
| **FSR sharpness** | How strongly FSR sharpens. |
| **Dynamic shadows, depth of field, bloom, motion blur** | The game's effects. When dynamic resolution is already at its minimum and the GPU still can't keep up, motion blur, depth-of-field blur and shadows are reduced, then brought back once there's room. |
| **Performance overlay** | Off by default. **FPS** shows the frame rate; **Detailed** adds the render resolution, FSR, and the CPU and GPU load. |

**Recommended:** keep dynamic resolution and FSR on. Fixed 1080p without FSR drops below 60 FPS in heavier areas.

### env.txt

`env.txt`, next to the game, holds settings that aren't in the menu, one `NAME=value` per line. For normal play, use only:

```
LO_GUEST_AFFINITY=1
```

It places the game's threads on the cores the Xbox 360 version asks for. Every speed option is already on by default, and the debug options cost speed, so leave the rest out.

## Reporting a problem

[Open an issue](https://github.com/Jorsall1/LostOdysseyRecomp/issues) with:

- the release version;
- your game edition;
- handheld or docked;
- your `env.txt`;
- what was on screen when it happened.

Also attach the files the game writes in `sdmc:/switch/LostOdysseyRecomp/`: `state/logs/runtime-*.log`, `stderr.log`, `early_boot.log` and, if they exist, `exit.log`, `crash.log` and `hang.log`.

Don't attach the game's disc files (`game/`).

## For developers

Building needs devkitPro with the [ounce devkit](https://github.com/ouncebrew/ounce-devkit) (libounce, OunceVK) installed. Generate the recompiled game code first, as described in [docs/SWITCH.md](docs/SWITCH.md) (the generated `LostOdysseyRecompLib/ppc` sources are not in the repository). Then:

```sh
export DEVKITPRO=/opt/devkitpro
. /opt/devkitpro/switch2vars.sh
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/libounce/cmake/Switch2.cmake
cmake --build build -j
```

The NRO is `build/LostOdysseyRecomp/LostOdysseyRecomp.nro`. Switch 2 code is under `LO_TARGET_SWITCH2` / `__SWITCH2__`; the plume changes are kept as patches in `tools/patches/`.

Switch 2 specifics worth knowing:

- **Memory:** kaiHEN masks the memory-mapping syscalls, so guest memory is one heap block (`Backend::Heap`), not mirrored views.
- **kaiHEN's R hook:** never send `SetCurrentProcess` (or any identity request) on a session from `smGetService` on Switch 2. Those sessions are libounce overrides shared with kaiHEN, and doing so breaks the R → Homebrew Menu hook until a reboot. The SD card is opened with `fsOpenSdCardFileSystem` instead.
- **Clears:** NVK writes every rectangle of one `vkCmdClearAttachments` into a single 64 KiB command block. More than 2730 rectangles overrun it and stop the GPU, so the renderer merges clear rectangles and issues at most 80 per call.
- **Diagnostics:** on a GPU stop the runtime log lists the last GPU batches and their operations, and `stderr.log` gets the GPU channel's error type (`[gpu channel]`).

This repository also carries the rest of upstream's source, so the PC, macOS and Android versions still build from it. For those, use [freefrank/LostOdysseyRecomp](https://github.com/freefrank/LostOdysseyRecomp). For the Switch 1 version, use [Ad0ky/LostOdysseyRecomp](https://github.com/Ad0ky/LostOdysseyRecomp).

## Credits

- **Switch 2 port:** [Jorsall1](https://github.com/Jorsall1) and Claude ([Anthropic](https://www.anthropic.com)).
- **Switch port**, which this port is built on: [Ad0ky](https://github.com/Ad0ky) and [depende3000](https://github.com/depende3000), in [Ad0ky/LostOdysseyRecomp](https://github.com/Ad0ky/LostOdysseyRecomp).
- **Lost Odyssey Recomp**, the original recompilation: [freefrank](https://github.com/freefrank), in [freefrank/LostOdysseyRecomp](https://github.com/freefrank/LostOdysseyRecomp), and its contributors: [MikeRavenelle](https://github.com/MikeRavenelle), [dj5927](https://github.com/dj5927), [Xarishark](https://github.com/Xarishark), [navjack](https://github.com/navjack), [frankzzz](https://github.com/frankzzz), [cngjd](https://github.com/cngjd) and [faisalkindi](https://github.com/faisalkindi).
- **Switch 2 homebrew:** [ouncebrew](https://github.com/ouncebrew) for [kaiHEN](https://github.com/ouncebrew/kaihen), [libounce](https://github.com/ouncebrew/libounce), [OunceVK](https://github.com/ouncebrew/ouncevk) and the [ounce devkit](https://github.com/ouncebrew/ounce-devkit).
- **Research and tools:** [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp), [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), [XenosRecomp](https://github.com/hedge-dev/XenosRecomp), [re:Blue](https://github.com/zolaware/reblue), [plume](https://github.com/renderbag/plume), [Xenia](https://github.com/xenia-project/xenia), [devkitPro](https://devkitpro.org) and [Mesa](https://mesa3d.org) (NVK).
- **Upscaling:** thanks to AMD for [FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) (MIT).
- **Audio** uses the [Xenia FFmpeg fork](https://github.com/xenia-project/FFmpeg) ([license](thirdparty/ffmpeg-LICENSE.txt)).

Lost Odyssey and its assets belong to their respective owners. This is an unofficial, non-commercial project under the GPL-3.0; dependencies keep their own licenses. Use data from discs you own, and don't commit game executables, archives, textures, audio, video, generated game code or captures to this repository.
