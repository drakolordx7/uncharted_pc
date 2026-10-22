# Uncharted: The Nathan Drake Collection for Windows (community build)

**Uncharted 1, 2 and 3 from the PS4 collection running natively on Windows, built on
[SuperMedo's port](https://github.com/Supermedo/uncharted_pc), with fixes from across the community.**

The port is SuperMedo's work: the original PS4 game code runs directly on your PC and the
graphics go through Vulkan. This fork collects the fixes people have written since then, ours and
other contributors', in one build that you can download and play. Everything we write here is also
sent upstream as a pull request, so SuperMedo can take any of it at any time.

**[Download the latest community build](https://github.com/drakolordx7/uncharted_pc/releases/latest)** ·
[What's in it](#whats-in-this-build) · [Report a problem](https://github.com/drakolordx7/uncharted_pc/issues/new/choose) ·
[Credits](CREDITS.md)

> **No game files are included.** You need your own decrypted dump of the collection
> (CUSA02343, CUSA02320, ...: the folder with eboot.bin, big2-ps4_Shipping.elf,
> big3-ps4_Shipping.elf, sce_sys, u1data, u2data and u3data).
> This project is not affiliated with Sony Interactive Entertainment or Naughty Dog.

## Installing and updating

Unzip the download into a folder and run `Uncharted.exe`. To update, unzip the new build over the
old folder and keep your `user` folder: it holds your settings, saves and logs. The launcher checks
this fork's releases for updates, not the official ones.

The builds don't include the DLSS bridge. XeSS and FSR work on any GPU.

## What's in this build

Everything in the official 0.3.6, plus the changes below. Each one is an open pull request on the
official repository; the link goes to it.

### Crashes

- Uncharted 1 "execute access to 0" / exit code 3 (`0x63f5d0`, `0x641532`): [#396](https://github.com/Supermedo/uncharted_pc/pull/396), and [#403](https://github.com/Supermedo/uncharted_pc/pull/403) by d4rksp4rt4n for Intel 12th gen and later
- Uncharted 2 chapter 21 grenade launcher (the big truck takes no damage) and other physics glitches: [#382](https://github.com/Supermedo/uncharted_pc/pull/382) by spacepepperoni
- Uncharted 2 `iSubMesh < m_numSubMeshInstances` assertion: [#397](https://github.com/Supermedo/uncharted_pc/pull/397)
- Start failures with exit 21 / error 487 (something already sits in the PS4 address range): [#406](https://github.com/Supermedo/uncharted_pc/pull/406)
- Draw-preparation workers no longer take the whole game down when they fault: [#409](https://github.com/Supermedo/uncharted_pc/pull/409)
- `m_gfxEopTick` assertion on more game versions: [#405](https://github.com/Supermedo/uncharted_pc/pull/405)
- Memory use that keeps growing over a long session: [#445](https://github.com/Supermedo/uncharted_pc/pull/445) (same fix as iannitram's #387)

### Performance

- Much less CPU load on 4 and 6 core processors (idle job workers sleep): [#346](https://github.com/Supermedo/uncharted_pc/pull/346) by kuraeow
- Less shader stutter: pipelines compile off the GPU thread: [#389](https://github.com/Supermedo/uncharted_pc/pull/389), [#416](https://github.com/Supermedo/uncharted_pc/pull/416)
- 4 GB cards stay inside their video memory: [#390](https://github.com/Supermedo/uncharted_pc/pull/390)
- A separate shader cache per GPU (fixes an Intel crash on a cache built by another GPU): [#412](https://github.com/Supermedo/uncharted_pc/pull/412)

### Graphics

- Uncharted 2 blur when aiming and on turrets: [#381](https://github.com/Supermedo/uncharted_pc/pull/381) by spacepepperoni
- Black weapons and brightness flicker: [#449](https://github.com/Supermedo/uncharted_pc/pull/449) by G3545
- Render above 1080p, and ultrawide: [#391](https://github.com/Supermedo/uncharted_pc/pull/391)
- Intel XeSS as an anti-aliasing choice: [#476](https://github.com/Supermedo/uncharted_pc/pull/476)

### Controls and launcher

- Mouse look on game version 1.02 (hooks found by pattern): [#348](https://github.com/Supermedo/uncharted_pc/pull/348)
- Keyboard and mouse button prompts, layouts and custom keybinding: [#420](https://github.com/Supermedo/uncharted_pc/pull/420) by Umar-Qayyum-dev
- Xbox button prompts for Xbox and other controllers: [#469](https://github.com/Supermedo/uncharted_pc/pull/469)
- Trophy notices in the game and a Trophies page in the launcher: [#472](https://github.com/Supermedo/uncharted_pc/pull/472)
- `--u1`, `--u2`, `--u3` start a game straight from a shortcut: [#399](https://github.com/Supermedo/uncharted_pc/pull/399)

### Audio

- 5.1 and 7.1 surround: [#392](https://github.com/Supermedo/uncharted_pc/pull/392)

Each release lists its exact changes in the [release notes](https://github.com/drakolordx7/uncharted_pc/releases).

## Reporting problems

Open an issue [here](https://github.com/drakolordx7/uncharted_pc/issues/new/choose) and attach
`user\last_run.log` from the run where it went wrong. Copy the log before you start the game again,
because the next run overwrites it.

**Never run a command or a download from an issue or a comment.** Issues titled "All Crashes Fix
Command" and similar are malware. Fixes only ever come as a release on this page.

## Contributing

Pull requests are welcome here. We also bring in good fixes from other forks and from pull requests
on the official repository, and we keep their authors on the commits and in [CREDITS.md](CREDITS.md).
If you'd rather your change didn't come in, open an issue and we'll take it out. See
[CONTRIBUTING.md](CONTRIBUTING.md) for how the branches are laid out.

## Features

- All three games from one launcher: Drake's Fortune, Among Thieves and Drake's Deception.
- 60 FPS like on PS4, or 120 FPS (higher rates are experimental).
- Anti-aliasing with TAA, AMD FSR 3.1, AMD FSR 4, NVIDIA DLSS (DLAA) or Intel XeSS (any GPU).
- Frame generation: AMD FSR 3 (any GPU) or NVIDIA DLSS (RTX 40 2x, RTX 50 up to 4x).
- Keyboard, mouse and controller together, all remappable. Mouse look in all three games, aiming included (game versions 1.00 and 1.02; in Among Thieves 1.00 only checked up to the opening scene).
- Button prompts show your keyboard and mouse keys while you play with them, Xbox buttons after
  input from an Xbox (or other) controller, and the PlayStation buttons after a PlayStation
  controller. Keys follow your keyboard layout (AZERTY...).
- Trophies: the games' own trophy lists, a notice in the game when one is earned, and a Trophies page in the launcher.
- Add to Steam and desktop shortcuts from the launcher, automatic updates.

## Requirements

- Windows 10 (1903 or later) or Windows 11, 64-bit.
- An Intel Core 4th generation (Haswell) or newer, an AMD Ryzen, or an AMD FX.
- A graphics card with Vulkan 1.3 and a current driver, with **6 GB of video memory or more**.
  4 GB cards (GTX 1650, RX 570) run out of video memory and crash after a while; on those, keep
  Anti-aliasing and Frame generation off.
- About 8 GB of free memory (RAM + page file).

## Known problems

- **Uncharted 3, Cartagena square (chapter 2):** at high frame rates (240 Hz setting, ~80 FPS)
  the game could freeze and close with "GPU crash: the graphics device was lost". It came from
  the speed fix for that area and is fixed in 0.3. If it still happens to you, set Frame rate to
  60 on the Display page and open an issue with your `user\last_run.log`.
- **Uncharted 3:** green or black shapes flash for a single frame on some characters (Sully's
  shirt in Cartagena). It happens in shadPS4 too, so it's a shader translation problem. Not fixed yet.
- **Crash with "ASSERTION: GetRenderFrameParams(frameToFreeInstancesFor)->m_gfxEopTick"**
  (mostly U1 after the first cutscene): the game expected the GPU side to be done with a frame it
  wanted to reuse, and stopped when it wasn't yet (shaders building on a first run). Fixed in
  0.3.2 (0.3.4 for the 1.02 versions): the game now waits for it. If you still see it, open an
  issue with your log.
- **Intel HD / UHD graphics:** a black screen on the HD 630 is fixed in 0.3.3. Older Intel chips
  (HD 520) can still crash inside Intel's own driver.
- **Uncharted 3 London bar crash** (fault at `0xd9c59b`, or device lost in the bar fight): fixed in
  0.3.4.
- **Uncharted 2 loading crashes** (`ReadSync file (req/read: ...)`, white textures then
  `iSubMesh < m_numSubMeshInstances`): file reads could come back cut short. Fixed in 0.3.5.
- **Older CPUs:** the games use instructions of the PS4's processor (AVX, BMI1, MOVBE, LZCNT).
  AMD FX runs since 0.3.6 (its missing MOVBE is emulated, a bit slower). Intel Core 3rd generation
  and older can't run the games yet; the game says so instead of crashing with 0xc000001d.
- **Uncharted 2, train wreck:** `IsFinite(deltaTransform)` in character-leg-ik.cpp, fixed in 0.3.6
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
