# Uncharted: The Nathan Drake Collection for Windows (community build)

Play Uncharted 1, 2 and 3 from the PS4 collection on your Windows PC. The games run natively, not in an emulator. This is SuperMedo's port ([Supermedo/uncharted_pc](https://github.com/Supermedo/uncharted_pc)) with extra fixes from players and other forks, collected into one build.

**[Download the latest build](https://github.com/drakolordx7/uncharted_pc/releases/latest)**

No game files are included: you need your own dump of the PS4 collection. Not affiliated with Sony or Naughty Dog.

## How to use

1. Unzip the download into a folder.
2. Run `Uncharted.exe` and choose your game dump on the launcher's Game page (the folder with `eboot.bin`, `sce_sys`, `u1data`, `u2data` and `u3data`).
3. Pick a game and press Play.

**Updating:** the launcher tells you when a new build is out. You can also unzip a new build over the old one: your settings and saves in the `user` folder are kept.

**You need:** Windows 10 or 11 (64-bit), a CPU with AVX (Intel 2nd gen / AMD FX or newer), and a graphics card with Vulkan 1.3 and 6 GB of video memory or more.

**Tips**
- Keyboard, mouse and controllers all work and can be remapped on the Controls page. Valves that ask you to turn the stick in circles: hold **R** (or **T** the other way).
- If something looks wrong, try the **Fixes** page in the launcher. "Automatic" is the right choice for most people.
- 4 GB graphics cards can run out of memory: keep frame generation off.

**Something broke?** [Open an issue](https://github.com/drakolordx7/uncharted_pc/issues/new/choose) and attach `user\last_run.log` from that session (copy it before you start the game again). Never run a command or download a file someone posts in an issue: real fixes only come as releases on this page.

## Contributing

Pull requests are welcome. Make them against the `community` branch, and say which game, game version (1.00 or 1.02) and graphics card you tested on.

We also bring in good fixes from other forks of the port. The original author stays on the commit and is listed below. If you'd rather your fix wasn't included, open an issue and we'll take it out.

To build it yourself: install MSYS2, open the CLANG64 shell and run `bash build.sh`, then `bash packaging/windows/package.sh` for the zip.

## Credits

**The port:** [SuperMedo](https://github.com/Supermedo) wrote it, on top of [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28 and the [shadPS4](https://github.com/shadps4-emu/shadPS4) renderer. Video decoding comes from [IFreemz](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR). Thanks to Mindro for the first shadPS4 Uncharted build.

**Fixes in this build:**
- [Mevlock](https://github.com/Mevlock): freeze fixes, crash-safe saves, frame generation memory leak, Uncharted 3 cloth, flashlight and speed fixes
- [Umar-Qayyum-dev](https://github.com/Umar-Qayyum-dev): keyboard and mouse button prompts, keyboard layouts, Uncharted 2 speed-ups
- [iannitram](https://github.com/iannitram): crash guards, memory leak fix, FSR fixes, faster compute work
- [currantsforeyes](https://github.com/currantsforeyes): Itemzflow dumps, save and memory fixes, EU 1.02 support
- [spacepepperoni](https://github.com/spacepepperoni) and ngin: Uncharted 2 aiming blur and accurate GPU memory
- [G3545](https://github.com/G3545): black weapons and brightness flicker fix, shader fix
- [FelipePrado0](https://github.com/FelipePrado0): older Intel CPU support, adapted from [SpartanPort](https://github.com/FelipePrado0/SpartanPort)
- [d4rksp4rt4n](https://github.com/d4rksp4rt4n): Uncharted 1 crash on newer Intel CPUs
- [kuraeow](https://github.com/kuraeow): lower CPU load on smaller CPUs
- [poncho-ajmv](https://github.com/poncho-ajmv): launcher error messages, Stop button, vibration toggle
- [Hugo Borges](https://github.com/hugouchoasborges): large GPU job fix, frame rate cap
- [kj2223](https://github.com/kj2223): Uncharted 1 shimmer fix
- [soni424](https://github.com/soni424): dark launcher title bar, shader crash guard
- [eduardoghi](https://github.com/eduardoghi): numpad bindings
- [parsifal295](https://github.com/parsifal295) and [devilismyfriend](https://github.com/devilismyfriend): video memory growth fix
- [drakolordx7](https://github.com/drakolordx7): this build, plus fixes for crashes, mouse, video memory, resolution, ultrawide, surround sound and keyboard controls

**Testing and research:** adrianeira, exyled, fidoboy, kloon15, tDevis, MrBboy95, rayer23, GameBunny77 and everyone who tested builds and sent logs.

**Libraries:** FSR (AMD), DLSS (NVIDIA), XeSS (Intel), LibAtrac9, SDL3, FFmpeg, Dear ImGui, sirit, xbyak, Vulkan Memory Allocator, PyInstaller, Pillow, the Cinzel font and Kenney's Input Prompts. Their licenses are in the `licenses` folder of the download.

Missing from this list, or want it worded differently? Open an issue.

## License

GPL v2 or later, see [LICENSE](LICENSE). Third-party parts keep their own licenses.
