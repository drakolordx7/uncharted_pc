# Uncharted: The Nathan Drake Collection for Windows (community build)

Uncharted 1, 2 and 3 from the PS4 collection running natively on Windows, built on
[SuperMedo's port](https://github.com/Supermedo/uncharted_pc) (0.3.8), with fixes from across the community.

The port is SuperMedo's work: the original PS4 game code runs directly on your PC and the
graphics go through Vulkan. This fork collects the fixes people have written since then, ours and
other contributors', in one build that you can download and play. Everything we write here is also
sent upstream as a pull request, so SuperMedo can take any of it at any time.

[Download the latest community build](https://github.com/drakolordx7/uncharted_pc/releases/latest) ·
[What's in it](#whats-in-this-build) · [Report a problem](https://github.com/drakolordx7/uncharted_pc/issues/new/choose) ·
[Credits](CREDITS.md)

> No game files are included. You need your own decrypted dump of the collection
> (CUSA02343, CUSA02320, ...: the folder with eboot.bin, big2-ps4_Shipping.elf,
> big3-ps4_Shipping.elf, sce_sys, u1data, u2data and u3data).
> This project is not affiliated with Sony Interactive Entertainment or Naughty Dog.

## Installing and updating

Unzip the download into a folder and run `Uncharted.exe`. To update, unzip the new build over the
old folder and keep your `user` folder: it holds your settings, saves and logs. The launcher checks
this fork's releases for updates, not the official ones.

The builds don't include the DLSS bridge. XeSS and FSR work on any GPU.

## What's in this build

Everything in the official 0.3.8, plus the changes below. Where there is a link, it goes to the pull
request on the official repository.

### Crashes

- Uncharted 1, 2 and 3 "execute access to 0" (exit code 3) when the constant table fill takes a page fault: its pages are written before the fill starts, on top of 0.3.8's red-zone fix: [#396](https://github.com/Supermedo/uncharted_pc/pull/396)

### Controls and launcher

- `--u1`, `--u2`, `--u3` start a game straight from a shortcut: [#399](https://github.com/Supermedo/uncharted_pc/pull/399)

### Older CPUs

- Runs on Intel Sandy and Ivy Bridge: the instructions they lack (BMI1, MOVBE, LZCNT, F16C) are rewritten at start-up (adapted from FelipePrado0's SpartanPort). Players confirmed it on a real Ivy Bridge. 0.3.8 has its own rewrite for Ivy Bridge; this build uses this one instead, because it also covers Sandy Bridge.

### From other forks

- A display flip that arrives before its label no longer stops the game, a missing CE task no longer crashes the GPU thread, and FSR 3.1 no longer uses protected memory (noise on AMD Phoenix): from iannitram's fork
- FSR 3 frame generation no longer leaks video memory every frame, and conditional GPU commands test the whole condition: Mevlock
- Dark title bar for the launcher on Windows: soni424
- Faster in Uncharted 2 and 3 (U3 Cartagena square 26 → 48 FPS in our tests): compute work no longer waits on the whole draw pipe, compute fences run in order with it, and typed-buffer writes copy only what changed. Also fixes an Uncharted 3 freeze about 15 seconds in: iannitram, Mevlock
- Uncharted 3 cloth no longer reads past shared memory, and the flashlight and shadow slices no longer drop out: Mevlock
- A bad GPU command, a failed pipeline or a missing vertex buffer table skips one draw instead of closing the game, and a null shader binding no longer crashes the jungle chapter (1.00 and 1.02): iannitram, soni424
- Textures are freed again when video memory fills up, so it no longer keeps growing: parsifal295, devilismyfriend
- Shader fix: GCN-style NaN compares (from shadPS4): iannitram
- Large compute dispatches are split, and a sparse-capable queue is picked when the GPU needs it: Hugo Borges
- Saves are written so an interrupted save can't corrupt the file: Mevlock
- Freeze fixes: the GPU thread could hang on its list of GPU-written memory (an Uncharted 3 chapter 1 freeze, and a likely cause of Uncharted 2 freezes), and flips at deaths, cutscenes and movie changes no longer wait on the thread that raised them: Mevlock
- Launcher: a frame rate cap: Hugo Borges
- Frame generation no longer crashes on a resized or undersized frame, closing the window no longer hangs, and Uncharted 3 no longer hangs on a first run with an empty shader cache: Mevlock
- Less CPU time spent waiting: the draw pipe sleeps instead of spinning, ignored predication no longer drains it, and barrier and register tracking do less work: Umar-Qayyum-dev, Mevlock
- Uncharted 2 reuses texture sets and skinning data instead of rebuilding them: Umar-Qayyum-dev
- A shader cache's meta and binary files must come from the same version (caches rebuild once): Mevlock
- Optional longer job-worker spin before sleeping, which Mevlock found reduces Uncharted 3 lighting flashes but costs FPS (`BB_JOB_SLEEP_SPINS=50000 BB_JOB_SLEEP_MS=1`): based on Mevlock's fork

Each release lists its exact changes in the [release notes](https://github.com/drakolordx7/uncharted_pc/releases).

## Reporting problems

Open an issue [here](https://github.com/drakolordx7/uncharted_pc/issues/new/choose) and attach
`user\last_run.log` from the run where it went wrong. Copy the log before you start the game again,
because the next run overwrites it.

Never run a command or a download from an issue or a comment. Issues titled "All Crashes Fix
Command" and similar are malware. Fixes only ever come as a release on this page.

## Contributing

Pull requests are welcome here. We also bring in good fixes from other forks and from pull requests
on the official repository, and we keep their authors on the commits and in [CREDITS.md](CREDITS.md).
If you'd rather your change didn't come in, open an issue and we'll take it out. See
[CONTRIBUTING.md](CONTRIBUTING.md) for how the branches are laid out.

## Features

- All three games from one launcher: Drake's Fortune, Among Thieves and Drake's Deception.
- 60 FPS like on PS4, or 120 FPS (higher rates are experimental).
- Render resolution above 1080p (up to 3200x1800) and ultrawide aspect ratios: the games draw a real larger or wider picture, set in the launcher.
- Anti-aliasing with TAA, AMD FSR 3.1, AMD FSR 4, NVIDIA DLSS (DLAA) or Intel XeSS (any GPU).
- Frame generation: AMD FSR 3 (any GPU) or NVIDIA DLSS (RTX 40 2x, RTX 50 up to 4x).
- Keyboard, mouse and controller together, all remappable. Mouse look in all three games, aiming included (game versions 1.00 and 1.02; in Among Thieves 1.00 only checked up to the opening scene).
- Valves and wheels that ask you to turn the left stick in circles (the submarine hatches in Drake's Fortune) work from the keyboard: hold R to turn counter-clockwise or T to turn clockwise (both remappable on the Controls page).
- Button prompts show your keyboard and mouse keys while you play with them, Xbox buttons after
  input from an Xbox (or other) controller, and the PlayStation buttons after a PlayStation
  controller. Keys follow your keyboard layout (AZERTY...).
- Trophies: the games' own trophy lists, a notice in the game when one is earned, and a Trophies page in the launcher.
- Stereo, 5.1 or 7.1 sound: the games are told the real speaker layout (it follows the Windows setting, or pick it in the launcher).
- Controller vibration settings, a Cheats page (infinite health and ammo per game), launcher languages, and your own covers and Steam art.
- A Fixes page for the fixes that cost speed: Automatic turns each one on only where it helps.
- Add to Steam and desktop shortcuts from the launcher, automatic updates.

## Requirements

- Windows 10 (1903 or later) or Windows 11, 64-bit.
- An Intel Core 2nd generation (Sandy Bridge) or newer, an AMD Ryzen, or an AMD FX (the CPU needs AVX).
- A graphics card with Vulkan 1.3 and a current driver, with 6 GB of video memory or more.
  4 GB cards (GTX 1650, RX 570) run out of video memory and crash after a while; on those, keep
  Anti-aliasing and Frame generation off.
- About 8 GB of free memory (RAM + page file).

## Known problems

- Uncharted 3, Cartagena square (chapter 2): at high frame rates (240 Hz setting, ~80 FPS)
  the game could freeze and close with "GPU crash: the graphics device was lost". It came from
  the speed fix for that area and is fixed in 0.3. If it still happens to you, set Frame rate to
  60 on the Display page and open an issue with your `user\last_run.log`.
- Uncharted 3: green or black shapes flash for a single frame on some characters (Sully's
  shirt in Cartagena). It happens in shadPS4 too, so it's a shader translation problem. Not fixed yet.
- Crash with "ASSERTION: GetRenderFrameParams(frameToFreeInstancesFor)->m_gfxEopTick"
  (mostly U1 after the first cutscene): the game expected the GPU side to be done with a frame it
  wanted to reuse, and stopped when it wasn't yet (shaders building on a first run). Fixed in
  0.3.2 (0.3.4 for the 1.02 versions): the game now waits for it. If you still see it, open an
  issue with your log.
- Intel HD / UHD graphics: a black screen on the HD 630 is fixed in 0.3.3. Older Intel chips
  (HD 520) can still crash inside Intel's own driver.
- Uncharted 3 London bar crash (fault at `0xd9c59b`, or device lost in the bar fight): fixed in
  0.3.4.
- Uncharted 2 loading crashes (`ReadSync file (req/read: ...)`, white textures then
  `iSubMesh < m_numSubMeshInstances`): file reads could come back cut short. Fixed in 0.3.5.
- Older CPUs: the games use PS4 CPU instructions (AVX, BMI1, MOVBE, LZCNT, F16C). AMD FX runs since
  0.3.6. Intel 2nd and 3rd gen (Sandy and Ivy Bridge, Xeon E3/E5 v1 and v2) lack some of them, so the
  loader rewrites those spots when the game starts and runs them in software, a little slower
  (#197, #515). Players confirmed it on a real Ivy Bridge; Sandy Bridge is only tested with the rewrite
  forced on a modern CPU.
  CPUs without AVX (Nehalem and older, Phenom II) still can't run the games.
- Random crashes with "execute access to 0" or a fault at `0x63f5d0` (U1): Windows wrote its
  exception data over values the game keeps just below the stack pointer. Fixed in 0.3.7.
- Uncharted 2, train wreck: `IsFinite(deltaTransform)` in character-leg-ik.cpp, fixed in 0.3.6
  for the EU 1.00 version.
- Frame rates above 120 are experimental.

## Building

MSYS2 CLANG64 on Windows: `bash build.sh` builds `out\bb-probe.exe`, and
`bash packaging/windows/package.sh` makes `dist\uncharted-windows` and the zip.

## Credits

The people whose fixes are in this community build are listed in [CREDITS.md](CREDITS.md).

Built on [bbport](https://github.com/deadinside28/bloodborne_pc), the native Linux port of
Bloodborne by deadinside28, and on the [shadPS4](https://github.com/shadps4-emu/shadPS4)
renderer. Video decoding (Videodec2) and the DLSS bridge come from
[IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR).
Thanks to Mindro for the shadPS4 Uncharted build that showed which problems to look for.

Also used: [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK (FSR),
FSR 4 assets from [FireBurn/Q2RTX](https://github.com/FireBurn/Q2RTX), the
[NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) (`nvngx_dlss.dll` under NVIDIA's license),
[Intel XeSS SDK](https://github.com/intel/xess) (`libxess.dll` under Intel's license),
[LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [SDL3](https://github.com/libsdl-org/SDL),
[FFmpeg](https://ffmpeg.org), [Dear ImGui](https://github.com/ocornut/imgui),
[sirit](https://github.com/shadps4-emu/sirit), [magic_enum](https://github.com/Neargye/magic_enum),
[xbyak](https://github.com/herumi/xbyak),
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[MSYS2](https://www.msys2.org) / [LLVM](https://llvm.org), [PyInstaller](https://pyinstaller.org),
[Pillow](https://python-pillow.org), the Cinzel font (SIL Open Font License) and the Xbox
button pictures from [Kenney](https://kenney.nl/assets/input-prompts)'s Input Prompts (CC0).

NVIDIA, GeForce RTX and DLSS are trademarks of NVIDIA Corporation. The default covers and the
icon are original artwork; no official art is included.

## License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses
(see `licenses\` in the download).
