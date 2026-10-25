# Uncharted: The Nathan Drake Collection for Windows (community build)

Community build by [drakolordx7](https://github.com/drakolordx7) of [SuperMedo's Uncharted PC port](https://github.com/Supermedo/uncharted_pc), with fixes from players and other forks. Runs Uncharted 1, 2 and 3 from the PS4 collection on Windows.

[Download the latest build](https://github.com/drakolordx7/uncharted_pc/releases/latest)

No game files are included; you need your own dump of the collection (CUSA02343, CUSA02344 or CUSA02320, version 1.00 or 1.02). Not affiliated with Sony or Naughty Dog.

## Requirements

- Windows 10 (1903 or later) or Windows 11, 64-bit
- CPU with AVX, SSE4.2 and POPCNT: Intel 2nd gen (Sandy Bridge), AMD FX or newer
- GPU with Vulkan 1.3 and at least 4 GB of video memory (6 GB or more recommended; on 4 GB keep frame generation off)
- About 8 GB of free memory, RAM plus page file (leave the Windows page file on "System managed size")

## Usage

1. Unzip the build into a folder and run `Uncharted.exe`.
2. On the Game page, select your dump folder (the one with `eboot.bin`, `sce_sys`, `u1data`, `u2data` and `u3data`).
3. Pick a game and press Play.

Settings, saves and logs are stored in `user\`. To update, use the launcher's update prompt or unzip a new build over the old one.

Controls are remapped on the Controls page. For prompts that ask you to rotate the left stick, hold R (or T for the other direction); the prompt on screen shows that key. Fixes that cost performance are on the Fixes page; Automatic enables each one only where it's needed.

Saves are backed up on the Saves page, and automatically before each launch (the last 10 are kept). Its Edit button unlocks all chapters, Crushing and Brutal, and Uncharted 1's rewards; close the game first. Every edit makes a backup first.

## Reporting problems

[Open an issue](https://github.com/drakolordx7/uncharted_pc/issues/new/choose) with `user\last_run.log` from the session where it happened (the next run overwrites it). Releases on this page are the only source of fixes; don't run commands posted in issues.

## Contributing

- `community` is the main branch and what releases are built from. `main` mirrors upstream.
- Open pull requests against `community`. Mention the game, game version and GPU you tested on.
- Fixes from other forks are cherry-picked with the original author kept on the commit.
- Build with MSYS2 (CLANG64 shell): `bash build.sh` produces `out\bb-probe.exe`, and `bash packaging/windows/package.sh` produces the release zip.

## Credits

Community build by [drakolordx7](https://github.com/drakolordx7). Port by [SuperMedo](https://github.com/Supermedo), built on [bbport](https://github.com/deadinside28/bloodborne_pc) (deadinside28) and the [shadPS4](https://github.com/shadps4-emu/shadPS4) renderer. Video decoding from [IFreemz](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR). Thanks to Mindro for the first shadPS4 Uncharted build.

Fixes and features in this build, by contributor. Pull request links go to the official repository; "fork" links go to the contributor's own fork the change came from.

- [Mevlock](https://github.com/Mevlock) ([fork](https://github.com/Mevlock/uncharted_pc-dev)): Uncharted 2 and 3 freeze fixes (buffer cache range lock, end-of-pipe flips, precise read-back deadlock), crash-safe save writes, FSR 3 frame generation memory leak, frame generation crash on resized frames, exit hang, Uncharted 3 first-run shader hang, Uncharted 3 cloth (compute shared memory bounds), flashlight and shadow dropout (depth array layers, HTILE), shader cache meta/binary pairing, tight typed-buffer writes, barrier and register tracking speed-ups, ignored-predication drain, COND_EXEC fix, buffer descriptors with an unsupported format, the optional longer job-worker spin, the 4-core job wait fix (a worker with a render context open retries a taken job lock in place before its job suspends, all three games), and the optional intro logo skip
- [BilalReloaded](https://github.com/bc160201983) ([Uncharted port](https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port), GPL v2): Uncharted 3 cloth solver barriers on 32-lane GPUs (Sully's shirt flashing in Cartagena), guest memory committed as the game allocates it (start failures with error 1450 on PCs with a small page file), Uncharted 3 look-at reset (crash climbing the fort in chapter 8), garbage descriptor guards (Uncharted 2 treasure effects, Uncharted 3 Syria), pipelines the driver refuses are created again without the pipeline cache, samplers without a border color table, resource tables the game is still filling
- [Umar-Qayyum-dev](https://github.com/Umar-Qayyum-dev): keyboard and mouse button prompts in all three games with automatic switching and keyboard layouts ([#420](https://github.com/Supermedo/uncharted_pc/pull/420)), the "execute access to 0" / red zone crash fix for all three games ([#559](https://github.com/Supermedo/uncharted_pc/pull/559)), Uncharted 2 aim blur ([#528](https://github.com/Supermedo/uncharted_pc/pull/528)), Ivy Bridge support ([#336](https://github.com/Supermedo/uncharted_pc/pull/336)), tiny render result read-back, Uncharted 2 texture set and skinning reuse, draw pipe sleep waits, wave-local shared memory barriers (optional)
- [spacepepperoni](https://github.com/spacepepperoni) ([fork](https://github.com/spacepepperoni/uncharted_Linux)): Uncharted 2 turret and aiming blur ([#381](https://github.com/Supermedo/uncharted_pc/pull/381)), accurate GPU memory for Uncharted 2, fixing chapter 21 and other physics problems ([#382](https://github.com/Supermedo/uncharted_pc/pull/382)), NaN guard for black squares in Uncharted 2
- [ngin](https://github.com/ngin): accurate GPU memory by default with a cheaper read-back, compute fences in order with the draw pipe (+42% in the U2 convoy) and written through backing memory (fixes Uncharted 3 freezing about 15 s in), compute queue drains only where needed, turret blur read-back, PS4 FPU state for guest threads, Linux build fixes
- João Pedro Valadares ([fork](https://github.com/spacepepperoni/uncharted_Linux)): texture cache images no longer move in memory (crash in the depth target code), GPU recording chunk never null, motion-vector pipelines not preloaded
- [ilyaqww](https://github.com/ilyaqww) ([fork](https://github.com/ilyaqww/uncharted_pc)): Uncharted 1 display driver reset on GeForce GTX 10 series (1024-thread compute groups run as 2x512, [#634](https://github.com/Supermedo/uncharted_pc/pull/634)), every lost device goes to the GPU crash report instead of an assert, and a crash names the commands the GPU had in flight
- [DUDUYG](https://github.com/DUDUYG): shader resource walker patched under a lock and for walkers loaded from the pipeline cache ("SrtWalkerSignalHandler" stop, read access to 0 in generated code)
- Vishwas Parpattegar ([fork](https://github.com/RealDishwash/uncharted_Linux)): shader resource walker generation no longer freezes after a GPU assertion on a draw preparation worker
- [Pepin208](https://github.com/Pepin208) ([fork](https://github.com/Pepin208/uncharted_pc_linux)): Uncharted 1 blue textures on rocks in the first jungle area (texture cache subresource check and stencil link, from shadPS4 #4818 and #5118), in-game display mode switch (windowed, borderless, exclusive fullscreen) applied while playing, in-game menu kept inside the game window
- [Tutuzaooo](https://github.com/Tutuzaooo) ([fork](https://github.com/Tutuzaooo/uncharted_Linux)): FSR 4 is only offered when its files are installed
- [dbcalo](https://github.com/dbcalo) ([fork](https://github.com/dbcalo/uncharted_pc)): in-game menu checkboxes and sliders keep the new value ([#623](https://github.com/Supermedo/uncharted_pc/pull/623)); raw mouse aiming in Uncharted 1 (the mouse turns the aiming camera 1:1, no stick speed cap), the cover camera turning with the mouse, and popping out of cover keeping the aim ([#626](https://github.com/Supermedo/uncharted_pc/pull/626)), built on Umar-Qayyum-dev's raw mouse aiming ([#561](https://github.com/Supermedo/uncharted_pc/pull/561)); no jump when aiming starts (the pop-out turn eases in and aims at the camera as shown)
- [antikmewzik](https://github.com/antikmewzik) ([fork](https://github.com/antikmewzik/uncharted_pc_controls_qol)): Uncharted 2 (1.02) mouse cameras: the aim, grenade, cover, look-around and chapter 5 truck chase cameras turned by the mouse directly
- [FelipePrado0](https://github.com/FelipePrado0): the load-time rewrite of BMI1, LZCNT, MOVBE and F16C instructions that runs the games on Sandy and Ivy Bridge, adapted from [SpartanPort](https://github.com/FelipePrado0/SpartanPort)
- [iannitram](https://github.com/iannitram) ([fork](https://github.com/iannitram/uncharted_pc)): nested indirect buffer leak fix ([#387](https://github.com/Supermedo/uncharted_pc/pull/387)), title number from AppContent ([#353](https://github.com/Supermedo/uncharted_pc/pull/353)), GCN NaN compare semantics (from shadPS4 #5257), missing CE task guard, early flip IRQ, FSR 3.1 protected memory and patch fixes, FSR driver logging, crash guards for bad GPU commands, failed pipelines and null vertex tables, save mount and flexible memory reports (#496, #497), compute queue ring reset, Vulkan device retry without DLSS, unsupported pixel formats
- [currantsforeyes](https://github.com/currantsforeyes) ([fork](https://github.com/currantsforeyes/uncharted_pc)): Itemzflow plain ELF dumps, save mount limit, flexible mapping split, EU CUSA02344 1.02 red zone tables, window title
- garyth.hawkins: idle job workers on the retail 1.02 build, EU 1.02 red zone tables, fallback window title, mount table and flexible mapping fixes
- [G3545](https://github.com/G3545): small compute-written buffers copied back to the game, fixing black weapons and brightness flicker ([#449](https://github.com/Supermedo/uncharted_pc/pull/449)), scalar instructions inside EXEC scopes ([#468](https://github.com/Supermedo/uncharted_pc/pull/468))
- [kj2223](https://github.com/kj2223): Uncharted 1 upscaler shimmer, jittered full-screen passes, English in-game menu, Uncharted 2 grey screen on launch ([#261](https://github.com/Supermedo/uncharted_pc/pull/261))
- [d4rksp4rt4n](https://github.com/d4rksp4rt4n): Uncharted 1 crash at 0x63f5d0 on Intel 12th gen and newer ([#403](https://github.com/Supermedo/uncharted_pc/pull/403))
- [kuraeow](https://github.com/kuraeow): idle job workers sleep, lowering CPU load on smaller CPUs ([#346](https://github.com/Supermedo/uncharted_pc/pull/346))
- [poncho-ajmv](https://github.com/poncho-ajmv) ([fork](https://github.com/poncho-ajmv/uncharted_pc)): launcher error reporting (crash reason, input settings, shader cache), reliable Stop, controller vibration settings, launcher languages, custom artwork, update package verification ([#607](https://github.com/Supermedo/uncharted_pc/pull/607)), atomic settings writes ([#605](https://github.com/Supermedo/uncharted_pc/pull/605)), prepared executable cache check ([#606](https://github.com/Supermedo/uncharted_pc/pull/606)), previous process wait check ([#604](https://github.com/Supermedo/uncharted_pc/pull/604))
- [bucanero](https://github.com/bucanero) ([save-decrypters](https://github.com/bucanero/save-decrypters), [Apollo Save Tool patches](https://github.com/bucanero/apollo-patches)), from Red-EyeX32 and aerosoul94's tlou_save_data_decrypter: the save encryption keys and checksum layout the save editor uses; Mevlock and BilalReloaded's "Unlock all chapters" toggle showed which menus Bluepoint's unlock flags control
- [saulo benigno](https://github.com/saulob): Cheats page with infinite health and ammo ([#116](https://github.com/Supermedo/uncharted_pc/pull/116))
- [Hugo Borges](https://github.com/hugouchoasborges) ([fork](https://github.com/hugouchoasborges/uncharted_pc)): compute dispatches above 65535 groups, sparse-capable queue selection, frame rate cap presets
- [sid1552](https://github.com/sid1552) ([fork](https://github.com/sid1552/uncharted_pc)): MP3 audio decoding, GPU crash report on device loss, buffer cache per 64-byte block, current MSYS2 build
- [EdouardSence](https://github.com/EdouardSence) ([fork](https://github.com/EdouardSence/uncharted_pc)): large command data no longer stops the game, GPU copies mark textures dirty, overlay cleanup, Linux window icon
- [soni424](https://github.com/soni424) ([fork](https://github.com/soni424/uncharted_pc)): dark launcher title bar, null shader binding guard
- [eduardoghi](https://github.com/eduardoghi): numpad bindings ([#473](https://github.com/Supermedo/uncharted_pc/pull/473)), zip that extracts on Linux ([#404](https://github.com/Supermedo/uncharted_pc/pull/404))
- [Canneli](https://github.com/canneli): design of the save backups (ZIP with a description file, checked paths, a backup before every restore), from their launcher
- [parsifal295](https://github.com/parsifal295) and [devilismyfriend](https://github.com/devilismyfriend): texture collector frees textures again (video memory growth)
- Pedro Delfino: more texture eviction under critical memory pressure
- Rick: prepare.py on CP950 Windows systems
- [gelibeanz](https://github.com/gelibeanz), [vander-x](https://github.com/vander-x): Linux file reads and build fixes; [lunar-me](https://github.com/lunar-me): documentation fixes
- [drakolordx7](https://github.com/drakolordx7): maintains this build; render above 1080p and ultrawide ([#391](https://github.com/Supermedo/uncharted_pc/pull/391)), Xbox button prompts ([#469](https://github.com/Supermedo/uncharted_pc/pull/469)), trophies ([#472](https://github.com/Supermedo/uncharted_pc/pull/472)), XeSS ([#476](https://github.com/Supermedo/uncharted_pc/pull/476)), surround sound ([#392](https://github.com/Supermedo/uncharted_pc/pull/392)), per-GPU shader caches ([#412](https://github.com/Supermedo/uncharted_pc/pull/412)), pipeline compiling off the GPU thread ([#389](https://github.com/Supermedo/uncharted_pc/pull/389), [#416](https://github.com/Supermedo/uncharted_pc/pull/416)), 30 FPS option ([#544](https://github.com/Supermedo/uncharted_pc/pull/544)), PS4 thread priorities ([#547](https://github.com/Supermedo/uncharted_pc/pull/547)), direct game shortcuts ([#399](https://github.com/Supermedo/uncharted_pc/pull/399)), mouse hooks for 1.02 ([#348](https://github.com/Supermedo/uncharted_pc/pull/348)), memory leak and several crash fixes, keyboard stick rotation for valves, keyboard prompts showing the bound keys for the Uncharted 2 square QTE and the Uncharted 1 valve, Uncharted 2 and 3 read-back fixes (black guns, aim blur), threaded movie scaling for older CPUs

Testing and research: adrianeira, exyled, fidoboy, kloon15, tDevis, MrBboy95, rayer23, GameBunny77 and everyone who sent logs.

Third-party: AMD FSR, NVIDIA DLSS, Intel XeSS, LibAtrac9, SDL3, FFmpeg, Dear ImGui, sirit, xbyak, Vulkan Memory Allocator, PyInstaller, Pillow, Cinzel font, Kenney Input Prompts, [ambientCG](https://ambientcg.com/a/Paper003) Paper003 texture (CC0). Licenses are in the `licenses` folder of the release.

## License

GPL v2 or later, see [LICENSE](LICENSE). Third-party components keep their own licenses.
