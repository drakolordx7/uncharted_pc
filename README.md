# Uncharted: The Nathan Drake Collection for Windows (community build)

Community build of [SuperMedo's Uncharted PC port](https://github.com/Supermedo/uncharted_pc), with fixes from players and other forks. Runs Uncharted 1, 2 and 3 from the PS4 collection on Windows.

[Download the latest build](https://github.com/drakolordx7/uncharted_pc/releases/latest)

No game files are included; you need your own dump of the collection (CUSA02343, CUSA02344 or CUSA02320, version 1.00 or 1.02). Not affiliated with Sony or Naughty Dog.

## Requirements

- Windows 10 or 11, 64-bit
- CPU with AVX (Intel 2nd gen, AMD FX or newer)
- GPU with Vulkan 1.3 and 6 GB of video memory (4 GB cards work with frame generation off)

## Usage

1. Unzip the build into a folder and run `Uncharted.exe`.
2. On the Game page, select your dump folder (the one with `eboot.bin`, `sce_sys`, `u1data`, `u2data` and `u3data`).
3. Pick a game and press Play.

Settings, saves and logs are stored in `user\`. To update, use the launcher's update prompt or unzip a new build over the old one.

Controls are remapped on the Controls page. For prompts that ask you to rotate the left stick, hold R (or T for the other direction). Fixes that cost performance are on the Fixes page; Automatic enables each one only where it's needed.

## Reporting problems

[Open an issue](https://github.com/drakolordx7/uncharted_pc/issues/new/choose) with `user\last_run.log` from the session where it happened (the next run overwrites it). Releases on this page are the only source of fixes; don't run commands posted in issues.

## Contributing

- `community` is the main branch and what releases are built from. `main` mirrors upstream.
- Open pull requests against `community`. Mention the game, game version and GPU you tested on.
- Fixes from other forks are cherry-picked with the original author kept on the commit.
- Build with MSYS2 (CLANG64 shell): `bash build.sh` produces `out\bb-probe.exe`, and `bash packaging/windows/package.sh` produces the release zip.

## Credits

Port by [SuperMedo](https://github.com/Supermedo), built on [bbport](https://github.com/deadinside28/bloodborne_pc) (deadinside28) and the [shadPS4](https://github.com/shadps4-emu/shadPS4) renderer. Video decoding from [IFreemz](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR). Thanks to Mindro for the first shadPS4 Uncharted build.

Fixes in this build:
- [Mevlock](https://github.com/Mevlock): freeze fixes, crash-safe saves, frame generation memory leak, Uncharted 3 cloth, flashlight and performance
- [Umar-Qayyum-dev](https://github.com/Umar-Qayyum-dev): keyboard and mouse button prompts, keyboard layouts, Uncharted 2 performance
- [iannitram](https://github.com/iannitram): crash guards, memory leak fix, FSR fixes, compute performance
- [currantsforeyes](https://github.com/currantsforeyes): Itemzflow dumps, save and memory fixes, EU 1.02 support
- [spacepepperoni](https://github.com/spacepepperoni), ngin: Uncharted 2 aiming blur, accurate GPU memory
- [G3545](https://github.com/G3545): black weapons and brightness flicker, shader fix
- [FelipePrado0](https://github.com/FelipePrado0): older Intel CPU support, adapted from [SpartanPort](https://github.com/FelipePrado0/SpartanPort)
- [d4rksp4rt4n](https://github.com/d4rksp4rt4n): Uncharted 1 crash on newer Intel CPUs
- [kuraeow](https://github.com/kuraeow): idle worker sleep (lower CPU load)
- [poncho-ajmv](https://github.com/poncho-ajmv): launcher error reporting, Stop button, vibration toggle
- [Hugo Borges](https://github.com/hugouchoasborges): large compute dispatch fix, frame rate cap
- [kj2223](https://github.com/kj2223): Uncharted 1 upscaler shimmer
- [soni424](https://github.com/soni424): dark launcher title bar, null shader binding guard
- [eduardoghi](https://github.com/eduardoghi): numpad bindings
- [parsifal295](https://github.com/parsifal295), [devilismyfriend](https://github.com/devilismyfriend): texture memory growth
- [drakolordx7](https://github.com/drakolordx7): community build maintainer; crash, mouse, video memory, resolution, ultrawide, surround and keyboard fixes

Testing and research: adrianeira, exyled, fidoboy, kloon15, tDevis, MrBboy95, rayer23, GameBunny77 and everyone who sent logs.

Third-party: AMD FSR, NVIDIA DLSS, Intel XeSS, LibAtrac9, SDL3, FFmpeg, Dear ImGui, sirit, xbyak, Vulkan Memory Allocator, PyInstaller, Pillow, Cinzel font, Kenney Input Prompts. Licenses are in the `licenses` folder of the release.

## License

GPL v2 or later, see [LICENSE](LICENSE). Third-party components keep their own licenses.
