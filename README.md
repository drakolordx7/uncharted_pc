# Uncharted: The Nathan Drake Collection for Windows (community build)

This lets you play Uncharted 1, 2 and 3 from the PS4 collection on a Windows PC. The PS4 game code runs directly on your computer. It's SuperMedo's port ([Supermedo/uncharted_pc](https://github.com/Supermedo/uncharted_pc)) plus fixes from players and other forks.

[Download the latest build](https://github.com/drakolordx7/uncharted_pc/releases/latest)

You need your own dump of the PS4 collection, since no game files are included. This project has nothing to do with Sony or Naughty Dog.

## How to use

1. Unzip the download into a folder.
2. Run `Uncharted.exe`. On the launcher's Game page, pick the folder with your dump (it has `eboot.bin`, `sce_sys`, `u1data`, `u2data` and `u3data` in it).
3. Pick a game and press Play.

The launcher tells you when there's a new build. You can also unzip a new build over the old one, and your settings and saves in the `user` folder stay.

You'll need Windows 10 or 11 (64-bit), a CPU with AVX (Intel 2nd gen, AMD FX or newer), and a graphics card with Vulkan 1.3 and at least 6 GB of video memory.

Keyboard, mouse and controllers all work, and you can remap them on the Controls page. When the game asks you to turn the stick in circles (valves, for example), hold R, or T to turn the other way. If something looks off, check the Fixes page in the launcher; leaving it on Automatic works for most people. On a 4 GB graphics card, keep frame generation off or the game can run out of video memory.

If something breaks, [open an issue](https://github.com/drakolordx7/uncharted_pc/issues/new/choose) and attach `user\last_run.log` from that session. Copy it before you start the game again, because the next run overwrites it. Don't run commands or downloads that people post in issues. Fixes only ever come as releases on this page.

## Contributing

Pull requests are welcome. Send them to the `community` branch and say which game, game version (1.00 or 1.02) and graphics card you tested on.

We also bring in fixes from other forks of the port. The original author stays on the commit and gets listed below. If you don't want your fix in here, open an issue and we'll take it out.

To build it yourself, install MSYS2, open the CLANG64 shell, and run `bash build.sh`. Then `bash packaging/windows/package.sh` makes the zip.

## Credits

[SuperMedo](https://github.com/Supermedo) wrote the port, building on [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28 and the [shadPS4](https://github.com/shadps4-emu/shadPS4) renderer. Video decoding comes from [IFreemz](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR), and Mindro made the first shadPS4 Uncharted build.

Fixes in this build come from:
- [Mevlock](https://github.com/Mevlock): freeze fixes, crash-safe saves, the frame generation memory leak, and Uncharted 3 cloth, flashlight and speed fixes
- [Umar-Qayyum-dev](https://github.com/Umar-Qayyum-dev): keyboard and mouse button prompts, keyboard layouts, Uncharted 2 speed-ups
- [iannitram](https://github.com/iannitram): crash guards, a memory leak fix, FSR fixes, faster compute work
- [currantsforeyes](https://github.com/currantsforeyes): Itemzflow dumps, save and memory fixes, EU 1.02 support
- [spacepepperoni](https://github.com/spacepepperoni) and ngin: Uncharted 2 aiming blur, accurate GPU memory
- [G3545](https://github.com/G3545): the black weapons and brightness flicker fix, a shader fix
- [FelipePrado0](https://github.com/FelipePrado0): older Intel CPU support, adapted from [SpartanPort](https://github.com/FelipePrado0/SpartanPort)
- [d4rksp4rt4n](https://github.com/d4rksp4rt4n): an Uncharted 1 crash on newer Intel CPUs
- [kuraeow](https://github.com/kuraeow): lower CPU load on smaller CPUs
- [poncho-ajmv](https://github.com/poncho-ajmv): launcher error messages, the Stop button, the vibration toggle
- [Hugo Borges](https://github.com/hugouchoasborges): a fix for large GPU jobs, the frame rate cap
- [kj2223](https://github.com/kj2223): the Uncharted 1 shimmer fix
- [soni424](https://github.com/soni424): the dark launcher title bar, a shader crash guard
- [eduardoghi](https://github.com/eduardoghi): numpad bindings
- [parsifal295](https://github.com/parsifal295) and [devilismyfriend](https://github.com/devilismyfriend): a fix for growing video memory use
- [drakolordx7](https://github.com/drakolordx7): this build, and fixes for crashes, the mouse, video memory, resolution, ultrawide, surround sound and keyboard controls

Thanks to adrianeira, exyled, fidoboy, kloon15, tDevis, MrBboy95, rayer23, GameBunny77 and everyone else who tested builds and sent logs.

The build uses AMD FSR, NVIDIA DLSS, Intel XeSS, LibAtrac9, SDL3, FFmpeg, Dear ImGui, sirit, xbyak, Vulkan Memory Allocator, PyInstaller, Pillow, the Cinzel font and Kenney's Input Prompts. Their licenses are in the `licenses` folder of the download.

If you're missing from this list or want your entry worded differently, open an issue.

## License

GPL v2 or later (see [LICENSE](LICENSE)). Third-party parts keep their own licenses.
