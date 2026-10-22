# Credits

## The port

- [SuperMedo](https://github.com/Supermedo) (Mohammed Albarghouthi) wrote the Uncharted port this
  build is based on: [Supermedo/uncharted_pc](https://github.com/Supermedo/uncharted_pc).
- [deadinside28](https://github.com/deadinside28): [bbport](https://github.com/deadinside28/bloodborne_pc),
  the native Linux port of Bloodborne that the port is built on.
- [shadPS4](https://github.com/shadps4-emu/shadPS4) and its contributors: the renderer.
- [IFreemz](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR): video decoding (Videodec2) and
  the DLSS bridge.
- Mindro: the shadPS4 Uncharted build that showed which problems to look for.

The third-party libraries and assets are listed in the README under "Credits" and in `licenses\`
in the download.

## Code in this build

| Who | What |
|---|---|
| [spacepepperoni](https://github.com/spacepepperoni) | Uncharted 2 turret and aiming blur ([#381](https://github.com/Supermedo/uncharted_pc/pull/381)), accurate GPU memory for Uncharted 2, which fixes chapter 21 and other physics problems ([#382](https://github.com/Supermedo/uncharted_pc/pull/382)) |
| [G3545](https://github.com/G3545) | Small compute-written buffers copied to guest memory, which fixes black weapons and brightness flicker ([#449](https://github.com/Supermedo/uncharted_pc/pull/449)) |
| [d4rksp4rt4n](https://github.com/d4rksp4rt4n) | Uncharted 1 crash at `0x63f5d0` on Intel 12th gen and later, guest stores below the red zone ([#403](https://github.com/Supermedo/uncharted_pc/pull/403)) |
| [Umar-Qayyum-dev](https://github.com/Umar-Qayyum-dev) | Keyboard and mouse button prompts, keyboard layouts and custom keybinding ([#420](https://github.com/Supermedo/uncharted_pc/pull/420)) |
| [kuraeow](https://github.com/kuraeow) | Idle job workers sleep, which lowers CPU load on 4 and 6 core processors ([#346](https://github.com/Supermedo/uncharted_pc/pull/346)) |
| [iannitram](https://github.com/iannitram) | The nested indirect buffer leak fix, backported from shadPS4 ([#387](https://github.com/Supermedo/uncharted_pc/pull/387)), which matches our #445; early flip IRQ, missing CE task guard, FSR 3.1 protected memory, FSR patch line endings |
| [currantsforeyes](https://github.com/currantsforeyes) | Itemzflow plain ELF support, save mount limit, flexible mapping split, EU CUSA02344 red-zone tables, window title |
| [Mevlock](https://github.com/Mevlock) | FSR 3 frame generation video memory leak, COND_EXEC condition width |
| [eduardoghi](https://github.com/eduardoghi) | Numpad bindings saved as keypad keys ([#473](https://github.com/Supermedo/uncharted_pc/pull/473)) |
| [poncho-ajmv](https://github.com/poncho-ajmv) | Launcher error reporting: crash reason, input settings and shader cache failures |
| [soni424](https://github.com/soni424) | Dark launcher title bar |
| [drakolordx7](https://github.com/drakolordx7) | Mouse hooks by pattern for 1.02, the slot-table crash, mesh raycaster copy, EOP tick matcher, address range retry, draw-prep walker, shader stutter, low VRAM, per-GPU caches, resolution and ultrawide, surround, XeSS, Xbox prompts, trophies, launcher game switches, the memory leak fix, and this fork |

## Research, testing and reports

These people found causes, tested builds or wrote up analysis that fixes in this build rely on:

- [adrianeira](https://github.com/adrianeira): notes on GPU readbacks, the slot-table crash and the EOP tick check
- [currantsforeyes](https://github.com/currantsforeyes): tested and reviewed several of the fixes in their Windows fork, and caught a shutdown stall in #397
- [iannitram](https://github.com/iannitram): reports and fixes for the flexible memory split and the save mount limit (#496, #497)
- [exyled](https://github.com/exyled): the CPU scheduling analysis in #518
- [fidoboy](https://github.com/fidoboy): an instruction emulation core for older Intel CPUs (#197)
- [kloon15](https://github.com/kloon15), [tDevis](https://github.com/tDevis), [MrBboy95](https://github.com/MrBboy95) and
  everyone else who tested builds and attached logs to their issues

If your work is in this build and you're missing here, or you'd like it described differently,
open an issue.
