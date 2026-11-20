# Skate 3 for Nintendo Switch 2 (skate3-nx2)

An unofficial homebrew port of the Skate 3 static recompilation
([skate3recomp](https://github.com/mchughalex/skate3recomp) by mchughalex) to the Nintendo Switch 2,
running as a `.nro` under kaiHEN.

The recompiled Xbox 360 code runs natively on the console's CPU. The Xbox 360 GPU is emulated
on Vulkan (OunceVK) by the rexglue SDK, which this project ports to Horizon.

**This repository contains no game files and no recompiled game code.** You need your own legally
obtained Xbox 360 copy of Skate 3 to build and play it, and the build output must not be shared.

## Status

| | Docked | Handheld |
|---|---|---|
| Menus | 60 fps | 60 fps |
| Gameplay | ~60 fps | ~50 fps in busy areas (uncapped by default; `handheld_fps_cap=40` for a locked 40) |


## What you need

- A Switch 2 with kaiHEN.
- Skate 3 for Xbox 360 (title ID `454108E6`), dumped as an ISO, and its Title Update 3 package
  `TU_12K2276_000000C000000.00000000000O3` (the file skate3recomp's own title update installer
  downloads).
- Linux with git, Python 3, Docker, CMake 3.25+, Ninja and Clang 18+ (I didn't test on Windows)

## Building

### 1. Clone with submodules

```sh
git clone --recursive https://github.com/irolup/skate3-nx2.git
cd skate3-nx2
```

`third_party/skate3recomp` is skate3recomp itself (unmodified) and `third_party/rexglue-sdk` is
the rexglue SDK with Switch 2 support.

### 2. Generate the recompiled code (on the PC)

This uses skate3recomp's own code generation, unchanged:

1. Extract the game from your ISO (for example with `extract-xiso`) into
   `third_party/skate3recomp/game/`, so that `game/default.xex` and
   `game/data/webkit/EAWebkit.xex` exist.
2. Put the Title Update 3 package `TU_12K2276_000000C000000.00000000000O3` in
   `third_party/skate3recomp/`.
3. Run the code generation:

   ```sh
   cd third_party/skate3recomp
   cmake --preset linux-relwithdebinfo -DSKATE3_GAME_DATA_ROOT="$PWD/game"
   cmake --build --preset linux-relwithdebinfo --target generate-all --parallel
   cd ../..
   ```

   This writes about 290 MB of C++ to `third_party/skate3recomp/generated/`. (If the code
   generator crashes when it exits, check that `generated/sources.cmake` and
   `generated/eawebkit/sources.cmake` exist: the output is complete at that point.)
4. Adapt the generated headers to the Switch 2 memory model:

   ```sh
   python3 tools/patch_generated.py
   ```

### 3. Build the NRO

Build the [ounce-devkit](https://github.com/ouncebrew/ounce-devkit) Docker image once:

```sh
git clone https://github.com/ouncebrew/ounce-devkit.git
docker build -t ounce-devkit ounce-devkit
```

Then build the port:

```sh
./build.sh -j8
```

The result is `build/skate3.nro`. The recompiled code needs roughly 2 GB of RAM per compile job:
lower `-j` if the build runs out of memory. A full build takes 20-30 minutes.

To give the NRO an icon in the homebrew menu, put a 256x256 baseline JPEG named `icon.jpg` next to
`CMakeLists.txt` before building.

## Installing on the SD card

```
sdmc:/switch/skate3/
  skate3.nro            build/skate3.nro
  options.txt           options.txt from this repository (required for full speed)
  game/                 the extracted game (the whole game/ folder from step 2)
    default.xexp        Title Update 3 patch files, from
    data/webkit/EAWebkit.xexp    third_party/skate3recomp/out/build/linux-relwithdebinfo/title_update_codegen/
```

The two `.xexp` files are extracted from the title update package by the code generation step;
copy them next to the `.xex` files they patch. The game applies them when it starts.

`options.txt` is needed: it holds the performance settings (CPU clock, GPU emulation tuning,
render target optimizations). Without it the game starts with the SDK's defaults and runs much
slower.

Start `skate3.nro` from the homebrew menu. Saves and settings are kept in
`sdmc:/switch/skate3/user/`, and `sdmc:/switch/skate3/cache/` holds folders the game needs.

## options.txt

`sdmc:/switch/skate3/options.txt` is read at startup: one `name=value` per line, `#` for comments.
Always copy the file from this repository to the SD card: it has the tuned settings the port needs
for its frame rate, and explains each one. Settings you may want to change:

- `handheld_fps_cap=0` (default, uncapped) or `40` for a locked, evenly paced 40 fps in handheld.
- `ns2_debug=true` writes detailed logs and statistics for bug reports (`logs/`, `trace.txt`,
  `sampler.txt`). Without it the game only keeps a small log of the last two launches.
- `handheld.<setting>=<value>` gives any setting a value used only in handheld mode.

## Known issues

- Occasional rendering artifacts (brief visual glitches). They don't affect gameplay.
- Handheld runs at about 50 fps in busy areas (docked holds ~60).
- No native renderer: the game uses the emulated Xbox 360 GPU path, not the PC version's native
  renderer.

## Reporting problems

Enable `ns2_debug=true`, reproduce the problem, then attach `logs/`, `trace.txt`,
`crash_hooks.txt` (if present) and `sampler.txt` from `sdmc:/switch/skate3/`.
`tools/prof_report.py` and `tools/ab_report.py` read the profiler output and statistics.

## Credits

- [skate3recomp](https://github.com/mchughalex/skate3recomp) by mchughalex: the Skate 3
  recompilation this port builds on.
- [rexglue SDK](https://github.com/mchughalex/rexglue-skate3) by Tom Clay, derived from
  [Xenia](https://xenia.jp): runtime, kernel HLE and GPU emulation.
- [ounce-devkit](https://github.com/ouncebrew/ounce-devkit) (libounce, OunceVK, kaiHEN): the
  Switch 2 homebrew toolchain.

## License

The code in this repository is released under the BSD 3-Clause license (see `LICENSE`); see
`NOTICE` for third-party code. Skate 3 is (c) 2010 Electronic Arts Inc. This project is not
affiliated with or endorsed by Electronic Arts or Nintendo.
