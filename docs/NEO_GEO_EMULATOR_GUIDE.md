# Neo Geo Emulator in Rust — Research, Stack & Bootstrap Guide

> **Date:** 2026-09-18 · **Target:** Neo Geo AES (home console) and MVS (arcade) cartridge systems. Neo Geo CD is an optional later phase. · **Repo license:** Apache-2.0
>
> **What this is:** the research behind a from-scratch Neo Geo emulator in Rust — the decisions, the hardware you have to model, the crates to use, how to structure the code, and a milestone plan with a test for every step. Crate versions, APIs and hardware facts were checked on the date above against the sources in [§13](#13-references).
>
> **Verified code:** five fragments in this document were compiled and cross-checked during the research: the L0 shrink-table generator (bit-exact with the real `000-lo.lo`, CRC32 `5A86CFF2`), the colour converter (identical to Geolith for all 65,536 inputs), the sprite and fix tile decoders (identical to Geolith, including horizontal flip), and the sprite line renderer (identical to a literal port of Geolith's on 30,720 randomly generated lines). All other code is a sketch to start from, not tested code.

## Contents

1. [Executive summary](#1-executive-summary)
2. [Is the `m68000` crate the right choice?](#2-is-the-m68000-crate-the-right-choice)
3. [The machine at a glance](#3-the-machine-at-a-glance)
4. [Hardware reference for emulator authors](#4-hardware-reference-for-emulator-authors)
5. [The Rust stack](#5-the-rust-stack)
6. [Architecture](#6-architecture)
7. [Scaffolding, step by step](#7-scaffolding-step-by-step)
8. [Roadmap and milestones](#8-roadmap-and-milestones)
9. [Testing and debugging strategy](#9-testing-and-debugging-strategy)
10. [Tools checklist](#10-tools-checklist)
11. [ROMs, BIOS and legal hygiene](#11-roms-bios-and-legal-hygiene)
12. [Gotchas checklist](#12-gotchas-checklist)
13. [References](#13-references)

---

## 1. Executive summary

The Neo Geo is one of the friendlier arcade machines to emulate from scratch. It has two well-documented CPUs (a 68000 running the game, a Z80 running the sound driver), one sound chip (Yamaha YM2610), and a video chip with **no background layers at all**: everything on screen is either a sprite or a single text-like "fix" layer. The two CPUs talk through a one-byte mailbox in each direction.

The hard parts are the sprite engine (sprite chains, shrinking, the 96-sprites-per-line limit), interrupt timing (VBlank plus a programmable, pixel-clocked timer used for raster effects), the YM2610's four sound engines, and the cartridge boards and protection chips of the late (1999+) games.

The most useful finding of this research: the best open reference, **Geolith**, is small, readable and BSD-3-Clause. It is about 5,500 lines of C for the cartridge systems, not counting its CPU and sound cores. It runs 100% of the commercial AES/MVS library, and it does so with **instruction-level** CPUs kept closely in step with video and sound. That tells you how much accuracy is enough, and it gives you code you are allowed to port, with attribution.

There is no mature Rust Neo Geo emulator today (a GitHub search finds only a couple of tiny work-in-progress repos), so there's no prior Rust art to lean on — but also nothing to compete with.

### Decisions

| Area | Recommendation | Why | Fallback / later |
|---|---|---|---|
| Target system | AES first, then MVS, then Universe BIOS | AES needs fewer devices (no RTC, backup RAM, coins, SFIX/SM1) | Neo Geo CD after everything else |
| ROM input | `.neo` files (TerraOnion format), converted from MAME sets with [Lithogen](https://github.com/carmiker/lithogen) | One file per game, already decrypted, standard layout. Used by Geolith, MiSTer and NeoSD | Loading MAME `.zip` sets means re-implementing every set's load layout and decryption |
| 68000 | [`m68k`](https://crates.io/crates/m68k) `=0.13.0` (MIT) behind your own `Cpu68k` trait | Passes all 261,894 SingleStepTests cases, including cycle and bus-access totals. Host-friendly API: autovector IACK, per-instruction stepping, `serde` | Your own core later, as a swap-in |
| Z80 | Write your own — the easiest CPU and a good learning target. Bootstrap with [`z80`](https://crates.io/crates/z80) 2.0 (MIT) | `z80` is a Rust port of jgz80, the Z80 core Geolith ships. Passes ZEXALL, supports all interrupt modes | `iz80` (BSD-3) panics in IM 0/IM 2 |
| YM2610 | Start with [`ymfm-sys`](https://crates.io/crates/ymfm-sys) (BSD-3, binds Aaron Giles' ymfm). Then port ymfm's OPNB/ADPCM/SSG to pure Rust | ymfm is MAME's FM core. Geolith uses a C port of it (~4,700 lines) | Write from datasheets |
| Scheduling | One 24 MHz master-clock counter. Step the 68000 one instruction at a time; catch up the LSPC, timer, Z80 and YM2610 after each step | The Geolith model, proven on the full library | Event-driven time slices, if profiling ever asks for it |
| Video | Scanline renderer: 320×224 visible, line buffers, pre-converted palette | Games change palettes, the fix layer and sprites between lines | Sub-line timing only if a game needs it |
| Frontend | `eframe`/`egui` desktop app with debugger windows, `cpal` audio, `gilrs` gamepads, plus a headless CLI | You will live in the debugger for months | `winit` + `pixels` player, SDL3, or a libretro core |
| Save states | `serde` + `postcard` with a versioned header | `bincode` is abandoned (3.0.0 is a tombstone release) | `rkyv` |
| Test content | SingleStepTests (68000, Z80), ZEXALL, ngdevkit's example ROMs + its open-source BIOS, the diagnostics BIOS, MAME trace diffs, golden frames | All legal, all automatable | Commercial games for compatibility passes |

### What to do in the first few weeks

1. Scaffold the workspace ([§7](#7-scaffolding-step-by-step)). Build a headless runner that loads a BIOS, resets the 68000 through `m68k`, and writes an instruction trace.
2. Boot the BIOS far enough that it waits for VBlank. Implement IRQ1 and `REG_IRQACK`, and diff your trace against MAME's ([§9.3](#93-trace-diffing-against-mame)).
3. Add palettes and the fix layer. First pixels: ngdevkit's `01-helloworld` and the diagnostics BIOS menu.
4. Add sprites. The BIOS eye-catcher and ngdevkit's `02-sprite`, `03-sprite-animation` and `05-scrolling` should render.
5. Add `.neo` loading, inputs and P-ROM bank switching. Early games become playable, without sound.

---

## 2. Is the `m68000` crate the right choice?

**Short answer: not any more.** Use [`m68k`](https://crates.io/crates/m68k) behind a small trait of your own. The trait keeps you free to swap in your own 68000 core later.

| | [`m68000`](https://crates.io/crates/m68000) (Stovent) | [`m68k`](https://crates.io/crates/m68k) ([benletchford/m68k-rs](https://github.com/benletchford/m68k-rs)) |
|---|---|---|
| Latest | 0.2.3, June 2025 | 0.13.0, 7 Sep 2026 |
| License | MPL-2.0 | MIT |
| Toolchain | **Nightly only** (`btree_extract_if`, `bigint_helper_methods`) | Stable Rust ≥ 1.93, edition 2024 (you have 1.94.1) |
| Validation | None published. Timings come from Motorola's manual | All 261,894 [SingleStepTests](https://github.com/SingleStepTests/m68000) 68000 cases match, including cycle and bus-access totals. Also runs Musashi's test binaries |
| Bus trait | `MemoryAccess`: `get_byte`… return `Option` (for bus errors) | `AddressBus`: six required read/write methods. Optional hooks: `sync(cpu_clocks)` for bus-exact timing, `interrupt_acknowledge(level)` (defaults to **autovector**), `take_boundary_request()`, fallible `try_*` |
| Execution | `interpreter(&mut mem)` | `execute(bus, cycles)`, `step`, `run_for_cycles[_with_hook]`, `run_batch` (+ optional Cranelift JIT) |
| Interrupts | `exception(Vector)` | `set_irq(level)` plus the IACK callback |
| Extras | Disassembler, assembler, C FFI | Disassembler (`m68k::dasm`), `serde` feature for save states |
| Written for | CeDImu (Philips CD-i, SCC68070) | Generic 68000–68060 |
| Risk | Slow cadence; nightly | Young: repo created Jan 2026, 65 releases, 12 of them breaking. **Pin `=0.13.0`** and upgrade on purpose |

Why `m68k` fits the Neo Geo:

- Cartridge systems use **autovectored** interrupts on levels 1–3. The crate's default `interrupt_acknowledge` already does exactly that.
- Instruction-level accuracy is enough. Geolith uses Musashi (instruction-level) and runs the whole commercial library. `m68k` gives you that, plus bus-cycle accounting through `AddressBus::sync` if you ever need it.
- A nightly-only core would drag the whole project onto nightly: CI, rust-analyzer, every dependency. Not worth it.

Other options, and why not now:

- **`r68k`** 0.2.2 (Musashi port, MIT): older design, no host hooks between instructions.
- **jgenesis `m68000-emu`**: excellent, with its own SingleStepTests runner. But it is GPL-3.0 and not on crates.io, so for an Apache-2.0 repo it is read-only reference.
- **Musashi (C, MIT) or Moira (C++, cycle-exact, MIT) over FFI**: works, but adds build and FFI friction for no benefit today.
- **Your own core**: the biggest single task in the project (a 65,536-entry decode table, 14 addressing modes, exception frames including address errors, timing tables). SingleStepTests make it verifiable. Do it as a later swap-in if learning the CPU is part of the goal. Don't start there.

The trait that keeps the choice reversible:

```rust
// crates/neogeo-core/src/cpu68k.rs
use crate::bus::MainBus;

/// Everything the machine needs from a 68000 core.
pub trait Cpu68k {
    fn reset(&mut self, bus: &mut MainBus);
    /// Run one instruction (plus any interrupt entry). Returns 68000 clock cycles.
    fn step(&mut self, bus: &mut MainBus) -> u32;
    /// Highest pending interrupt level, 0 = none.
    fn set_irq_level(&mut self, level: u8);
    fn pc(&self) -> u32;
}

pub struct CrateM68k(m68k::CpuCore);

impl CrateM68k {
    pub fn new() -> Self {
        let mut cpu = m68k::CpuCore::new();
        cpu.set_cpu_type(m68k::CpuType::M68000);
        Self(cpu)
    }
}

impl Cpu68k for CrateM68k {
    fn reset(&mut self, bus: &mut MainBus) { self.0.reset(bus) }
    // `execute` never splits an instruction, so a budget of 1 runs exactly one.
    // It takes TRAP / Line-A / Line-F as real exceptions, which is what hardware
    // emulation wants. Same contract as Musashi's `m68k_execute(1)` in Geolith's main loop.
    fn step(&mut self, bus: &mut MainBus) -> u32 { self.0.execute(bus, 1) as u32 }
    fn set_irq_level(&mut self, level: u8) { self.0.set_irq(level) }
    fn pc(&self) -> u32 { self.0.pc }
}
```

`MainBus` implements `m68k::AddressBus` ([§6.5](#65-bus-decoding)). Note that `run_for_cycles*` *surfaces* traps to the host instead of taking them (you would then call `take_trap_exception` and friends yourself), which is why `execute` is the simpler fit here.

---

## 3. The machine at a glance

```
                     master clock: 24.000 MHz (MVS) / 24.167829 MHz (AES)
          ┌──────────────┬──────────────┬───────────────┬──────────────┐
          │ ÷2 → 12 MHz  │ ÷4 → 6 MHz   │ ÷3 → 8 MHz    │ ÷6 → 4 MHz   │
          ▼              ▼ pixel clock  ▼               ▼
   ┌─────────────┐  ┌───────────────┐  ┌────────────┐  ┌────────────┐
   │   68000     │  │ LSPC2-A2 +    │  │  YM2610    │  │    Z80     │
   │  main CPU   │◄►│ NEO-B1 (video)│  │ FM · SSG · │◄►│ sound CPU  │
   │             │  │ VRAM 68 KiB   │  │ ADPCM-A/B  │  │ 2 KiB RAM  │
   └──────┬──────┘  └───────┬───────┘  └─────┬──────┘  └─────┬──────┘
          │ P ROM, BIOS,    │ C ROM (sprites) │ V ROMs        │ M1 ROM
          │ 64 KiB work RAM │ S/SFIX (fix)    │ (samples)     │ (banked by NEO-ZMC)
          │ palette RAM     │ L0 (shrink)     │               │
          └── REG_SOUND $320000: 1-byte latch each way, a write fires the Z80 NMI ──┘
```

| Item | Value |
|---|---|
| CPUs | 68000 @ 12 MHz (game), Z80 @ 4 MHz (sound driver only) |
| Sound | YM2610 @ 8 MHz: 4 FM channels, 3 SSG square channels + noise, 6 ADPCM-A channels (fixed ≈18.5 kHz), 1 ADPCM-B channel (variable rate) |
| Resolution | 320×224 visible (SNK's safe area: 304 px / 38×28 fix tiles). 384×264 total per frame |
| Frame rate | 59.1856 Hz (MVS), 59.5998 Hz (AES, NTSC) |
| Sprites | 381 per frame (sprite numbers 1–381), 96 per line. Each is one 16 px column of up to 32 tiles (512 px); sprites can be chained sideways. Shrink only, no zoom-in |
| Fix layer | 40×32 map of 8×8 tiles, 4 bpp, first 16 palettes only, always on top |
| Colours | 2 banks × 256 palettes × 16 colours. 16-bit colour: 5 bits per channel plus a shared "dark" bit. Up to 3,840 colours on screen |
| Work RAM | 64 KiB (68000), 2 KiB (Z80) |
| VRAM | 64 KiB + 4 KiB, word-addressed, reachable only through three LSPC registers |
| Palette RAM | 2 × 8 KiB |
| MVS extras | 64 KiB battery-backed RAM, uPD4990A real-time clock, coin inputs, DIP switches, SFIX/SM1 board ROMs |
| Cartridge | P (68000 program), S (fix tiles), M1 (Z80 program, up to 4 MiB), V (ADPCM samples, 24-bit addresses = 16 MiB per ADPCM bus), C (sprite tiles, up to 2²⁰ tiles = 128 MiB) |

---

## 4. Hardware reference for emulator authors

Sources: the [NeoGeo Development Wiki](https://wiki.neogeodev.org), Geolith's source, and MAME's `src/mame/snk/neogeo*.cpp` (these driver files are BSD-3-Clause). Where they disagree, it is noted. For timing questions none of them settle, the ground truth is Furrtek's chip-level Verilog in the [MiSTer core](https://github.com/MiSTer-devel/NeoGeo_MiSTer) (`rtl/video/lspc2_a2.v`, `lspc_timer.v`, `irq.v`, `rtl/io/c1_wait.v`, `watchdog.v`…). It is GPL-2.0: read it, don't copy it into this Apache-2.0 repo.

### 4.1 Clocks and video timing

| Clock | MVS | AES (NTSC) | Derivation |
|---|---|---|---|
| Master (`mclk`) | 24.000000 MHz | 24.167829 MHz | Crystal |
| 68000 | 12 MHz | ≈12.084 MHz | mclk ÷ 2 |
| YM2610 | 8 MHz | ≈8.056 MHz | mclk ÷ 3 |
| Pixel clock | 6 MHz | ≈6.042 MHz | mclk ÷ 4 |
| Z80 | 4 MHz | ≈4.028 MHz | mclk ÷ 6 |
| Line rate | 15.625 kHz | ≈15.734 kHz | 384 pixels per line |
| Frame rate | **59.1856 Hz** | **59.5998 Hz** | 264 lines per frame |
| YM2610 output rate | 55,555.6 Hz | ≈55,943.5 Hz | YM clock ÷ 144 |

PAL AES boards run 312 lines (50.43 Hz). Geolith doesn't support PAL either; ignore it.

Everything is an integer number of master cycles, so keep all timing in integer `mclk`:

| Unit | Per pixel | Per line | Per frame |
|---|---|---|---|
| mclk | 4 | 1,536 | 405,504 |
| 68000 cycles | 2 | 768 | 202,752 |
| Z80 cycles | ⅔ | 256 | 67,584 |
| YM2610 output samples | — | ≈3.56 | ≈938.7 |

**Horizontal** (384 px): 28 px sync + 28 px back porch + **320 px active** + 8 px front porch.

**Vertical** (264 lines). Bits 15–7 of `REG_LSPCMODE` expose the raster line counter, which runs from `$0F8` to `$1FF`:

| Counter | Lines | Region |
|---|---|---|
| `$0F8–$0FF` | 8 | Vertical sync |
| `$100–$10F` | 16 | Top border (blanked on NTSC) |
| `$110–$1EF` | 224 | **Active display** |
| `$1F0–$1FF` | 16 | Bottom border (blanked on NTSC) |

**Events your scheduler must produce:**

| Event | When | Notes |
|---|---|---|
| Render a line | Once per line | Hardware fetches a line's sprites during the previous line into double line buffers. Palette and fix changes show up at once |
| VBlank → IRQ1 pending | End of active display. MAME: 58 mclk into raster line `$1F0`. Geolith: CPU cycle 29 of its line `$1F1` | The two references sit about one line apart — one sign that sub-line timing is still best effort (below) |
| Timer reload (mode bit 6) | Start of vertical blanking, once per frame | MAME: 1,146 mclk after the VBlank start position |
| Timer countdown | Every pixel (4 mclk = 2 CPU cycles) | IRQ2 when it reaches 0 ([§4.4](#44-interrupts)) |
| Auto-animation tick | Once per frame | [§4.6.5](#465-auto-animation) |
| Watchdog | Counts continuously | Resets the machine after **3,244,030 mclk** (≈ 8 frames) without a write to `$300001`. MAME and Geolith use the same constant |

Exact sub-line positions (for example when the line counter increments) are "best effort" in both Geolith and MAME. Start with Geolith's placements: within a 768-cycle line, the VBlank IRQ and auto-animation tick happen at CPU cycle 29, the line render (and VBlank timer reload) at 573, and the line counter increment at 712. Refine only when a game proves them wrong.

### 4.2 68000 memory map (cartridge systems)

| Range | Size | Wait states | Contents |
|---|---|---|---|
| `$000000–$00007F` | 128 B | — | **Vector table**: the BIOS's (after reset) or the cartridge's (`REG_SWPBIOS` / `REG_SWPROM`) |
| `$000000–$0FFFFF` | 1 MiB | 0–1 (cart `ROMWAIT`) | P ROM, fixed first bank |
| `$100000–$10FFFF` | 64 KiB | 0 | Work RAM (`$10F300`+ reserved for the BIOS). Mirrored up to `$1FFFFF` |
| `$200000–$2FFFFF` | 1 MiB | 0–3 (cart `PWAIT`) | P ROM switchable bank, plus cartridge special chips (bank register at `$2FFFF0`, protection) |
| `$300000–$3FFFFF` | — | 0 | I/O registers ([§4.3](#43-io-registers)) |
| `$400000–$401FFF` | 8 KiB | 0 | Palette RAM (active bank). Mirrored up to `$7FFFFF` |
| `$800000–$BFFFFF` | — | 2 | Memory card (8-bit, odd addresses) |
| `$C00000–$C1FFFF` | 128 KiB | 0 | BIOS ("system ROM"). Mirrored up to `$CFFFFF` |
| `$D00000–$D0FFFF` | 64 KiB | 0 | Backup RAM (**MVS only**). Mirrored up to `$DFFFFF`, write-protected by `REG_SRAMLOCK` |
| `$E00000–$FFFFFF` | — | — | Unmapped |

Implementation notes:

- The 68000 has a 24-bit address bus: mask every address with `0xFF_FFFF`.
- Unmapped reads: return `0xFF` / `0xFFFF` (Geolith's choice) as an open-bus approximation, and log them.
- Wait states are real but small. Geolith and MAME don't model them. Keep the `AddressBus::sync` hook in mind and ignore it at first.

### 4.3 I/O registers

**Mirrors.** Copy MAME's address map:

| Register block | Base | MAME mirror mask |
|---|---|---|
| P1 / DIPs / watchdog | `$300000` | `$01FFFE` (repeats every 2 bytes up to `$31FFFF`) |
| `REG_SYSTYPE` | `$300081` | `$01FF7E` (odd addresses with bit 7 set) |
| `REG_SOUND` / `REG_STATUS_A` | `$320000` | `$01FFFE` |
| `REG_P2CNT` | `$340000` | `$01FFFE` |
| `REG_STATUS_B` (read) / output registers (write) | `$380000` | reads `$01FFFE`; writes to `$380000–$3800FF` mirror with `$01FF00` |
| System latches | `$3A0000–$3A001F` | `$01FFE0` |
| LSPC registers | `$3C0000` | reads: 4 registers repeating every 8 bytes (`$01FFF8`); writes: 8 registers every 16 bytes (`$01FFF0`) |
| Palette RAM | `$400000–$401FFF` | `$3FE000` |

**Inputs and miscellaneous** (switch inputs are active low: 0 = pressed):

| Address | Name | Read | Write |
|---|---|---|---|
| `$300000` | `REG_P1CNT` | P1 joystick: bit 0 Up, 1 Down, 2 Left, 3 Right, 4 A, 5 B, 6 C, 7 D | — |
| `$300001` | `REG_DIPSW` | Hardware DIP switches (MVS) | **Kick the watchdog** (any value) |
| `$300081` | `REG_SYSTYPE` | Bit 7 test button; bit 6: 0 = 1/2-slot, 1 = 4/6-slot board | — |
| `$320000` | `REG_SOUND` | Reply byte from the Z80 | Command byte to the Z80 (fires the Z80 NMI if enabled) |
| `$320001` | `REG_STATUS_A` | Bit 0 coin 1, 1 coin 2, 2 service, 3 coin 3, 4 coin 4, 5 4/6-slot, **6 RTC time pulse, 7 RTC data** | — |
| `$340000` | `REG_P2CNT` | P2 joystick (same bits as P1) | — |
| `$380000` | `REG_STATUS_B` | Bit 0 P1 Start, 1 P1 Select, 2 P2 Start, 3 P2 Select, 4–5 memory card inserted (`00` = inserted), 6 card write-protected, **7: 0 = AES, 1 = MVS** | — |
| `$380001` | `REG_POUTPUT` | — | Joypad port outputs |
| `$380011` | `REG_CRDBANK` | — | Memory card bank (3 bits) |
| `$380021` | `REG_SLOT` | — | MVS slot select (on AES a mirror of `REG_POUTPUT`) |
| `$380031` / `$380041` | `REG_LEDLATCHES` / `REG_LEDDATA` | — | MVS LED displays and marquee |
| `$380051` | `REG_RTCCTRL` | — | uPD4990A: bit 0 DATA IN, bit 1 CLK, bit 2 STROBE |
| `$380061–$3800E7` | Coin counters and lockouts | — | Any value |

**System latches** (`$3A0001–$3A001F`, odd bytes; the data written is ignored). They come in pairs, and **address bit 4 is the value latched**:

| Address (bit 4 = 0) | Effect | Address (bit 4 = 1) | Effect |
|---|---|---|---|
| `$3A0001` `REG_NOSHADOW` | Normal video | `$3A0011` `REG_SHADOW` | Darkened video |
| `$3A0003` `REG_SWPBIOS` | BIOS vector table at `$000000` | `$3A0013` `REG_SWPROM` | Cartridge vector table |
| `$3A0005` `REG_CRDUNLOCK1` | Card writes enabled | `$3A0015` `REG_CRDLOCK1` | Card writes disabled |
| `$3A0007` `REG_CRDLOCK2` | Card writes disabled | `$3A0017` `REG_CRDUNLOCK2` | Card writes enabled |
| `$3A0009` `REG_CRDREGSEL` | Card "register select" | `$3A0019` `REG_CRDNORMAL` | Card normal |
| `$3A000B` `REG_BRDFIX` | Board SFIX + SM1 (MVS) | `$3A001B` `REG_CRTFIX` | Cartridge S ROM + M1 |
| `$3A000D` `REG_SRAMLOCK` | Backup RAM write-protected | `$3A001D` `REG_SRAMUNLOCK` | Backup RAM writable |
| `$3A000F` `REG_PALBANK0` | Palette bank 0 | `$3A001F` `REG_PALBANK1` | Palette bank 1 |

MAME models this as a 74HC259 addressable latch, which is also the cleanest code: `latch[(a >> 1) & 7] = (a >> 4) & 1`. Geolith labels the two palette-bank addresses the other way round. That makes no visible difference, because the two banks are symmetric.

**LSPC video registers** (`$3C0000–$3C000E`). A byte write to an even address stores the byte in both halves of the word:

| Address | Name | Read | Write |
|---|---|---|---|
| `$3C0000` | `REG_VRAMADDR` | VRAM word at the current address | Set VRAM address |
| `$3C0002` | `REG_VRAMRW` | VRAM word at the current address | Write VRAM, then `addr += mod`, wrapping inside the current zone |
| `$3C0004` | `REG_VRAMMOD` | Modulo | Set the signed 16-bit modulo |
| `$3C0006` | `REG_LSPCMODE` | Bits 15–7 raster line counter; bit 3: 1 = 50 Hz; bits 2–0 auto-animation counter | Bits 15–8 auto-animation speed; bit 7 timer reload at 0; bit 6 timer reload at frame start; bit 5 timer reload on `REG_TIMERLOW` write; bit 4 timer IRQ enable; bit 3 disable auto-animation |
| `$3C0008` | `REG_TIMERHIGH` | (mirror of `$3C0000`) | Timer reload value, bits 31–16 |
| `$3C000A` | `REG_TIMERLOW` | (mirror of `$3C0002`) | Timer reload value, bits 15–0 |
| `$3C000C` | `REG_IRQACK` | (mirror of `$3C0004`) | Acknowledge: bit 2 VBlank, bit 1 timer, bit 0 IRQ3 |
| `$3C000E` | `REG_TIMERSTOP` | (mirror of `$3C0006`) | Bit 0: stop the timer during the PAL border lines |

### 4.4 Interrupts

**68000 (cartridge systems): three autovectored levels.**

| Level | Vector address | Source | Set | Cleared by |
|---|---|---|---|---|
| 1 | `$64` | VBlank | Entering the bottom border, every frame | `REG_IRQACK` bit 2 |
| 2 | `$68` | LSPC timer (the "raster" IRQ) | Timer reaches 0 while enabled (`REG_LSPCMODE` bit 4) | `REG_IRQACK` bit 1 |
| 3 | `$6C` | "Pending after reset" | Power-on / cold boot | `REG_IRQACK` bit 0 |

- The pending flags are latches. The CPU's interrupt-acknowledge cycle does **not** clear them; only `REG_IRQACK` does. Present the highest pending level to the CPU after every step.
- Neo Geo CD swaps levels 1 and 2 and uses vectored interrupts. Ignore that until the CD phase.

**Timer (IRQ2).** A 32-bit down-counter clocked by the 6 MHz pixel clock (one tick every 2 CPU cycles), reloaded from `REG_TIMERHIGH:REG_TIMERLOW`. It reloads immediately on a `REG_TIMERLOW` write (mode bit 5), at the start of each frame (bit 6), and/or whenever it reaches 0 (bit 7). A reload value below 5 floods the CPU with interrupts. Games use it for raster effects: Neo Turf Masters reloads it with 767 so it fires every 2 lines to draw the ground in perspective. Riding Hero's road, Viewpoint's logo and Sengoku 2's intro use it too.

**Z80:**

| Line | Vector | Source | Enable / acknowledge |
|---|---|---|---|
| INT (IM 1) | `$0038` | YM2610 timers A/B | Acknowledged through the YM2610's registers |
| NMI | `$0066` | 68000 writes `REG_SOUND` | Enabled by a write to port `$08`, disabled by a write to port `$18`, **disabled after reset**. Reading port `$00` acknowledges it |

The Z80 powers up in IM 0. A Z80 core must handle IM 0 with `$FF` on the data bus (which behaves like `RST 38h`), even though drivers normally switch to IM 1.

### 4.5 Reset, BIOS hand-off and the cartridge header

1. At reset, the BIOS vector table is mapped at `$000000`, so SSP and PC come from the BIOS image (`$C00000`). IRQ3 is pending.
2. The BIOS initialises the hardware and runs its tests (MVS). It talks to the Z80: command `$01` must be answered with `$01`, or the MVS BIOS shows **"Z80 ERROR"** and locks up. It checks the cartridge header and plays the eye-catcher (the SNK logo, drawn with sprites).
3. Before starting the game, it selects the cartridge vector table (`REG_SWPROM`) and the cartridge S and M1 ROMs (`REG_CRTFIX`), then jumps through the header.

The header (in the P ROM, 68000 byte order):

| Offset | Size | Contents |
|---|---|---|
| `$100` | 7 | `"NEO-GEO"` |
| `$107` | 1 | System version (0 for cartridges) |
| `$108` | 2 | NGH number (BCD): the game's ID. `.neo` board quirks key on it |
| `$10A` | 4 | P ROM size |
| `$10E` | 4 | Pointer to the backup-RAM block in work RAM (debug DIPs first) |
| `$112` | 2 | Size of that block (≤ 4096) |
| `$114` | 1 | Eye-catcher: 0 = drawn by the BIOS, 1 = by the game, 2 = none |
| `$115` | 1 | Sprite bank (upper 8 tile-number bits) for the BIOS eye-catcher |
| `$116` / `$11A` / `$11E` | 4 each | Soft-DIP layouts: Japan / US / Europe |
| `$122` | 6 | `JMP USER` (entry point) |
| `$128` | 6 | `JMP PLAYER_START` |
| `$12E` | 6 | `JMP DEMO_END` |
| `$134` | 6 | `JMP COIN_SOUND` |
| `$182` | 4 | Pointer to the security code (second cartridge check) |

The BIOS image itself carries its type at offset `$400` (0 = AES, `$80` = MVS) and its region at `$401` (0 Japan, 1 US, 2 Europe).

### 4.6 Video: LSPC, VRAM, sprites, fix layer, palettes

#### 4.6.1 VRAM

VRAM holds **no graphics**: only sprite attributes, the fix map and the sprite lists. Graphics come straight from the C and S ROMs. Every VRAM address points to a 16-bit word.

| Word range | Zone | Contents |
|---|---|---|
| `$0000–$6FFF` | Lower (slow) | **SCB1**: sprite tile maps, 64 words per sprite |
| `$7000–$74FF` | Lower | **Fix map**, 40 columns × 32 rows |
| `$7500–$7FFF` | Lower | Free, or fix bank-switching data (NEO-CMC games) |
| `$8000–$81FF` | Upper (fast) | **SCB2**: shrink coefficients |
| `$8200–$83FF` | Upper | **SCB3**: Y position, sticky bit, height |
| `$8400–$85FF` | Upper | **SCB4**: X position |
| `$8600–$867F` / `$8680–$86FF` | Upper | Sprite lists for even / odd lines (written by the LSPC itself) |
| `$8700–$87FF` | Upper | Unused |

The modulo wraps **inside the current zone**: `addr = ((addr + mod) & 0x7FFF) | (addr & 0x8000)`. Software has to set the address directly to cross zones. Real hardware needs 12–16 CPU cycles between VRAM accesses. Emulators ignore that, and it is why overclocked consoles glitch.

#### 4.6.2 Sprite control blocks

| Block | Word | Bits |
|---|---|---|
| SCB1, even word (per tile) | Tile number | 15–0: tile number bits 15–0 |
| SCB1, odd word (per tile) | Attributes | 15–8 palette · 7–4 tile number bits 19–16 · 3 auto-animate ×8 · 2 auto-animate ×4 · 1 V-flip · 0 H-flip |
| SCB2 (per sprite) | Shrink | 11–8 horizontal shrink (`$F` = full width) · 7–0 vertical shrink (`$FF` = full height) |
| SCB3 (per sprite) | Vertical | 15–7 Y · 6 **sticky** (chain to previous sprite) · 5–0 height in tiles (0 = off, 1–32; 33 = 32 tiles with looping borders when shrunk) |
| SCB4 (per sprite) | Horizontal | 15–7 X |

Rules:

- The LSPC walks sprites **1 to 381**. Sprite 0 is never drawn. Higher numbers draw **on top**. The fix layer is above all sprites. Behind everything is the **backdrop colour**: the last entry of the active palette bank (`$401FFE`).
- **96 sprites per line** at most: the first 96, in index order, that touch the line. Some games show the limit (Samurai Shodown IV's intro).
- **Sticky bit**: a chained sprite ignores its own Y, height and vertical shrink and inherits them from the chain leader. Its X is the previous sprite's X plus that sprite's drawn width (`hshrink + 1`). Horizontal shrink does *not* propagate along the chain.
- **Y**: the sprite's first line is `(496 − Y) mod 512` counted from the first visible line — equivalently `0x200 − Y` counted from the start of the top border (`$100`). X and Y wrap at 512. Pixels with X ≥ 320 are off-screen.

#### 4.6.3 Sprite line rendering

Per-line algorithm, following Geolith's `geo_lspc_sprcalc`. `line` is 0–511, counted from the start of the top border (raster `$100`), so the visible area is lines 16–239. This function was fuzzed against a literal Rust port of Geolith's routine: 60 random VRAM states (sticky chains, height-33 sprites, auto-animation, flips, both shrinks, the 96-sprite limit) × 512 lines, with identical output on every line. The one intentional difference: it wraps tile numbers with `%` instead of Geolith's power-of-two mask, which only matters for C ROM sizes that aren't a power of two.

```rust
/// Renders the sprite layer of one line into `buf`: absolute palette-RAM index
/// (bank * 4096 + palette * 16 + colour), or 0 for transparent. Colour 0 is always
/// transparent, so 0 is free to mean "nothing here".
fn sprite_line(vram: &[u16], l0: &[u8], c_rom: &[u8], aa_counter: u32, aa_disabled: bool,
               palette_bank_base: u16, line: u32, buf: &mut [u16; 320]) {
    let tile_count = (c_rom.len() / 128) as u32;
    let (mut x, mut y, mut height, mut vshrink, mut hshrink) = (0u32, 0u32, 0u32, 0xFFu32, 0xFu32);
    let mut drawn = 0;
    for s in 1..=381usize {
        let scb2 = vram[0x8000 + s] as u32;
        let scb3 = vram[0x8200 + s] as u32;
        if scb3 & 0x40 != 0 {
            x = (x + hshrink + 1) & 0x1FF;          // sticky: glue to the previous sprite
        } else {
            x = (vram[0x8400 + s] as u32 >> 7) & 0x1FF;
            y = (scb3 >> 7) & 0x1FF;
            height = scb3 & 0x3F;
            vshrink = scb2 & 0xFF;
        }
        hshrink = (scb2 >> 8) & 0xF;

        // Row inside the sprite's 512-line window.
        let row = line.wrapping_sub(0x200 - y) & 0x1FF;
        if height == 0 || row >= height * 16 { continue; }   // height 33 is always "inside"
        if drawn == 96 { break; }                             // per-line hardware limit
        drawn += 1;

        // Vertical shrink through the L0 table (§4.6.4).
        let mut invert = row > 0xFF;
        let mut z = if invert { (row & 0xFF) ^ 0xFF } else { row & 0xFF };
        if height == 33 {
            let span = (vshrink + 1) << 1;
            z %= span;
            if z > vshrink { z = span - 1 - z; invert = !invert; }
        }
        let mut v = l0[((vshrink << 8) | z) as usize] as u32;
        if invert { v ^= 0x1FF; }
        let (slot, tile_row) = ((v >> 4) & 0x1F, v & 0xF);

        // Tile map entry for that slot.
        let map = (s << 6) + (slot as usize) * 2;
        let attr = vram[map + 1] as u32;
        let mut tile = vram[map] as u32 | ((attr & 0xF0) << 12);
        if !aa_disabled {
            match (attr >> 2) & 3 {
                1 => tile = (tile & !3) | (aa_counter & 3),       // 4-frame loop
                2 | 3 => tile = (tile & !7) | (aa_counter & 7),   // 8-frame loop
                _ => {}
            }
        }
        let tile = tile % tile_count;
        let ty = if attr & 2 != 0 { 15 - tile_row } else { tile_row };
        let hflip = attr & 1 != 0;
        let pal_base = palette_bank_base + ((attr >> 8) as u16) * 16;

        // Horizontal shrink: draw only the pixels whose bit is set in the pattern.
        let mask = HSHRINK[hshrink as usize];
        let mut dx = 0;
        for p in 0..16u32 {
            if mask & (1 << p) == 0 { continue; }
            let px = if hflip { 15 - p } else { p };
            let color = sprite_pixel(c_rom, tile, px, ty);
            let sx = (x + dx) & 0x1FF;
            if color != 0 && sx < 320 { buf[sx as usize] = pal_base + color as u16; }
            dx += 1;
        }
    }
}
```

This renders a line's sprites on the spot. Hardware (and Geolith) evaluate sprites one to two lines ahead into double line buffers. Start simple; move sprite evaluation earlier if a game's mid-frame sprite changes land one line off.

#### 4.6.4 Shrinking

**Vertical**, via the L0 ROM: 256 tables of 256 bytes, one table per vertical-shrink value. For sprite rows 0–255 the table index is the row. Each byte holds the tile slot in its high nibble and the row inside that tile in its low nibble. For rows 256–511 the table is read backwards (`index ^ 0xFF`) and the result is XORed with `0x1FF`, giving slots 16–31 with mirrored rows. The dump is 128 KiB, but only the first 64 KiB is used (the two halves are identical). If a sprite's window is taller than its shrunk graphics, the rest repeats the last line (the L0 table's `$FF` padding).

**You don't need the `000-lo.lo` file.** ngdevkit's open-source BIOS generates the table with a short script ([`zoom-rom.py`](https://github.com/dciabrin/ngdevkit/blob/master/nullbios/zoom-rom.py)). A Rust port was checked during this research: its output is 131,072 bytes with **CRC32 `5A86CFF2`, identical to the real dump**.

```rust
/// L0 vertical-shrink table, bit-exact with 000-lo.lo (CRC32 5A86CFF2).
/// Algorithm from ngdevkit's nullbios/zoom-rom.py.
pub fn generate_l0() -> Vec<u8> {
    let mut out = Vec::with_capacity(0x2_0000);
    for _ in 0..2 {
        let mut on = [false; 256];
        for i in [8i32, 12, 10, 14, 9, 13, 11, 15] {
            for j in [0i32, 16, -8, 8] {
                for k in [0i32, -128, 64, -64, 32, -96, 96, -32] {
                    on[(128 + i + j + k) as usize] = true;
                    let mut level = [0xFFu8; 256];
                    for (n, v) in (0..256).filter(|&v| on[v]).enumerate() {
                        level[n] = v as u8;
                    }
                    out.extend_from_slice(&level);
                }
            }
        }
    }
    out
}
```

**Horizontal**: a fixed pattern inside the LSPC, no ROM. Bit `p` set means pixel `p` of the tile's 16 is drawn; shrink value `n` draws `n + 1` pixels:

```rust
/// Horizontal shrink patterns (MAME / NeoGeo wiki). Index = SCB2 bits 11-8.
pub const HSHRINK: [u16; 16] = [
    0x0100, 0x0110, 0x1110, 0x1114, 0x5114, 0x5154, 0x5554, 0x5555,
    0x5755, 0x575D, 0xD75D, 0xD7DD, 0xF7DD, 0xF7DF, 0xFFDF, 0xFFFF,
];
```

#### 4.6.5 Auto-animation

An 8-bit prescaler counts frames. When it underflows, it reloads from `REG_LSPCMODE` bits 15–8 and increments a 3-bit counter, so a speed of *n* advances the animation every *n + 1* frames. A new speed takes effect at the next reload. Tiles with the ×4 or ×8 bit set get the **low 2 or 3 bits of their tile number replaced** (not added to) by the counter; ×8 wins if both are set. `REG_LSPCMODE` bit 3 freezes the effect while the counters keep running. Games read the counter back from `REG_LSPCMODE` bits 2–0.

#### 4.6.6 Fix layer

- The map is at VRAM `$7000`, **column-major**: `entry = vram[0x7000 + col * 32 + row]`, 40 columns × 32 rows (256 lines, including the borders). NTSC shows rows 2–29 (28 rows = 224 lines). Columns 0 and 39 sit in the overscan, which is why SNK's safe area is 38×28.
- Entry: bits 15–12 palette (**palettes 0–15 only**), bits 11–0 tile number (4,096 tiles = 128 KiB of S ROM).
- Tiles come from the cartridge **S ROM**, or on MVS from the board's **SFIX** ROM (selected by `REG_BRDFIX`/`REG_CRTFIX`). The AES has no SFIX and always uses the cartridge S ROM.
- Colour index 0 is transparent. The fix layer always draws above sprites.
- Late NEO-CMC games (Garou, Metal Slug 3/4, KOF 2000/2003, SVC, Matrimelee…) use **fix bank switching** through VRAM `$7500`+: type 1 switches per line, type 2 per tile. Geolith's `geo_lspc_fixline_line` / `_tile` implement both (ported from MAME).

#### 4.6.7 Palettes and colour

- Palette RAM appears at `$400000–$401FFF`: 4,096 words for the active bank, 16 words per palette. Two banks, selected by the system latch.
- **Colour word**: bit 15 dark (shared LSB of all channels, *inverted* in hardware) · 14 R0 · 13 G0 · 12 B0 · 11–8 R4–R1 · 7–4 G4–G1 · 3–0 B4–B1. Sanity checks: `$0F00` is red, `$8000` is black, `$7FFF` is white.
- `$400000` (palette 0, colour 0) is the **reference colour** and must stay black. `$401FFE` (the last colour) is the **backdrop**.
- `REG_SHADOW` darkens the whole output (a 150 Ω pull-down). Halving each channel is a fine approximation; Geolith also offers a resistor-network model.
- Palettes can be written mid-frame. Hardware shows "snow" at the pixel being drawn. Emulators just apply the change from the next line on.

Geolith's "raw" palette conversion, checked equal to Geolith for all 65,536 inputs:

```rust
/// Neo Geo colour word -> RGB888 (the dark bit is inverted in hardware).
pub fn neo_color_to_rgb(c: u16) -> [u8; 3] {
    let dark_lsb = ((c >> 15) & 1) ^ 1;
    let ch = |msb4: u16, lsb: u16| -> u8 {
        let v6 = (msb4 << 2) | (lsb << 1) | dark_lsb;   // 6-bit intensity
        ((v6 as u32 * 259 + 33) >> 6) as u8             // round(v6 * 255 / 63)
    };
    [ch((c >> 8) & 0xF, (c >> 14) & 1), ch((c >> 4) & 0xF, (c >> 13) & 1), ch(c & 0xF, (c >> 12) & 1)]
}
```

Precompute two 65,536-entry lookup tables (normal and shadow). Keep a second copy of palette RAM already converted to RGBA and update it on every palette write, so drawing a pixel is a single array read.

#### 4.6.8 Tile formats

**C ROM (sprites)**: 16×16 px, 4 bpp planar, 128 bytes per tile, stored as four 8×8 blocks in the order top-right, bottom-right, top-left, bottom-left — the *right* half comes first. Each 8-pixel row is 4 bytes, one per bitplane, and the **leftmost pixel is bit 0**. C ROMs come in pairs: odd chips (C1, C3…) hold bitplanes 0–1, even chips (C2, C4…) hold bitplanes 2–3. MAME regions and `.neo` files **interleave each pair byte by byte** (C1 at even offsets), so a row's four bytes are bitplanes `[0, 2, 1, 3]`.

**S ROM / SFIX (fix)**: 8×8 px, 4 bpp packed, 32 bytes per tile, stored in column pairs: bytes `$10–$17` hold pixel columns 0–1 (rows 0–7), `$18–$1F` columns 2–3, `$00–$07` columns 4–5, `$08–$0F` columns 6–7. In each byte, the **low nibble is the left pixel**.

Both decoders were checked equal to Geolith's (including horizontal flip, which is simply `x → 15 − x`):

```rust
/// Colour index (0 = transparent) of pixel (x, y) of a 16x16 sprite tile.
/// `c`: byte-interleaved C ROM (C1 at even offsets, C2 at odd), as in MAME regions and .neo files.
pub fn sprite_pixel(c: &[u8], tile: u32, x: u32, y: u32) -> u8 {
    let base = tile as usize * 128 + if x < 8 { 64 } else { 0 } + y as usize * 4;
    let bit = x & 7;
    let p = |i: usize| (c[base + i] >> bit) & 1;
    p(0) | p(2) << 1 | p(1) << 2 | p(3) << 3
}

/// Colour index (0 = transparent) of pixel (x, y) of an 8x8 fix tile.
pub fn fix_pixel(s: &[u8], tile: u32, x: u32, y: u32) -> u8 {
    const COL: [usize; 4] = [0x10, 0x18, 0x00, 0x08];
    let b = s[tile as usize * 32 + COL[(x >> 1) as usize] + y as usize];
    if x & 1 == 0 { b & 0x0F } else { b >> 4 }
}
```

(Neo Geo CD sprite data uses a different byte order, bitplanes `[1, 0, 3, 2]`.)

#### 4.6.9 Output

- Render 320 × 224 (raster lines `$110–$1EF`). Keep a 320 × 256 internal buffer if you want to see the borders in the debugger.
- Most MVS monitors hid about 8 px on each side, and SNK's safe area is 304 px wide. Offer a "crop to 304" option.
- The picture fills a 4:3 screen, so pixels are slightly narrow: pixel aspect ratio = (4/3) / (320/224) ≈ 0.933.

### 4.7 Sound: Z80, the 68k ↔ Z80 mailbox, YM2610

#### 4.7.1 Z80 memory map (cartridge systems)

| Range | Size | Contents |
|---|---|---|
| `$0000–$7FFF` | 32 KiB | M1 ROM, fixed (start of the ROM) |
| `$8000–$BFFF` | 16 KiB | Bank window, selected via port `$0B` (8-bit bank × 16 KiB → 4 MiB) |
| `$C000–$DFFF` | 8 KiB | Bank window, port `$0A` (× 8 KiB) |
| `$E000–$EFFF` | 4 KiB | Bank window, port `$09` (× 4 KiB) |
| `$F000–$F7FF` | 2 KiB | Bank window, port `$08` (× 2 KiB) |
| `$F800–$FFFF` | 2 KiB | Z80 RAM |

The M1 ROM is the cartridge's, or on MVS the board's SM1 driver (`REG_BRDFIX`). The AES has no SM1. Bank numbers are undefined at power-on. Geolith starts with an identity mapping (window `$8000` → ROM `$8000`, and so on) because some games never set the banks. Mask bank numbers to the actual M1 size.

#### 4.7.2 Z80 I/O ports

| Port (low byte) | Read | Write |
|---|---|---|
| `$00` | Command byte from the 68000; acknowledges the NMI | Clear the command latch to `$00` |
| `$04`–`$07` | YM2610 status / data | YM2610: `$04` address A, `$05` data A, `$06` address B, `$07` data B |
| `$08` / `$09` / `$0A` / `$0B` | **Select the bank** for `$F000` / `$E000` / `$C000` / `$8000`. The bank number is the **high byte of the port address** | Enable NMIs |
| `$0C` | — | Reply byte to the 68000 (`REG_SOUND`) |
| `$18` | (same as `$08`–`$0B`) | Disable NMIs |

- Reads decode with mask `$0C`, so `$C0` mirrors `$00` (some drivers use it). Writes to `$08`/`$18` decode with `$1C`.
- **Bank switching happens on port *reads*, and the bank number travels on the upper half of the 16-bit port address.** `IN A,(n)` puts A on A15–A8; `IN r,(C)` puts B there. Your Z80 core must pass the **full 16-bit port address** to the bus. Both `z80` and `iz80` do.

#### 4.7.3 The 68k ↔ Z80 mailbox

- One byte each way, no shared RAM. 68000 → Z80: write `REG_SOUND`; the byte is latched and a Z80 NMI fires (if enabled); the Z80 reads it from port `$00`. Z80 → 68000: write port `$0C`; the 68000 reads it from `REG_SOUND`. No interrupt on this side.
- Commands every driver must implement: **`$01`** prepare for a slot switch (stop sound, reply `$01`, loop in RAM — the MVS BIOS shows "Z80 ERROR" without that reply); **`$02`** play the eye-catcher jingle; **`$03`** soft reset. Many drivers acknowledge other commands by echoing `cmd | $80`.
- There is no hardware FIFO; drivers buffer commands in software, so NMI latency matters. Sync the Z80 after every 68000 instruction (the Geolith model) and no command gets lost.

#### 4.7.4 YM2610 (OPNB)

| Part | Channels | Notes |
|---|---|---|
| FM | 4 | 4 operators each. The chip is a YM2610B with two FM channels removed: channel codes `001, 010, 101, 110`. Ports 4/5 drive FM 1–2, ports 6/7 drive FM 3–4 |
| SSG | 3 square + noise + envelope | YM2149-compatible, without the I/O ports. **Mono analog** output, mixed outside the chip |
| ADPCM-A | 6 | Fixed ≈18.52 kHz (8 MHz ÷ 12 ÷ 6 ÷ 6). 4-bit Yamaha ADPCM → 12-bit. Samples in the **V1** ROM; start/end addresses are 256-byte aligned (24-bit address space). Ports 6/7 |
| ADPCM-B | 1 | Variable, 1.85–55.5 kHz, 4-bit → 16-bit. Samples in **V2** (the same ROM on single-V boards). Ports 4/5. The CD systems lack it |
| Timers | A, B | Drive the Z80 **INT**. Most drivers time their music from them — no timers, no music |

- FM and ADPCM go through a YM3016 stereo DAC; the SSG output is analog mono. You mix them yourself. Geolith adds the SSG to both stereo channels and clamps.
- On hardware, the chip needs 17 YM clocks after an address write and 83 after a data write (the busy flag). Homebrew that ignores this works in emulators and fails on hardware.
- ADPCM end-of-sample flags live in register `$1C`: write 1 to clear and mask a flag, 0 to unmask it.
- Geolith clocks ymfm once every 72 Z80 cycles (= 432 mclk) and gets one stereo sample per call at ≈55.5 kHz (ymfm's "medium fidelity").

### 4.8 Cartridges, ROM files and the `.neo` format

| ROM | Bus | Contents | Loading notes |
|---|---|---|---|
| **P** (P1, P2…) | 68000 | Program | Stored with **every 16-bit word byte-swapped**, like BIOS dumps. Swap once at load. Up to 1 MiB is fixed at `$000000`; larger programs are bank-switched into `$200000` |
| **S** (S1) | LSPC | Fix tiles | 128 KiB. NEO-CMC games hide it at the end of the C ROM data (Lithogen extracts it) |
| **M1** | Z80 | Sound driver + music data | Banked by NEO-ZMC(2) |
| **V1, V2…** | YM2610 | ADPCM-A / ADPCM-B samples | Early boards split A and B data; later boards (PCM chip) mix both in the same ROMs |
| **C1, C2…** | LSPC / ZMC2 | Sprite tiles | Pairs, byte-interleaved |

**P ROM bank switching (standard boards).** A write to `$2FFFF0–$2FFFFF` selects the 1 MiB bank seen at `$200000`: `offset = 0x10_0000 + (value & mask) * 0x10_0000`. At reset, programs larger than 1 MiB map offset `$100000` there; programs of 1 MiB or less mirror offset 0. MAME and Geolith both decode `$2FFFF0–$2FFFFF`; on some real boards the latch reacts to any write in the `$2xxxxx` window. In MAME's sets, a single 2 MiB P chip has its two megabytes swapped; `.neo` files already store the fixed bank first.

**The `.neo` format** (TerraOnion; used by NeoSD, MiSTer and Geolith). All numbers are little-endian `u32`:

| Offset | Field |
|---|---|
| 0 | Magic `"NEO"` followed by version byte `1` |
| 4, 8, 12, 16, 20, 24 | Sizes of P, S, M1, V1, V2, C. `V2 = 0` means a single V ROM serves both ADPCM-A and ADPCM-B |
| 28, 32, 36, 40 | Year, genre, screenshot number, **NGH number** |
| 44 | Name (33 bytes) |
| 77 | Manufacturer (17 bytes) |
| 4096 | Data: P, S, M1, V1, V2, C, back to back. C is already interleaved; P is still word-swapped |

`.neo` files are **decrypted but not patched**. Lithogen applies the static decryption (CMC42/CMC50 C/S/M1, SMA program, NEO-PCM2, PVC, KOF '98, the KOF 2002 family, bootlegs). Runtime behaviour — SMA bank switching and PRNG, PVC registers, challenge-response checks — remains your emulator's job, exactly as on hardware.

### 4.9 Board types and protection chips

Geolith keys special handling on the header's NGH number. Its [`geo_neo.c`](https://github.com/libretro/geolith-libretro/blob/master/src/geo_neo.c) and [`geo_m68k.c`](https://github.com/libretro/geolith-libretro/blob/master/src/geo_m68k.c) are the most compact complete list (BSD-3: portable with attribution):

| NGH | Game(s) | Handling |
|---|---|---|
| `006`, `019`, `038` | Riding Hero, League Bowling, Thrash Rally | Linkable multiplayer board (serial link faked) |
| `008`, `3E7` | Jockey Grand Prix, V-Liner | BrezzaSoft board with cartridge RAM |
| `047`, `052` | Fatal Fury 2, Super Sidekicks | PRO-CT0 protection |
| `242` | KOF '98 | KOF98 protection (overlay writes at `$2FFFF0`) |
| `250` | Metal Slug X | Protection registers in the `$2FFFxx` window |
| `251` | KOF '99 | NEO-SMA (if the set is encrypted) |
| `253` | Garou | NEO-SMA (two chip revisions) + per-line fix bank switching |
| `256` | Metal Slug 3 | NEO-SMA (if encrypted) + per-line fix bank switching |
| `257` | KOF 2000 | NEO-SMA (if encrypted) + per-tile fix bank switching |
| `263` | Metal Slug 4 | Per-line fix bank switching |
| `266` | Matrimelee | Per-tile fix bank switching |
| `268` | Metal Slug 5 | NEO-PVC |
| `269` | SNK vs. Capcom | NEO-PVC + per-tile fix bank switching |
| `271` | KOF 2003 | NEO-PVC + per-tile fix bank switching |
| `004`, `027`, `036`, `048` | Mahjong titles | Mahjong controller |

On top of these come bootleg-specific boards (KOF 2003 bootlegs, KOF 10th Anniversary, CTHD 2003, MS5 Plus…) and a handful of titles that only run in MVS mode.

What each chip does:

| Chip | Where | Emulator impact |
|---|---|---|
| **NEO-CMC** 042 / 050 | CHAFIO boards, 1999+. 042: KOF '99, Garou, MS3, Ganryu, Zupapa… 050: KOF 2000–2003, MS4, MS5, Rage of the Dragons, SVC, SS5… | Encrypted C and S (042), plus M1 (050): **static**, handled by Lithogen. Also enables fix bank switching: **runtime**, in your LSPC |
| **NEO-SMA** | KOF '99, KOF 2000, MS3 (some sets), Garou | P encryption (static) plus **runtime** scrambled bank switching and a 16-bit LFSR PRNG (reset value `$2345`) |
| **NEO-PCM2** | MS4, MS5, KOF 2001–2003, SVC, SS5, Matrimelee, Rage of the Dragons… | V ROM scrambling / encryption: static |
| **NEO-PVC** | SVC, MS5, KOF 2003 | P scrambling (static) plus **runtime** bank switching (`$2FFFF0/2`), colour pack/unpack helper registers, 8 KiB RAM at `$2FE000` |

### 4.10 System ROMs, AES vs MVS, watchdog, RTC, backup RAM

**BIOS files** (names used by MAME and Geolith):

| System | Archive | Files |
|---|---|---|
| AES | `aes.zip` | `neo-epo.bin` (export) or `neo-po.bin` (Japan) |
| MVS | `neogeo.zip` | `sp-s2.sp1` (Europe) · `sp-u2.sp1` (US) · `japan-j3.bin` (Japan) · `sp-45.sp1` (Asia) · `sfix.sfix` (board fix tiles) · `sm1.sm1` (board sound driver) · `000-lo.lo` (L0 — you can generate it) |
| Universe BIOS | `neogeo.zip` | `uni-bios_4_0.rom`, free for personal use from [unibios.free.fr](http://unibios.free.fr/download.html) |

BIOS images are word-byte-swapped, like P ROMs.

| | AES | MVS |
|---|---|---|
| `REG_STATUS_B` bit 7 | 0 | 1 |
| Fix / Z80 ROM | Always the cartridge's | Board SFIX / SM1 until `REG_CRTFIX` |
| Backup RAM `$D00000` | No | 64 KiB, battery-backed, lockable |
| RTC | No | NEC uPD4990A: serial protocol over `REG_RTCCTRL`, read back through `REG_STATUS_A` bits 6–7. The BIOS watches its 1 Hz "time pulse" |
| Coins, service, DIPs | No | Yes |
| Boot friction | Low | Z80 `$01` handshake, RTC, backup RAM contents, coin/credit settings |

- **Watchdog**: every write to `$300001` restarts the count. After 3,244,030 mclk without one, the whole system resets. Some games use watchdog resets as copy protection, so emulate it, but give the debugger a switch to turn it off.
- **Memory card**: 2 KiB, 8-bit, at `$800000` on odd addresses; even-address byte reads return `$FF`.
- **Backup RAM** holds MVS bookkeeping, soft DIPs and game saves. Persist it per game.

### 4.11 Real-hardware quirks you must not "fix"

From Geolith's [`NONBUGS`](https://github.com/libretro/geolith-libretro/blob/master/NONBUGS) file. All of these happen on real hardware:

- MVS BIOS boot screen: the top-right corner of the fix layer updates late, which shows as tearing.
- Samurai Shodown IV intro: a block of the top border disappears because of the 96-sprites-per-line limit.
- Master of Syougi: the title-screen reflection raster effect starts one line late.
- Ninja Commando: corrupted title image in attract mode.
- Metal Slug 4: wrong fix graphics on the parental-advisory screen.
- KOF 2003 bootlegs: incorrect zooming. Ganryu and Nightmare in the Dark: audio clicks and glitches.

---

## 5. The Rust stack

Versions are the latest on crates.io as of 2026-09-18.

### 5.1 68000

`m68k = "=0.13.0"` behind the `Cpu68k` trait. The full comparison is in [§2](#2-is-the-m68000-crate-the-right-choice).

### 5.2 Z80

Whatever Z80 core you use must provide:

- The **full 16-bit port address** on `IN`/`OUT` (bank switching, [§4.7.2](#472-z80-io-ports)).
- Reads with side effects (the NMI acknowledge on port `$00`, bank selects on ports `$08–$0B`), so reads need `&mut` access or interior mutability.
- NMI and INT, with IM 1 at minimum, and IM 0 behaving like `RST 38h` when the bus reads `$FF`.
- T-state counts per instruction.
- Access to all state for save states, including WZ/MEMPTR, IFF1/IFF2, IM, HALT and pending NMI/INT.

| Crate | Version · license | Validation | API fit | Notes |
|---|---|---|---|---|
| [`z80`](https://crates.io/crates/z80) | 2.0.0 (Aug 2026) · MIT | ZEXDOC + ZEXALL; verified against VisualZ80 | `Z80_io` trait (`read_byte`, `write_byte`, `port_in`, `port_out`), `step() -> u32` cycles, `assert_irq(data)`, `clr_irq()`, `pulse_nmi()`, all IM modes | A c2rust port of [jgz80](https://github.com/carmiker/jgz80), **the Z80 core Geolith ships**. Around 70 `unsafe` blocks. `read_byte` / `port_in` take `&self`, so wrap your bus in a `RefCell` (below). Small project (a handful of stars) |
| [`iz80`](https://crates.io/crates/iz80) | 0.5.1 · BSD-3 | ZEXALL | `&mut` `Machine` trait, `signal_interrupt`, `signal_nmi`, `cycle_count` | **Panics on IM 0 and IM 2** (`"not implemented"`). Instruction-level |
| [`cell80-z80`](https://crates.io/crates/cell80-z80) | 0.8.0 · MIT/Apache-2.0 | 1,530,000/1,530,000 SingleStepTests incl. cycle counts; ZEXDOC | `no_std`, `Bus` trait | Very young, and part of a larger agent-sandbox project. Evaluate before depending on it |
| [`z80emu`](https://crates.io/crates/z80emu) | 0.11.0 · LGPL-3.0 | T-state accurate | Rich | LGPL plus Rust's static linking is friction for an Apache-2.0 app |
| **Your own** | — | [SingleStepTests/z80](https://github.com/SingleStepTests/z80) (1,000 tests per opcode, including bus cycles) + ZEXALL | Exactly what you need | 1,500–3,000 lines. The classic first CPU. Recommended once the 68000 side boots |

The machine only talks to its own trait, so the choice stays reversible:

```rust
// crates/neogeo-core/src/sound/z80.rs
pub trait Z80Bus {
    fn read(&mut self, addr: u16) -> u8;
    fn write(&mut self, addr: u16, value: u8);
    fn port_in(&mut self, port: u16) -> u8;          // full 16-bit port address
    fn port_out(&mut self, port: u16, value: u8);
}

pub trait Z80Core {
    fn reset(&mut self);
    /// Execute one instruction; returns T-states.
    fn step(&mut self, bus: &mut impl Z80Bus) -> u32;
    fn set_int(&mut self, asserted: bool);
    fn nmi(&mut self);
}

/// Adapter for the `z80` crate, whose read callbacks take `&self`.
pub struct CrateZ80(z80::Z80);

struct Shim<'a, B: Z80Bus>(std::cell::RefCell<&'a mut B>);

impl<B: Z80Bus> z80::Z80_io for Shim<'_, B> {
    fn read_byte(&self, a: u16) -> u8 { self.0.borrow_mut().read(a) }
    fn write_byte(&mut self, a: u16, v: u8) { self.0.get_mut().write(a, v) }
    fn port_in(&self, p: u16) -> u8 { self.0.borrow_mut().port_in(p) }
    fn port_out(&mut self, p: u16, v: u8) { self.0.get_mut().port_out(p, v) }
}

impl Z80Core for CrateZ80 {
    fn reset(&mut self) { self.0.reset() }
    fn step(&mut self, bus: &mut impl Z80Bus) -> u32 { self.0.step(&mut Shim(std::cell::RefCell::new(bus))) }
    // $FF on the data bus: IM 0 then executes RST 38h, like the real open bus.
    fn set_int(&mut self, on: bool) { if on { self.0.assert_irq(0xFF) } else { self.0.clr_irq() } }
    fn nmi(&mut self) { self.0.pulse_nmi() }
}
```

### 5.3 YM2610

| Option | License | Effort | Notes |
|---|---|---|---|
| [`ymfm-sys`](https://crates.io/crates/ymfm-sys) 0.2.0 (a `cxx` binding of ymfm) | BSD-3 | Hours | Needs a C++14 compiler (Xcode Command Line Tools). `create_chip(ChipType::Ym2610, clock)`, `write(port, value)`, `generate(&mut [i32])` at the chip's native rate, and an `InterfaceHandler` for ADPCM ROM reads, timers and IRQ. **Brand new** (Aug 2026, ~50 downloads): keep it behind your trait, and fall back to building ymfm yourself with the `cc` crate if it misbehaves |
| Port ymfm's OPNB to Rust, starting from Geolith's C port (`ymfm_opn.c`, `ymfm_fm.inc`, `ymfm_adpcm.c`, `ymfm_ssg.c`: ≈4,700 lines) | BSD-3 | 2–4 weeks | Pure Rust, wasm-friendly, trivial save states. C is far easier to port than ymfm's C++ templates |
| From scratch, with datasheets and ymfm as the oracle | Yours | Months | Only if sound-chip emulation itself is the goal |

The core defines the trait; the `neogeo-ym2610` crate implements it; the app picks one. That keeps C++ out of `neogeo-core` (useful for wasm):

```rust
// crates/neogeo-core/src/sound/ym2610.rs
pub trait Ym2610 {
    fn reset(&mut self);
    /// `port` 0..=3 = Z80 ports $04..=$07.
    fn read(&mut self, port: u8) -> u8;
    fn write(&mut self, port: u8, value: u8);
    /// Advance by one output sample (144 YM clocks = 432 mclk) and return it (L, R).
    /// Implementations own the V ROMs (ADPCM-A reads V1, ADPCM-B reads V2).
    fn clock_sample(&mut self) -> [i32; 2];
    fn irq(&self) -> bool;
}
```

Wiring `ymfm-sys` means three things: ADPCM ROM reads through `InterfaceHandler` (V1 for ADPCM-A, V2 for ADPCM-B), the two timers (ymfm hands you durations in chip clocks; you count them down and report expiry), and `update_irq` → Z80 INT. Geolith's [`geo_ymfm.c`](https://github.com/libretro/geolith-libretro/blob/master/src/geo_ymfm.c) (176 lines) is a complete worked example of exactly this glue.

### 5.4 Video, windowing and UI

| Crate | Version | Role | Verdict |
|---|---|---|---|
| `eframe` / `egui` / `egui_extras` | 0.36.2 | Window + GPU + immediate-mode UI in one | **Main development app**: game view plus debugger windows |
| `winit` | 0.30.13 | Windows and events | For a lean "player" frontend later |
| `pixels` | 0.17.2 | wgpu-backed pixel buffer with scaling | Pair with `winit` for the player |
| `softbuffer` | 0.4.8 | CPU blit into a window | Fewer dependencies than `pixels` |
| `minifb` | 0.28.0 | Window + framebuffer in about ten lines | Quickest "first pixels" if you don't want egui yet |
| `wgpu` | 30.0.1 | GPU API | Only if you write your own CRT shaders |
| `sdl3` | 0.20.0 | SDL3 bindings (needs the library: `brew install sdl3`) | If you prefer SDL's all-in-one video/audio/input |

egui's API moves quickly. In 0.36 the `App` trait's required method is `ui(&mut self, ui: &mut egui::Ui, frame: &mut eframe::Frame)`, with an optional `logic(&mut self, ctx, frame)` hook that runs before each `ui` call. Older tutorials use `update(ctx, frame)`; check the examples for the version you pin.

### 5.5 Audio

| Crate | Version | Role |
|---|---|---|
| `cpal` | 0.18.2 | Audio output. In 0.18 `build_output_stream` takes the `StreamConfig` **by value**, and `SampleRate` is a plain `u32` |
| `rtrb` | 0.4.0 | Realtime-safe single-producer/single-consumer ring buffer between the emulator thread and the audio callback |
| `ringbuf` | 0.5.2 | Alternative ring buffer |
| `rubato` | 5.0.0 | High-quality resampling, later. A linear resampler plus dynamic rate control is enough to start ([§6.8](#68-audio-output-and-frame-pacing)) |

### 5.6 Input

`gilrs` 0.11.2 for gamepads; the keyboard comes from egui (or winit). Map to the active-low register bits of [§4.3](#43-io-registers).

### 5.7 Save states and serialization

- `serde` 1.0 + `postcard` 1.1 (feature `use-std`): compact, stable, `no_std`-friendly.
- **Not `bincode`**: 3.0.0 is a tombstone release that only contains a compile error; the maintainers stopped development and point to `wincode`, `postcard` and `rkyv`.
- `rkyv` 0.8 if you ever want zero-copy rewind buffers.
- `m68k`'s `serde` feature serializes the 68000's architectural state.

### 5.8 Files and ROM management

`zip` 8.6 (MAME BIOS zips), `crc32fast` 1.5 and `sha1` 0.11 (identify dumps), `quick-xml` 0.42 (parse MAME's `hash/neogeo.xml` software list), `png` 0.18 (screenshots, golden frames), `clap` 4.6, `anyhow` / `thiserror`, `log` + `env_logger` (or `tracing`), `bytemuck` (view the RGBA framebuffer as bytes without `unsafe` in your code).

### 5.9 Development crates

`insta` 1.48 (snapshot tests), `criterion` 0.8 (benchmarks), `proptest` 1.11 (fuzz the decoders), and much later `ggrs` 0.13 (GGPO-style rollback netplay — possible because the core is deterministic).

### 5.10 Licensing for an Apache-2.0 project

| Component | License | How you may use it |
|---|---|---|
| `m68k`, `z80`, `cpal`, `egui`… | MIT / Apache-2.0 | Normal dependencies |
| `iz80`, ymfm, `ymfm-sys` | BSD-3-Clause | Dependencies; keep their notices |
| **Geolith** | BSD-3-Clause | **Port code with attribution**: keep its copyright line and license text (file headers + a `NOTICE`/`THIRD_PARTY` file) |
| MAME's `src/mame/snk/neogeo*.cpp` | BSD-3-Clause (per file header) | Same as Geolith. Much of MAME is GPL-2.0+, so check the header of every file you port from (for example the protection devices in `src/devices/bus/neogeo/`) |
| `m68000` | MPL-2.0 | Fine as a dependency (file-level copyleft) |
| ngdevkit / its examples | LGPL-3.0 / GPL-3.0 | Tools and test content only; nothing is linked into your binary |
| jgenesis, MiSTer core | GPL-3.0 / GPL-2.0 | **Read-only reference** |
| FinalBurn Neo | Non-commercial license | Read-only reference |
| `z80emu` | LGPL-3.0 | Avoid |

Enforce it with `cargo-deny`:

```toml
# deny.toml
[licenses]
allow = [
    "MIT", "Apache-2.0", "Apache-2.0 WITH LLVM-exception", "BSD-2-Clause", "BSD-3-Clause",
    "ISC", "Zlib", "Unicode-3.0", "MPL-2.0", "BSL-1.0", "CC0-1.0",
]
confidence-threshold = 0.9
```

---

## 6. Architecture

### 6.1 Principles

1. **The core is a pure library.** `neogeo-core` does no I/O, spawns no threads and never reads the wall clock. Frontends own windows, audio devices, files and time. The core is therefore testable headless, deterministic, and portable (desktop, libretro, wasm).
2. **One clock.** A `u64` master-cycle counter (24 MHz ticks). Every component converts with integer dividers. No floating-point time in the core.
3. **Lockstep first.** Step the 68000 one instruction at a time and catch everything else up after each step: Geolith's model, proven on the full library. Move to bigger time slices only if a profile tells you to.
4. **Scanline rendering.** Render each line at a fixed point in the line, from a palette kept converted to RGBA.
5. **Everything is state.** All mutable machine state is serializable. ROMs are not state: skip them in save states and re-attach them on load.
6. **Swappable chips.** CPU and sound cores sit behind small traits (`Cpu68k`, `Z80Core`, `Ym2610`), so crates can be replaced by your own implementations without touching the machine.
7. **Debug hooks behind a cargo feature.** Breakpoints, watchpoints and traces cost nothing when the feature is off.
8. **`#![forbid(unsafe_code)]` in the core.** FFI lives in its own crate (`neogeo-ym2610` with the `ymfm` feature).

### 6.2 Workspace layout

```
neo-geo/
├── Cargo.toml                  # virtual workspace: shared package info, dependency versions, profiles
├── rust-toolchain.toml
├── deny.toml
├── crates/
│   ├── neogeo-core/            # the machine. No I/O, no threads, no wall clock. #![forbid(unsafe_code)]
│   │   └── src/
│   │       ├── lib.rs          # NeoGeo: new / power_on / run_frame / framebuffer / audio / input
│   │       ├── timing.rs       # clock constants
│   │       ├── bios.rs         # System (AES/MVS), BIOS images, generated L0 table
│   │       ├── cpu68k.rs       # Cpu68k trait + m68k adapter
│   │       ├── bus.rs          # MainBus: 68000 memory map, AddressBus impl
│   │       ├── io.rs           # inputs, DIPs, system latches, watchdog, backup RAM, memory card
│   │       ├── rtc.rs          # uPD4990A (MVS)
│   │       ├── lspc/           # VRAM, palettes, timer, IRQ latches, beam position, line renderer
│   │       ├── sound/          # Z80Core/Z80Bus traits, ZMC banks, mailbox, Ym2610 trait
│   │       ├── cart/           # .neo parser, board types (default, SMA, PVC, CT0, KOF98…)
│   │       └── state.rs        # save-state structs and versioning
│   ├── neogeo-ym2610/          # Ym2610 implementations: `ymfm` feature (ymfm-sys) now, `native` later
│   ├── neogeo-z80/             # (later) your own Z80
│   └── neogeo-m68k/            # (optional, later) your own 68000
├── apps/
│   ├── neogeo-desktop/         # eframe + egui debugger + cpal + gilrs
│   └── neogeo-headless/        # CLI: run N frames, PNG/WAV dumps, traces, golden tests
├── tools/
│   └── neogeo-romtool/         # inspect .neo files, dump tiles/palettes, build .neo from raw ROM files
├── scripts/
│   └── fetch-fixtures.sh       # downloads test suites and builds ngdevkit examples (never committed)
├── roms/                       # gitignored: bios/, neo/, fixtures/
└── docs/
```

### 6.3 Ownership model

The borrow checker shapes an emulator's layout. The 68000 needs `&mut` access to the whole bus while it runs, and devices on the bus need to interrupt the CPU. So:

- The CPU is a **sibling** of the bus, never inside it. Devices raise flags on the bus; after each instruction the scheduler reads the IRQ level from the bus and pushes it into the CPU.
- The Z80 and YM2610 live **inside** the 68000's bus (in `SoundSystem`), because 68000 accesses to `REG_SOUND` must reach the mailbox. The Z80 gets a short-lived bus view built from disjoint field borrows ([§6.7](#67-sound-system)).
- No `Rc<RefCell<…>>` object graphs and no trait objects on the hot path: plain structs and `&mut`.

```rust
pub struct NeoGeo {
    cpu: CrateM68k,           // sibling of the bus (borrowck!)
    pub bus: MainBus,
    clock: u64,               // master cycles since power-on
    frame_end: u64,
}

pub struct MainBus {
    pub system: System,       // Aes | Mvs
    pub bios: Bios,           // system ROM (68000 byte order), SFIX/SM1 on MVS, generated L0
    pub cart: Cartridge,      // P/S/M1/C ROMs + board logic (bank registers, protection)
    pub work_ram: Box<[u8]>,  // 64 KiB
    pub lspc: Lspc,           // VRAM, palettes, timer, IRQ latches, beam position, renderer
    pub io: Io,               // inputs, DIPs, system latches, watchdog, backup RAM, memcard, RTC
    pub sound: SoundSystem,   // Z80 + NEO-ZMC banks + mailbox + YM2610
}
```

### 6.4 Scheduler

```rust
// crates/neogeo-core/src/lib.rs
impl NeoGeo {
    /// Emulate exactly one video frame (405,504 master cycles).
    pub fn run_frame(&mut self) {
        self.frame_end += MCLK_PER_FRAME;
        while self.clock < self.frame_end {
            // 1. One 68000 instruction (or interrupt entry + instruction).
            let mclk = self.cpu.step(&mut self.bus) as u64 * M68K_DIV;
            self.clock += mclk;

            // 2. Advance everything the 68000 can observe, in Geolith's order.
            self.bus.io.watchdog_tick(mclk);
            self.bus.advance_video(mclk);          // beam, timer IRQ2, VBlank IRQ1, auto-animation, line renders
            self.bus.sound.run_until(self.clock);  // Z80 + YM2610 catch-up: mailbox latency < 1 instruction

            // 3. Interrupt lines and resets.
            self.cpu.set_irq_level(self.bus.lspc.irq.level());
            if self.bus.io.watchdog_expired() {
                self.reset(ResetKind::Watchdog);   // 68000 + Z80 + YM2610 + LSPC, not a power cycle
            }
        }
    }
}
```

An interrupt raised during an instruction becomes visible at the next step, i.e. at most one instruction late. Geolith lives with the same granularity. The budget is tiny by modern standards: 202,752 CPU cycles per frame, roughly 20,000 68000 instructions and 8,000 Z80 instructions.

### 6.5 Bus decoding

```rust
// crates/neogeo-core/src/bus.rs
impl m68k::AddressBus for MainBus {
    fn read_byte(&mut self, a: u32) -> u8 { self.read8(a & 0xFF_FFFF) }
    fn read_word(&mut self, a: u32) -> u16 { self.read16(a & 0xFF_FFFF) }
    fn read_long(&mut self, a: u32) -> u32 {
        (self.read16(a & 0xFF_FFFF) as u32) << 16 | self.read16(a.wrapping_add(2) & 0xFF_FFFF) as u32
    }
    fn write_byte(&mut self, a: u32, v: u8) { self.write8(a & 0xFF_FFFF, v) }
    fn write_word(&mut self, a: u32, v: u16) { self.write16(a & 0xFF_FFFF, v) }
    fn write_long(&mut self, a: u32, v: u32) {
        self.write16(a & 0xFF_FFFF, (v >> 16) as u16);
        self.write16(a.wrapping_add(2) & 0xFF_FFFF, v as u16);
    }
    // interrupt_acknowledge: keep the default (autovector).
}

impl MainBus {
    pub fn read16(&mut self, a: u32) -> u16 {
        match a >> 20 {
            0x0 if a < 0x80 && !self.io.latch.cart_vectors => be16(&self.bios.rom, a),
            0x0 => self.cart.read_fixed16(a),
            0x1 => be16(&self.work_ram, a & 0xFFFF),
            0x2 => self.cart.read_banked16(a),         // bank window + protection chips
            0x3 => self.read_io16(a),
            0x4..=0x7 => self.lspc.palette_read16(a),
            0x8..=0xB => 0xFF00 | self.io.memcard_read(a) as u16,
            0xC => be16(&self.bios.rom, a & 0x1_FFFF),
            0xD if self.system == System::Mvs => be16(&self.io.backup_ram, a & 0xFFFF),
            _ => 0xFFFF,
        }
    }

    fn read_io16(&mut self, a: u32) -> u16 {
        match a & 0x3E_0000 {                           // 128 KiB register blocks
            0x30_0000 => {
                let lo = if a & 0x80 != 0 { self.io.systype() } else { self.io.dipsw() };
                (self.io.input.p1 as u16) << 8 | lo as u16
            }
            0x32_0000 => (self.sound.reply() as u16) << 8 | self.io.status_a() as u16,
            0x34_0000 => (self.io.input.p2 as u16) << 8 | 0xFF,
            0x38_0000 => { let b = self.io.status_b(self.system) as u16; b << 8 | b }
            0x3C_0000 => self.lspc.read_reg(a),         // 4 registers, mirrored every 8 bytes
            _ => 0xFFFF,
        }
    }

    pub fn read8(&mut self, a: u32) -> u8 {
        // Deriving byte reads from word reads is fine here: only a few protection
        // registers have read side effects (the SMA PRNG steps on every read), and
        // the cartridge code handles those itself.
        let w = self.read16(a & !1);
        if a & 1 == 0 { (w >> 8) as u8 } else { w as u8 }
    }

    pub fn write16(&mut self, a: u32, v: u16) {
        match a >> 20 {
            0x1 => set_be16(&mut self.work_ram, a & 0xFFFF, v),
            0x2 => self.cart.write_banked16(a, v),      // bank select at $2FFFF0, protection
            0x3 => self.write_io16(a, v),
            0x4..=0x7 => self.lspc.palette_write16(a, v),
            0xD if self.io.backup_unlocked() => set_be16(&mut self.io.backup_ram, a & 0xFFFF, v),
            _ => log::debug!("unmapped write16 {a:06X} = {v:04X}"),
        }
    }

    // write8 is the same shape, with two I/O rules: the system latches ($3A00xx) are
    // odd-byte writes, and LSPC registers store a byte written to an even address in
    // both halves of the word.
}

fn be16(mem: &[u8], a: u32) -> u16 { let i = a as usize; u16::from_be_bytes([mem[i], mem[i + 1]]) }
fn set_be16(mem: &mut [u8], a: u32, v: u16) { let i = a as usize; mem[i..i + 2].copy_from_slice(&v.to_be_bytes()) }
```

A `match` on the top address nibble is fast enough. If profiling ever says otherwise, switch to a 4 KiB page table of plain-memory pointers with a slow path for I/O.

### 6.6 LSPC

```rust
// crates/neogeo-core/src/lspc/mod.rs
#[derive(Default)]
pub struct IrqLatches { pub vblank: bool, pub timer: bool, pub reset: bool }

impl IrqLatches {
    pub fn level(&self) -> u8 {
        if self.reset { 3 } else if self.timer { 2 } else if self.vblank { 1 } else { 0 }
    }
    pub fn ack(&mut self, v: u16) {                 // REG_IRQACK
        if v & 4 != 0 { self.vblank = false; }
        if v & 2 != 0 { self.timer = false; }
        if v & 1 != 0 { self.reset = false; }
    }
}

/// Borrowed views of the graphics ROMs, built from disjoint fields of MainBus.
pub struct Gfx<'a> { pub c_rom: &'a [u8], pub fix_rom: &'a [u8], pub l0: &'a [u8] }

pub struct Lspc {
    pub vram: Vec<u16>,          // 0x8800 words
    pub palram: Vec<u16>,        // 2 banks × 4096 words
    pal_rgba: Vec<u32>,          // palram converted to RGBA8 (u32::from_le_bytes([r, g, b, 0xFF]))
    pub palette_bank: u16,       // 0 or 1, from the system latch
    pub irq: IrqLatches,
    vram_addr: u16,
    vram_mod: i16,
    mode: u16,                   // last REG_LSPCMODE write
    timer_reload: u32,
    timer: u32,
    pixel_frac: u64,             // leftover mclk (< 4) for the pixel-clocked timer
    line: u32,                   // 0..264; raster counter = line + 0xF8
    line_mclk: u64,              // position inside the current line
    aa_prescaler: u8,
    aa_counter: u8,
    pub frame: Vec<u32>,         // 320 × 224 RGBA8
}

impl Lspc {
    pub fn write_reg(&mut self, a: u32, v: u16) {
        match a & 0xE {
            0x0 => self.vram_addr = v,
            0x2 => {
                if let Some(w) = self.vram.get_mut(self.vram_addr as usize) { *w = v; }
                let zone = self.vram_addr & 0x8000;  // the modulo wraps inside the current zone
                self.vram_addr = (self.vram_addr.wrapping_add(self.vram_mod as u16) & 0x7FFF) | zone;
            }
            0x4 => self.vram_mod = v as i16,
            0x6 => self.write_mode(v),
            0x8 => self.timer_reload = (self.timer_reload & 0x0000_FFFF) | (v as u32) << 16,
            0xA => {
                self.timer_reload = (self.timer_reload & 0xFFFF_0000) | v as u32;
                if self.mode & 0x20 != 0 { self.timer = self.timer_reload; }
            }
            0xC => self.irq.ack(v),
            _ => {}                                  // $3C000E REG_TIMERSTOP: PAL only
        }
    }

    pub fn read_reg(&self, a: u32) -> u16 {
        match a & 0x6 {
            0x0 | 0x2 => self.vram.get(self.vram_addr as usize).copied().unwrap_or(0xFFFF),
            0x4 => self.vram_mod as u16,
            _ => (((self.line + 0xF8) << 7) as u16) | (self.aa_counter & 7) as u16,
        }
    }

    fn write_mode(&mut self, v: u16) {
        let had_vblank_reload = self.mode & 0x40 != 0;
        self.mode = v;
        // Geolith: enabling "reload at frame start" during VBlank reloads immediately.
        if !had_vblank_reload && v & 0x40 != 0 && self.line + 0xF8 >= 0x1F0 {
            self.timer = self.timer_reload;
        }
    }

    /// Advance the beam by `mclk` master cycles: timer, IRQs, auto-animation, line renders.
    pub fn advance(&mut self, mut mclk: u64, gfx: &Gfx) {
        self.pixel_frac += mclk;
        self.tick_timer((self.pixel_frac / 4) as u32);   // the timer counts pixels (4 mclk)
        self.pixel_frac %= 4;

        // Event positions inside the 1,536-mclk line: Geolith's CPU cycles 29 / 573 / 712, × 2.
        while mclk > 0 {
            let start = self.line_mclk;
            let run = mclk.min(MCLK_PER_LINE - start);
            let end = start + run;
            let raster = self.line + 0xF8;
            if start <= 58 && end > 58 {
                if raster == 0x1F1 { self.irq.vblank = true; }     // Geolith's line; MAME uses $1F0
                if raster == 0x100 { self.tick_auto_animation(); }
            }
            if start <= 1146 && end > 1146 {
                self.render_line(raster, gfx);
                if raster == 0x1F0 && self.mode & 0x40 != 0 { self.timer = self.timer_reload; }
            }
            if start <= 1424 && end > 1424 { self.line = (self.line + 1) % 264; }
            self.line_mclk = end % MCLK_PER_LINE;
            mclk -= run;
        }
    }

    fn tick_timer(&mut self, pixels: u32) {
        if self.timer > pixels { self.timer -= pixels; return; }
        for _ in 0..pixels {                               // near zero: tick one pixel at a time
            self.timer = self.timer.wrapping_sub(1);
            if self.timer == 0 {
                if self.mode & 0x80 != 0 { self.timer = self.timer_reload; }   // repeat mode
                if self.mode & 0x10 != 0 { self.irq.timer = true; }            // IRQ2 enabled
            }
        }
    }

    fn tick_auto_animation(&mut self) {
        self.aa_prescaler = self.aa_prescaler.wrapping_sub(1);
        if self.aa_prescaler == 0xFF {
            self.aa_counter = (self.aa_counter + 1) & 7;
            self.aa_prescaler = (self.mode >> 8) as u8;
        }
    }

    fn render_line(&mut self, raster: u32, gfx: &Gfx) {
        if !(0x110..0x1F0).contains(&raster) { return; }   // visible lines only
        let rel = raster - 0x100;                           // 0..255 from the top-border start
        let y = (raster - 0x110) as usize;                  // visible row 0..223
        let bank = self.palette_bank * 4096;

        let mut idx = [0u16; 320];
        sprite_line(&self.vram, gfx.l0, gfx.c_rom, self.aa_counter as u32,
                    self.mode & 0x08 != 0, bank, rel, &mut idx);           // §4.6.3
        let backdrop = self.pal_rgba[bank as usize + 0xFFF];
        let row = &mut self.frame[y * 320..(y + 1) * 320];
        for (px, &i) in row.iter_mut().zip(idx.iter()) {
            *px = if i != 0 { self.pal_rgba[i as usize] } else { backdrop };
        }

        // Fix layer on top: palettes 0-15 of the active bank.
        let (frow, fy) = ((rel >> 3) as usize, rel & 7);
        let fix_tiles = (gfx.fix_rom.len() / 32) as u32;
        for col in 0..40usize {
            let entry = self.vram[0x7000 + col * 32 + frow];
            let pal = bank as usize + ((entry >> 12) as usize) * 16;
            let tile = (entry & 0x0FFF) as u32 % fix_tiles;
            for x in 0..8u32 {
                let c = fix_pixel(gfx.fix_rom, tile, x, fy);              // §4.6.8
                if c != 0 { row[col * 8 + x as usize] = self.pal_rgba[pal + c as usize]; }
            }
        }
    }
}
```

`MainBus::advance_video` builds the `Gfx` view from disjoint fields (`&self.cart.c`, the S ROM or SFIX depending on the `REG_CRTFIX` latch, `&self.bios.l0`) and calls `self.lspc.advance(mclk, &gfx)`. Palette writes update `palram` and `pal_rgba` together; `REG_SHADOW` can be applied when converting, or with a second RGBA table.

### 6.7 Sound system

```rust
// crates/neogeo-core/src/sound/mod.rs
#[derive(Default)]
pub struct Mailbox { pub command: u8, pub reply: u8, pub nmi_enabled: bool }

pub struct SoundSystem {
    z80: CrateZ80,
    ram: [u8; 0x800],
    m1: Vec<u8>,                 // cartridge M1; on MVS the board SM1 is chosen by the fix/M1 latch
    banks: [u32; 4],             // ROM offsets of the $F000, $E000, $C000, $8000 windows
    pub mailbox: Mailbox,
    ym: Box<dyn Ym2610>,
    clock: u64,                  // master cycles emulated so far
    ym_acc: u64,
    pub samples: Vec<[i16; 2]>,  // native-rate output, drained by the frontend each frame
}

struct Z80BusView<'a> {
    ram: &'a mut [u8; 0x800],
    m1: &'a [u8],
    banks: &'a mut [u32; 4],
    mailbox: &'a mut Mailbox,
    ym: &'a mut dyn Ym2610,
}

impl Z80Bus for Z80BusView<'_> {
    fn read(&mut self, a: u16) -> u8 {
        let rom = |off: usize| self.m1[off % self.m1.len()];
        match a {
            0x0000..=0x7FFF => rom(a as usize),
            0x8000..=0xBFFF => rom(self.banks[3] as usize + (a as usize & 0x3FFF)),
            0xC000..=0xDFFF => rom(self.banks[2] as usize + (a as usize & 0x1FFF)),
            0xE000..=0xEFFF => rom(self.banks[1] as usize + (a as usize & 0x0FFF)),
            0xF000..=0xF7FF => rom(self.banks[0] as usize + (a as usize & 0x07FF)),
            _ => self.ram[a as usize & 0x7FF],
        }
    }

    fn write(&mut self, a: u16, v: u8) {
        if a >= 0xF800 { self.ram[a as usize & 0x7FF] = v; }
    }

    fn port_in(&mut self, port: u16) -> u8 {
        let bank = (port >> 8) as u32;                      // NEO-ZMC: bank number rides on A15-A8
        match port & 0xFF {
            0x00 => self.mailbox.command,                   // also acknowledges the NMI
            0x04..=0x07 => self.ym.read((port & 3) as u8),
            0x08 => { self.banks[0] = bank * 0x0800; 0 }    // $F000 window, 2 KiB units
            0x09 => { self.banks[1] = bank * 0x1000; 0 }    // $E000 window, 4 KiB units
            0x0A => { self.banks[2] = bank * 0x2000; 0 }    // $C000 window, 8 KiB units
            0x0B => { self.banks[3] = bank * 0x4000; 0 }    // $8000 window, 16 KiB units
            _ => 0,
        }
    }

    fn port_out(&mut self, port: u16, v: u8) {
        match port & 0xFF {
            0x00 | 0xC0 => self.mailbox.command = 0,
            0x04..=0x07 => self.ym.write((port & 3) as u8, v),
            0x08..=0x0B => self.mailbox.nmi_enabled = true,
            0x0C => self.mailbox.reply = v,
            0x18 => self.mailbox.nmi_enabled = false,
            _ => {}
        }
    }
}

impl SoundSystem {
    /// The 68000 wrote REG_SOUND.
    pub fn write_command(&mut self, v: u8) {
        self.mailbox.command = v;
        if self.mailbox.nmi_enabled { self.z80.nmi(); }
    }

    pub fn reply(&self) -> u8 { self.mailbox.reply }

    /// Run the Z80 and YM2610 up to master-clock time `target`.
    pub fn run_until(&mut self, target: u64) {
        while self.clock < target {
            let t_states = {
                let mut bus = Z80BusView {                  // disjoint field borrows
                    ram: &mut self.ram, m1: &self.m1, banks: &mut self.banks,
                    mailbox: &mut self.mailbox, ym: self.ym.as_mut(),
                };
                self.z80.step(&mut bus)
            };
            let mclk = t_states as u64 * Z80_DIV;
            self.clock += mclk;
            self.ym_acc += mclk;
            while self.ym_acc >= YM_SAMPLE_MCLK {           // one sample per 432 mclk (≈55.5 kHz)
                self.ym_acc -= YM_SAMPLE_MCLK;
                let [l, r] = self.ym.clock_sample();
                self.samples.push([l.clamp(-32768, 32767) as i16, r.clamp(-32768, 32767) as i16]);
            }
            self.z80.set_int(self.ym.irq());
        }
    }
}
```

### 6.8 Audio output and frame pacing

The core emits ≈939 stereo samples per MVS frame at ≈55,555 Hz. The frontend resamples to the device rate (usually 48 kHz) and feeds a ring buffer that the `cpal` callback drains. Two clocks are now in play — the emulated 59.19 Hz and the audio device's — so they drift. The standard fix is **dynamic rate control (DRC)**: nudge the resampling ratio by up to ±0.5% to keep the ring buffer about half full. Nobody can hear a 0.5% pitch change, and there are never underruns or growing latency.

```rust
// apps/neogeo-desktop/src/audio.rs
/// Linear-interpolating resampler. Good enough to start; swap in `rubato` later.
#[derive(Default)]
pub struct LinearResampler { pos: f64, prev: [f32; 2] }

impl LinearResampler {
    /// `ratio` = output rate / input rate.
    pub fn process(&mut self, input: &[[i16; 2]], ratio: f64, out: &mut Vec<[f32; 2]>) {
        let step = 1.0 / ratio;
        for s in input {
            let cur = [s[0] as f32 / 32768.0, s[1] as f32 / 32768.0];
            while self.pos < 1.0 {
                let t = self.pos as f32;
                out.push([self.prev[0] + (cur[0] - self.prev[0]) * t,
                          self.prev[1] + (cur[1] - self.prev[1]) * t]);
                self.pos += step;
            }
            self.pos -= 1.0;
            self.prev = cur;
        }
    }
}

/// DRC: `fill` is the ring buffer occupancy, 0.0..=1.0. Returns the ratio to use this frame.
pub fn drc_ratio(device_rate: f64, native_rate: f64, fill: f64) -> f64 {
    (device_rate / native_rate) * (1.0 + 0.005 * (1.0 - 2.0 * fill))
}
```

Frame pacing, simplest first:

1. **Accumulator (start here).** In eframe's `logic`, add the elapsed wall time to an accumulator and run `floor(acc / (1 / 59.1856))` frames; DRC takes care of audio. That is what the skeleton in [§7.6](#76-neogeo-desktop) does.
2. **Audio-driven.** Run a frame whenever the ring buffer drops below a threshold. Perfect audio, slight video judder on 60 Hz displays.
3. **VSync + DRC**, RetroArch-style: one emulated frame per display refresh (59.19 → 60 Hz is a 1.4% speed-up), with DRC absorbing the difference. Smoothest video; add it later behind an option.

### 6.9 Debugger

The debugger is what makes a from-scratch emulator possible. Build these as egui windows behind the `debug-hooks` feature, roughly in this order:

1. **CPU views**: 68000 registers + disassembly around PC (`m68k::dasm`), step / step-over / run-to / breakpoints; the same for the Z80.
2. **Trace buffer**: a ring of the last N instructions (PC, opcode, registers), dumped automatically on watchdog resets, illegal instructions and address errors — most "the game hangs" bugs end in one of these.
3. **Memory viewer**: 68000 space, VRAM, palette RAM, Z80 space, with watchpoints (break on write to an address or range).
4. **Palette viewer**: 2 × 256 × 16 swatches with the raw words.
5. **Sprite table**: SCB2–SCB4 decoded for sprites 1–381 (X, Y, height, sticky, shrink), click to highlight on screen; the sprites touching a chosen line, in draw order, with the 96-limit cut-off marked.
6. **Tile browsers**: C ROM tiles with a chosen palette, fix tiles (S ROM and SFIX), the fix map.
7. **LSPC state**: raster line, timer value/reload/mode, IRQ latches, auto-animation state.
8. **Audio**: per-channel mute and solo for FM 1–4, SSG A–C, ADPCM-A 1–6 and ADPCM-B; the mailbox history (command/reply bytes with timestamps).

Implementation: a `DebugHooks` struct checked before each 68000 step (a sorted `Vec` of breakpoint PCs, or a `u32 → bool` bitmap), and watchpoints checked in the bus write path — each behind `#[cfg(feature = "debug-hooks")]` so release players pay nothing.

### 6.10 Save states, rewind, determinism

- One `SaveState` struct per component, `#[derive(Serialize, Deserialize)]`, serialized with `postcard`. Header: magic, format version, game identity (NGH + CRC32 of the P ROM). ROM data is `#[serde(skip)]` and re-attached after loading.
- **Test it early**: save, run 600 frames, hash the frame; load, run 600 frames, hash — the hashes must match. Run that on every CI build.
- **Rewind**: keep a state every N frames in a `VecDeque` (compress with `lz4_flex` or `zstd` if memory matters).
- **Determinism**: inputs are applied once per frame; the MVS RTC takes its start time from the frontend (injectable, fixed in tests); no `HashMap` iteration in anything that affects emulation. This is also what makes input replays, frame-exact bisecting and `ggrs` rollback netplay possible later.

---

## 7. Scaffolding, step by step

### 7.1 Install prerequisites (macOS)

```bash
# Rust (1.94.1 is already installed) and cargo tools
rustup component add rustfmt clippy rust-src
brew install cargo-nextest cargo-deny cargo-insta samply

# Reference emulator and reverse-engineering tools
brew install mame ghidra rizin z80dasm
brew install --cask imhex

# ngdevkit: open-source Neo Geo SDK (m68k GCC, SDCC for Z80, nullbios, GnGeo)
brew tap dciabrin/ngdevkit          # newer Homebrew may also ask for: brew trust dciabrin/ngdevkit
brew install ngdevkit ngdevkit-gngeo
brew install pkg-config autoconf automake rsync zip imagemagick sox make python
```

Homebrew's `mame` formula installs only the `mame` binary. Tools like `unidasm` need a MAME source build (`make TOOLS=1`). `vasm`/`vlink` (needed to build the diagnostics BIOS from source) are not in Homebrew; build them from [sun.hasenbraten.de/vasm](http://sun.hasenbraten.de/vasm/), or download the diagnostics BIOS's prebuilt zip linked from [its README](https://github.com/jwestfall69/neogeo-diag-bios).

### 7.2 Create the workspace

The repo already exists (`Apache-2.0` license, empty `docs/`). Write the root `Cargo.toml` from [§7.3](#73-root-files) first, then:

```bash
cd ~/rust/neo-geo
mkdir -p crates apps tools scripts roms
cargo new --lib crates/neogeo-core
cargo new --lib crates/neogeo-ym2610
cargo new apps/neogeo-headless
cargo new apps/neogeo-desktop
cargo new tools/neogeo-romtool
```

### 7.3 Root files

`Cargo.toml`:

```toml
[workspace]
resolver = "3"
members = ["crates/*", "apps/*", "tools/*"]

[workspace.package]
edition = "2024"
rust-version = "1.93"                      # m68k 0.13 needs 1.93+
license = "Apache-2.0"
repository = "https://github.com/pmagaz/neo-geo"

[workspace.dependencies]
neogeo-core = { path = "crates/neogeo-core" }
neogeo-ym2610 = { path = "crates/neogeo-ym2610" }
m68k = "=0.13.0"                           # young crate with frequent breaking releases: pin exactly
z80 = "2.0"
ymfm-sys = "0.2"
serde = { version = "1", features = ["derive"] }
postcard = { version = "1", features = ["use-std"] }
thiserror = "2"
anyhow = "1"
log = "0.4"
env_logger = "0.11"
bytemuck = "1"
clap = { version = "4", features = ["derive"] }
zip = "8"
png = "0.18"
crc32fast = "1"
eframe = "0.36"
egui = "0.36"
cpal = "0.18"
gilrs = "0.11"
rtrb = "0.4"
insta = "1"
criterion = "0.8"

[profile.dev]
opt-level = 1                              # an emulator at opt-level 0 is unusably slow

[profile.dev.package."*"]
opt-level = 3                              # optimize dependencies (m68k, egui…) even in dev builds

[profile.release]
lto = "fat"
codegen-units = 1

[profile.profiling]                        # cargo build --profile profiling, then samply
inherits = "release"
debug = true
```

`rust-toolchain.toml`:

```toml
[toolchain]
channel = "1.94"
components = ["rustfmt", "clippy", "rust-src"]
```

`.gitignore` — the current one was copied from another project: it ignores `Cargo.lock` (wrong for a workspace that ships binaries — **commit the lock file**) and carries unrelated paths. Replace it with:

```gitignore
/target/
**/*.rs.bk
.DS_Store
.env
.claude/

# ROMs, BIOS and downloaded test suites: never commit
/roms/
*.neo
*.sp1
*.rom

# Local emulator output
/out/
*.state
```

Add `deny.toml` from [§5.10](#510-licensing-for-an-apache-20-project).

### 7.4 `neogeo-core`

`crates/neogeo-core/Cargo.toml`:

```toml
[package]
name = "neogeo-core"
version = "0.1.0"
edition.workspace = true
rust-version.workspace = true
license.workspace = true

[features]
default = []
debug-hooks = []

[dependencies]
m68k = { workspace = true, features = ["serde"] }
z80.workspace = true
serde.workspace = true
thiserror.workspace = true
log.workspace = true

[dev-dependencies]
insta.workspace = true
crc32fast.workspace = true
```

`src/timing.rs`:

```rust
//! Clock constants. All emulation time is integer master cycles (mclk).
pub const MCLK_MVS: u64 = 24_000_000;
pub const MCLK_AES: u64 = 24_167_829;

pub const MCLK_PER_PIXEL: u64 = 4;
pub const MCLK_PER_LINE: u64 = 1_536;                     // 384 pixels
pub const LINES_PER_FRAME: u64 = 264;
pub const MCLK_PER_FRAME: u64 = MCLK_PER_LINE * LINES_PER_FRAME;   // 405_504

pub const M68K_DIV: u64 = 2;                              // 12 MHz
pub const Z80_DIV: u64 = 6;                               // 4 MHz
pub const YM_DIV: u64 = 3;                                // 8 MHz
pub const YM_SAMPLE_MCLK: u64 = 144 * YM_DIV;             // 432 mclk per output sample

pub const WATCHDOG_MCLK: u64 = 3_244_030;                 // ≈ 8 frames (MAME, mvstech.txt)

pub const FPS_MVS: f64 = MCLK_MVS as f64 / MCLK_PER_FRAME as f64;          // 59.1856
pub const FPS_AES: f64 = MCLK_AES as f64 / MCLK_PER_FRAME as f64;          // 59.5998
pub const YM_RATE_MVS: f64 = MCLK_MVS as f64 / YM_SAMPLE_MCLK as f64;      // 55_555.6 Hz
```

`src/cart/neo.rs`:

```rust
//! TerraOnion .neo loader. Layout verified against Geolith's geo_neo.c.
#[derive(Debug, thiserror::Error)]
pub enum NeoError {
    #[error("not a .neo file (bad magic)")]
    BadMagic,
    #[error("file is truncated")]
    Truncated,
}

pub struct NeoFile {
    pub ngh: u32,
    pub name: String,
    pub p: Vec<u8>,      // converted to 68000 (big-endian) byte order
    pub s: Vec<u8>,
    pub m1: Vec<u8>,
    pub v1: Vec<u8>,     // ADPCM-A
    pub v2: Vec<u8>,     // ADPCM-B (copy of v1 when the file has a single V ROM)
    pub c: Vec<u8>,      // byte-interleaved C ROM pairs
}

impl NeoFile {
    pub fn parse(data: &[u8]) -> Result<Self, NeoError> {
        if data.len() < 4096 || &data[..4] != b"NEO\x01" {
            return Err(NeoError::BadMagic);
        }
        let u32_at = |o: usize| u32::from_le_bytes(data[o..o + 4].try_into().unwrap()) as usize;
        let [p_len, s_len, m_len, v1_len, v2_len, c_len] = [4, 8, 12, 16, 20, 24].map(u32_at);

        let mut off = 4096;
        let mut take = |len: usize| -> Result<Vec<u8>, NeoError> {
            let chunk = data.get(off..off + len).ok_or(NeoError::Truncated)?.to_vec();
            off += len;
            Ok(chunk)
        };
        let mut p = take(p_len)?;
        let s = take(s_len)?;
        let m1 = take(m_len)?;
        let v1 = take(v1_len)?;
        let v2 = if v2_len == 0 { v1.clone() } else { take(v2_len)? };
        let c = take(c_len)?;

        for word in p.chunks_exact_mut(2) {
            word.swap(0, 1);                 // stored word-swapped, like MAME's P ROM dumps
        }
        let name = String::from_utf8_lossy(&data[44..77]).trim_end_matches('\0').trim().to_string();
        Ok(Self { ngh: u32_at(40) as u32, name, p, s, m1, v1, v2, c })
    }
}
```

`src/bios.rs` — the core takes raw bytes; unzipping belongs to the frontends:

```rust
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum System { Aes, Mvs }

pub struct Bios {
    pub rom: Vec<u8>,            // 128 KiB, 68000 byte order
    pub sfix: Option<Vec<u8>>,   // MVS board fix tiles
    pub sm1: Option<Vec<u8>>,    // MVS board sound driver
    pub l0: Vec<u8>,             // generated, §4.6.4
}

impl Bios {
    pub fn new(_system: System, mut rom: Vec<u8>, sfix: Option<Vec<u8>>, sm1: Option<Vec<u8>>) -> Self {
        for word in rom.chunks_exact_mut(2) {
            word.swap(0, 1);
        }
        Self { rom, sfix, sm1, l0: crate::lspc::generate_l0() }
    }
}
```

`src/lib.rs`:

```rust
//! Neo Geo AES/MVS machine. Pure library: no I/O, no threads, no wall clock.
#![forbid(unsafe_code)]

pub mod bios;
pub mod bus;
pub mod cart;
pub mod cpu68k;
pub mod io;
pub mod lspc;
pub mod sound;
pub mod timing;

pub use bios::{Bios, System};
pub use cart::neo::NeoFile;
pub use sound::ym2610::Ym2610;

use bus::MainBus;
use cpu68k::{Cpu68k, CrateM68k};
use timing::{M68K_DIV, MCLK_PER_FRAME};

/// Register-ready input state (active low: 0 = pressed).
#[derive(Clone, Copy)]
pub struct Input { pub p1: u8, pub p2: u8, pub start_select: u8, pub coins: u8 }

pub struct NeoGeo {
    cpu: CrateM68k,
    pub bus: MainBus,
    clock: u64,
    frame_end: u64,
}

impl NeoGeo {
    pub fn new(system: System, bios: Bios, game: NeoFile, ym: Box<dyn Ym2610>) -> Self {
        let mut neo = Self {
            cpu: CrateM68k::new(),
            bus: MainBus::new(system, bios, game, ym),
            clock: 0,
            frame_end: 0,
        };
        neo.power_on();
        neo
    }

    pub fn power_on(&mut self) {
        self.bus.reset();                 // BIOS vectors, banks, latches, LSPC, Z80 + YM2610
        self.cpu.reset(&mut self.bus);    // SSP and PC from the BIOS vector table
        self.bus.lspc.irq.reset = true;   // IRQ3 is pending after a cold boot
    }

    pub fn run_frame(&mut self) { /* §6.4 */ }

    pub fn set_input(&mut self, input: Input) { self.bus.io.input = input; }

    pub fn framebuffer_rgba(&self) -> &[u8] { bytemuck::cast_slice(&self.bus.lspc.frame) }

    pub fn take_audio(&mut self) -> Vec<[i16; 2]> { std::mem::take(&mut self.bus.sound.samples) }
}
```

(Add `bytemuck.workspace = true` to the core's dependencies for `framebuffer_rgba`.)

### 7.5 `neogeo-headless`

The headless runner is the first executable to build and the one CI uses. `apps/neogeo-headless/Cargo.toml` depends on `neogeo-core`, `neogeo-ym2610`, `anyhow`, `clap`, `env_logger`, `zip` and `png`.

```rust
// apps/neogeo-headless/src/main.rs
use anyhow::{Context, Result};
use clap::Parser;
use std::{io::Read, path::{Path, PathBuf}};

#[derive(Parser)]
#[command(about = "Run the Neo Geo core without a window")]
struct Args {
    /// MAME-layout BIOS zip: aes.zip, or ngdevkit's nullbios build of it
    #[arg(long)]
    bios: PathBuf,
    /// Game in .neo format
    #[arg(long)]
    rom: PathBuf,
    #[arg(long, default_value_t = 600)]
    frames: u32,
    /// Save the last frame as a PNG
    #[arg(long)]
    png: Option<PathBuf>,
}

fn main() -> Result<()> {
    env_logger::init();
    let args = Args::parse();

    let bios_rom = read_zip_member(&args.bios, "neo-epo.bin")?;
    let bios = neogeo_core::Bios::new(neogeo_core::System::Aes, bios_rom, None, None);
    let game = neogeo_core::NeoFile::parse(&std::fs::read(&args.rom)?).context("parsing .neo")?;
    let ym = Box::new(neogeo_ym2610::Ymfm::new(game.v1.clone(), game.v2.clone()));
    let mut neo = neogeo_core::NeoGeo::new(neogeo_core::System::Aes, bios, game, ym);

    for _ in 0..args.frames {
        neo.run_frame();
    }
    if let Some(path) = &args.png {
        save_png(path, neo.framebuffer_rgba(), 320, 224)?;
    }
    Ok(())
}

fn read_zip_member(zip_path: &Path, name: &str) -> Result<Vec<u8>> {
    let mut zip = zip::ZipArchive::new(std::fs::File::open(zip_path)?)?;
    let mut file = zip.by_name(name).with_context(|| format!("{name} not in {}", zip_path.display()))?;
    let mut buf = Vec::new();
    file.read_to_end(&mut buf)?;
    Ok(buf)
}

fn save_png(path: &Path, rgba: &[u8], width: u32, height: u32) -> Result<()> {
    let file = std::io::BufWriter::new(std::fs::File::create(path)?);
    let mut encoder = png::Encoder::new(file, width, height);
    encoder.set_color(png::ColorType::Rgba);
    encoder.set_depth(png::BitDepth::Eight);
    encoder.write_header()?.write_image_data(rgba)?;
    Ok(())
}
```

Grow it with `--trace <file>` (one line per 68000 instruction: `PC: disassembly`), `--wav <file>`, `--frame-hash` (print a CRC32 per frame) and `--input <script>` for scripted button presses.

### 7.6 `neogeo-desktop`

`apps/neogeo-desktop/Cargo.toml` depends on `neogeo-core`, `neogeo-ym2610`, `eframe`, `egui`, `cpal`, `rtrb`, `gilrs`, `anyhow`, `log`, `env_logger`.

```rust
// apps/neogeo-desktop/src/main.rs
mod audio;                               // LinearResampler + drc_ratio from §6.8

use eframe::egui;
use neogeo_core::{timing, Input, NeoGeo};

fn main() -> eframe::Result {
    env_logger::init();
    let neo = load_machine_from_args().expect("load BIOS and game");   // same helpers as the headless app
    eframe::run_native(
        "neo-geo",
        eframe::NativeOptions::default(),
        Box::new(|cc| Ok(Box::new(App::new(cc, neo)))),
    )
}

struct App {
    neo: NeoGeo,
    screen: egui::TextureHandle,
    audio: Option<AudioOut>,
    last: std::time::Instant,
    acc: f64,
}

impl App {
    fn new(cc: &eframe::CreationContext<'_>, neo: NeoGeo) -> Self {
        let blank = egui::ColorImage::from_rgba_unmultiplied([320, 224], &vec![0; 320 * 224 * 4]);
        let screen = cc.egui_ctx.load_texture("screen", blank, egui::TextureOptions::NEAREST);
        Self { neo, screen, audio: AudioOut::start().ok(), last: std::time::Instant::now(), acc: 0.0 }
    }
}

impl eframe::App for App {
    // Emulation runs here; eframe calls `logic` once before every `ui`.
    fn logic(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        let now = std::time::Instant::now();
        self.acc = (self.acc + (now - self.last).as_secs_f64()).min(0.1);   // no spiral after a stall
        self.last = now;
        let frame_time = 1.0 / timing::FPS_MVS;
        let mut ran = false;
        while self.acc >= frame_time {
            self.neo.set_input(read_input(ctx));
            self.neo.run_frame();
            let samples = self.neo.take_audio();
            if let Some(audio) = &mut self.audio { audio.push(&samples); }
            self.acc -= frame_time;
            ran = true;
        }
        if ran {
            let img = egui::ColorImage::from_rgba_unmultiplied([320, 224], self.neo.framebuffer_rgba());
            self.screen.set(img, egui::TextureOptions::NEAREST);
        }
        ctx.request_repaint();
    }

    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        egui::CentralPanel::default().show(ui, |ui| {
            let avail = ui.available_size();
            let h = avail.y.min(avail.x * 3.0 / 4.0);                // 4:3 display
            ui.centered_and_justified(|ui| {
                ui.add(egui::Image::new(&self.screen).fit_to_exact_size(egui::vec2(h * 4.0 / 3.0, h)));
            });
        });
        // Debugger windows (§6.9) go here, e.g. egui::Window::new("Sprites").show(ui.ctx(), |ui| …)
    }
}

fn read_input(ctx: &egui::Context) -> Input {
    use egui::Key::*;
    ctx.input(|i| {
        let bit = |key, b: u8| if i.key_down(key) { 0 } else { 1 << b };   // active low
        Input {
            p1: bit(ArrowUp, 0) | bit(ArrowDown, 1) | bit(ArrowLeft, 2) | bit(ArrowRight, 3)
                | bit(Z, 4) | bit(X, 5) | bit(C, 6) | bit(V, 7),
            p2: 0xFF,
            start_select: bit(Enter, 0) | bit(Backspace, 1) | 0xFC,
            coins: 0xFF,
        }
    })
}

struct AudioOut {
    _stream: cpal::Stream,                // must stay alive
    producer: rtrb::Producer<f32>,
    capacity: usize,
    device_rate: f64,
    resampler: audio::LinearResampler,
}

impl AudioOut {
    fn start() -> anyhow::Result<Self> {
        use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
        let device = cpal::default_host()
            .default_output_device()
            .ok_or_else(|| anyhow::anyhow!("no audio output device"))?;
        let config: cpal::StreamConfig = device.default_output_config()?.into();   // f32 on macOS
        let channels = config.channels as usize;
        let device_rate = config.sample_rate as f64;                              // u32 in cpal 0.18
        let capacity = device_rate as usize / 10 * 2;                              // ≈100 ms, stereo
        let (producer, mut consumer) = rtrb::RingBuffer::<f32>::new(capacity);
        let stream = device.build_output_stream::<f32, _, _>(
            config,
            move |out: &mut [f32], _| {
                for frame in out.chunks_mut(channels) {
                    frame[0] = consumer.pop().unwrap_or(0.0);
                    let right = consumer.pop().unwrap_or(0.0);
                    if channels > 1 { frame[1] = right; }
                }
            },
            |e| log::error!("audio stream: {e}"),
            None,
        )?;
        stream.play()?;
        Ok(Self { _stream: stream, producer, capacity, device_rate, resampler: Default::default() })
    }

    fn push(&mut self, samples: &[[i16; 2]]) {
        let fill = 1.0 - self.producer.slots() as f64 / self.capacity as f64;
        let ratio = audio::drc_ratio(self.device_rate, timing::YM_RATE_MVS, fill);
        let mut out = Vec::with_capacity(samples.len() + 16);
        self.resampler.process(samples, ratio, &mut out);
        for [l, r] in out {
            let _ = self.producer.push(l);
            let _ = self.producer.push(r);
        }
    }
}
```

Gamepads: poll `gilrs` in `logic` and OR its buttons into the same active-low bytes.

### 7.7 Fixtures script

Test content is downloaded or built locally, never committed:

```bash
#!/usr/bin/env bash
# scripts/fetch-fixtures.sh
set -euo pipefail
FIX=roms/fixtures
mkdir -p "$FIX"

# CPU conformance suites (large downloads)
[ -d "$FIX/m68000" ] || git clone --depth 1 https://github.com/SingleStepTests/m68000 "$FIX/m68000"
[ -d "$FIX/z80" ]    || git clone --depth 1 https://github.com/SingleStepTests/z80 "$FIX/z80"

# Open-source BIOS + example ROMs (needs `brew install ngdevkit` and the example deps)
[ -d "$FIX/ngdevkit-examples" ] || git clone --recursive https://github.com/dciabrin/ngdevkit-examples "$FIX/ngdevkit-examples"
(
  cd "$FIX/ngdevkit-examples"
  export PATH="$HOMEBREW_PREFIX/opt/python3/bin:$PATH"
  autoreconf -iv && ./configure && gmake
)
```

ngdevkit's `nullbios` produces MAME-named files (`sp-s2.sp1`, `neo-epo.bin`, `neo-po.bin`, `sfix.sfix`, `sm1.sm1`, `000-lo.lo`) packed into `aes.zip` and `neogeo.zip`, so the emulator can load its BIOS exactly as it will load the real ones. The examples are built as MAME-style ROM sets (they borrow an existing game's set name); `neogeo-romtool` should turn them into `.neo` files (header + P word-swap + C pair interleave) — about 100 lines, and the same code you need for any raw ROM dump.

---

## 8. Roadmap and milestones

Each milestone ends with something you can see or test. Sound comes late on purpose: the AES BIOS doesn't need it, and video bugs are far easier to find than audio ones.

| # | Milestone | Build | Done when |
|---|---|---|---|
| M0 | Foundations | Workspace ([§7](#7-scaffolding-step-by-step)); CI running `cargo fmt --check`, `cargo clippy -- -D warnings`, `cargo nextest run`, `cargo deny check`; fixtures script; MAME and ngdevkit installed | CI is green on the empty core |
| M1 | 68000 wired | `Cpu68k` + `m68k` adapter, a flat test bus, disassembly trace in `neogeo-headless` | A hand-assembled test program runs and the trace prints PC + disassembly |
| M2 | BIOS boots, no video | Memory map, BIOS byte swap, vector swap, work RAM, I/O stubs, IRQ latches + `REG_IRQACK`, VBlank IRQ1, watchdog | nullbios, then `neo-epo.bin`, run 60+ frames without a watchdog reset; the PC trace matches MAME's for the first several thousand instructions |
| M3 | First pixels | Palette RAM + conversion, VRAM registers + modulo, fix layer, backdrop | `01-helloworld` and `04-palette` match golden PNGs; the diagnostics BIOS menu is readable |
| M4 | Sprites | SCB1–4, chains, the 96 limit, flips, both shrinks, auto-animation, generated L0 | `02-sprite`, `03-sprite-animation`, `05-scrolling` match goldens; the BIOS eye-catcher animates |
| M5 | Games, silent | `.neo` loader, P bank switching, inputs, `REG_CRTFIX`, soft DIPs | `12`–`14` (P ROM sizes, bank switching) pass; early titles such as NAM-1975, Magician Lord and Metal Slug are playable without sound |
| M6 | Raster timer | IRQ2 with all reload modes, the line counter in `REG_LSPCMODE` | `09-horizontal-sync` matches; Neo Turf Masters' ground and Riding Hero's road look right |
| M7 | Sound | Z80 (crate), ZMC banks, ports, mailbox, NMI; YM2610 via `ymfm-sys`; resampler + `cpal` | `06`, `15`, `16` sound right; the BIOS gets its `$01` reply; the eye-catcher jingle plays |
| M8 | Usable frontend | Gamepads, fullscreen, 304 px crop, save states, rewind, screenshots, config file | A save-state round trip gives identical frame hashes over 600 frames |
| M9 | MVS and Universe BIOS | Backup RAM + lock, uPD4990A RTC, coins, service, DIPs, memory card | The MVS BIOS boots without errors; bookkeeping and high scores persist; Universe BIOS menus work |
| M10 | Late boards | Fix bank switching, NEO-SMA, NEO-PVC, PRO-CT0, KOF98, MSLUGX (ported from Geolith, with attribution) | Garou, Metal Slug 3, KOF 2000/2003, SVC and Metal Slug 5 boot and play |
| M11 | Own cores, accuracy | Your own Z80 (SingleStepTests + ZEXALL), a native Rust YM2610, event timing reviewed against the MiSTer Verilog, the NONBUGS list | Swapping in your cores changes no golden frame, and reference audio stays within tolerance |
| M12 | Optional | libretro core, Neo Geo CD, wasm build, `ggrs` rollback netplay | — |

---

## 9. Testing and debugging strategy

### 9.1 CPU conformance

- **68000.** The `m68k` crate already runs the full SingleStepTests suite in its own CI; you don't need to rerun it. If you write your own core, start from the approach of m68k-rs's `tests/singlestep_m68000_v1_tests.rs` (MIT). The [SingleStepTests/m68000](https://github.com/SingleStepTests/m68000) files are `.json.bin` (convert them with the repo's `decode.py`), RAM is given in 16-bit words, and the initial state includes the 68000's two-word prefetch queue. The TAS and TRAPV files are flagged as unreliable.
- **Z80.** [SingleStepTests/z80](https://github.com/SingleStepTests/z80): JSON, 1,000 tests per opcode, with registers, RAM, per-cycle bus activity and port traffic. A `serde` harness is short:

```rust
#[derive(serde::Deserialize)]
struct Case {
    name: String,
    initial: CpuState,
    #[serde(rename = "final")]
    expected: CpuState,
    #[serde(default)]
    ports: Vec<(u16, u8, String)>,        // (address, value, "r" | "w")
}

#[derive(serde::Deserialize)]
struct CpuState {
    pc: u16, sp: u16, a: u8, b: u8, c: u8, d: u8, e: u8, f: u8, h: u8, l: u8,
    i: u8, r: u8, ix: u16, iy: u16, wz: u16, af_: u16, bc_: u16, de_: u16, hl_: u16,
    im: u8, iff1: u8, iff2: u8,
    ram: Vec<(u16, u8)>,
}
```

  Check registers and RAM first, then the cycle count (the length of the `cycles` array), then per-cycle bus activity if you go cycle-exact. Add ZEXDOC/ZEXALL: they are CP/M programs, so trap `CALL 5` to print their BDOS output.

### 9.2 The open-source test-ROM ladder

ngdevkit's examples line up almost one-to-one with the milestones. They build against ngdevkit's open-source BIOS, so CI can run them without a single copyrighted byte:

| ngdevkit example | Exercises (going by its name) | Milestone |
|---|---|---|
| `01-helloworld` | Fix layer, palettes, VBlank IRQ, BIOS hand-off | M3 |
| `02-sprite` | SCB1–4 | M4 |
| `03-sprite-animation` | Sprite tile animation | M4 |
| `04-palette` | Palette RAM, colour format | M3 |
| `05-scrolling` | Sprite positioning, chains | M4 |
| `06-sound-adpcma` | Z80, mailbox, ADPCM-A | M7 |
| `07-attract-and-game` | BIOS attract / game flow | M5 |
| `08-software-dips` | Soft DIPs from the header | M5 / M9 |
| `09-horizontal-sync` | Timer IRQ2, raster effects | M6 |
| `10-credits-management` | Coins and credits (MVS) | M9 |
| `11-backup-ram` | Backup RAM | M9 |
| `12-prom-1mb`, `13-prom-full-2mb`, `14-prom-bankswitch` | P ROM layouts, bank switching | M5 |
| `15-sound-adpcmb` | ADPCM-B | M7 |
| `16-sound-music` | FM, SSG, YM2610 timers | M7 |
| `18-memory-card` | Memory card | M9 |

Read each example's source for what it really touches.

The **[diagnostics BIOS](https://github.com/jwestfall69/neogeo-diag-bios)** (a disassembly of smkdan's diag BIOS, extended) replaces the system ROM and M1 and tests work RAM, backup RAM, palette RAM, VRAM and 68000↔Z80 communication, reporting on the fix layer only. It is ideal at M3, and again at M7 for the Z80 tests. Because it was written to find faults in real hardware, any failure it reports in your emulator is a real bug.

### 9.3 Trace diffing against MAME

MAME's debugger can log every executed instruction:

```bash
printf 'trace boot.tr,maincpu,noloop\ngo\n' > trace.cmd
mame mslug -window -debug -debugscript trace.cmd -seconds_to_run 5
```

(`noloop` turns off MAME's loop folding, so every instruction is logged; use `audiocpu` instead of `maincpu` for the Z80.) Make `neogeo-headless --trace` print the same `PC: disassembly` format and compare the PC column first:

```bash
diff <(cut -d: -f1 boot.tr | tr a-f A-F) <(cut -d: -f1 mine.tr | tr a-f A-F) | head
```

Caveats: both sides must start from the same state (same BIOS, same system type, same RAM contents), and the traces drift apart as soon as interrupt timing differs by an instruction. Diffing is most useful from reset to the first few VBlanks, and between breakpoints on a specific routine you are debugging.

### 9.4 Golden frames

```rust
#[test]
fn helloworld_frame_300() {
    let Some(mut neo) = fixture_machine("ngdevkit/01-helloworld.neo") else { return };   // skip without fixtures
    for _ in 0..300 {
        neo.run_frame();
    }
    insta::assert_snapshot!(format!("{:08x}", crc32fast::hash(neo.framebuffer_rgba())));
}
```

- Save the matching PNG next to the snapshot so a human can look at it, and review changes with `cargo insta review`.
- Tests that need fixtures should *skip*, not fail, when `roms/fixtures` is missing, so a checkout without fixtures stays green. Give CI a job that builds the fixtures first.
- Add input scripts ("press A at frame 120") to get past title screens deterministically.

### 9.5 Audio

- **VGM logs.** [vgmrips.net](https://vgmrips.net) has Neo Geo soundtrack packs. VGM files record the YM2610's register writes (commands `0x58`/`0x59` for its two ports) plus its ADPCM ROM data (data blocks `0x82` for ADPCM-A, `0x83` for ADPCM-B). A small `neogeo-romtool vgm` player that drives your `Ym2610` implementation and writes a WAV tests the sound chip with no Z80 and no game involved.
- **A/B against ymfm.** Feed the same register stream to your native YM2610 and to `ymfm-sys`. When porting ymfm, aim for sample-exact output; otherwise compare RMS error and spectrograms (`sox`, Audacity).
- **Per-channel mute** in the debugger isolates FM vs SSG vs ADPCM problems quickly.

### 9.6 Compatibility tracking

Keep `docs/COMPATIBILITY.md` with one row per game: name, NGH, board type, status (boots / in-game / playable / perfect), known issues, last tested commit. Work through the library roughly in release order: early games are simple, late games exercise the protection boards. When in doubt compare with Geolith and MAME, and check [§4.11](#411-real-hardware-quirks-you-must-not-fix) before "fixing" anything.

### 9.7 Performance

- Per frame: 202,752 68000 cycles (≈20,000 instructions), ≈8,000 Z80 instructions, 224 rendered lines. A plain interpreter in Rust needs a small fraction of one core for that; performance won't matter for a long time.
- Profile with `cargo build --profile profiling`, then `samply record target/profiling/neogeo-headless --frames 3000 …`.
- Keep a `criterion` benchmark ("600 frames of a fixed demo") to catch regressions.

---

## 10. Tools checklist

| Tool | Get it | Use |
|---|---|---|
| Rust 1.94 + rustfmt/clippy | rustup | Build |
| cargo-nextest, cargo-deny, cargo-insta | `brew install …` | Tests, license checks, snapshot review |
| samply | `brew install samply` | Profiling (Firefox Profiler UI) |
| MAME | `brew install mame` | Reference behaviour, debugger, instruction traces |
| Geolith | RetroArch core (`geolith_libretro`) or source | The reference you port from; A/B comparisons |
| FinalBurn Neo | RetroArch core | Second opinion |
| MiSTer NeoGeo core | GitHub | Timing ground truth (Verilog) |
| ngdevkit + GnGeo | `brew tap dciabrin/ngdevkit` | Test ROMs in C/asm, the open-source BIOS, GnGeo with GDB remote debugging |
| Lithogen | Build from source (autotools) | MAME set → `.neo` |
| vasm / vlink | Build from source | The diagnostics BIOS; hand-written 68000/Z80 test ROMs |
| Ghidra | `brew install ghidra` | Disassemble BIOS and game code (68000 and Z80) |
| rizin | `brew install rizin` | Quick command-line disassembly |
| z80dasm | `brew install z80dasm` | Z80 disassembly of M1 ROMs |
| ImHex | `brew install --cask imhex` | Hex editor with a pattern language (write a `.neo` header pattern) |
| sox, Audacity | `brew install sox`, `brew install --cask audacity` | Audio comparison |

Documents worth keeping open: the NeoGeo Development Wiki; Motorola's M68000 Programmer's Reference Manual and MC68000 User's Manual (instruction timing tables); Zilog's Z80 CPU User Manual and Sean Young's *The Undocumented Z80 Documented*; the translated YM2610 datasheet; Geolith's and MAME's sources; Copetti's *Neo Geo Architecture*.

---

## 11. ROMs, BIOS and legal hygiene

- Game ROMs and SNK's BIOS images are copyrighted. The emulator must never ship or download them, and the repo must never contain them (`roms/` is gitignored).
- Legal sources: dumps of cartridges and BIOS chips you own; digital purchases that include ROMs (the MiSTer core documents loading ROMs from GOG releases; check each release's terms); the Universe BIOS 4.0 (free for personal use); homebrew whose authors allow it.
- Fully open test content for CI: ngdevkit's nullbios and the examples you build, the diagnostics BIOS, SingleStepTests, ZEXALL.
- Verify dumps against MAME's `hash/neogeo.xml` (CRC32 and SHA-1 per ROM), or the checksums Geolith was written against (the "LunaGarlic" set; see its `rename-neo.sh`).
- Convert with Lithogen: `lithogen mslug.zip` → `mslug.neo` (it finds a clone's parent zip automatically).

---

## 12. Gotchas checklist

**Loading**

1. P ROM and BIOS images are stored with every 16-bit word byte-swapped. Swap once at load, then read big-endian.
2. `.neo` with `V2 size = 0` means one V ROM serves both ADPCM-A and ADPCM-B.
3. After reset the vector table comes from the BIOS: SSP and PC are the BIOS's, not the cartridge's.

**Interrupts and timing**

4. IRQ3 is pending at power-on.
5. The IRQ latches are cleared only by `REG_IRQACK` (bit 2 VBlank, bit 1 timer, bit 0 IRQ3), never by the CPU's acknowledge cycle.
6. The timer counts **pixels** (6 MHz, one tick per 2 CPU cycles), not lines. Reload values below 5 flood the CPU.
7. Keep all time as integer master cycles; floating-point time drifts.
8. Writes to `$300001` kick the watchdog. A missing watchdog hides hung games; a wrong one resets working games. Make it switchable in the debugger.

**Sound**

9. Z80 banks are selected by port **reads**, with the bank number in A15–A8.
10. Z80 NMIs are off after reset until port `$08` is written; reading port `$00` acknowledges one.
11. The Z80 powers up in IM 0: present `$FF` on the data bus for INT.
12. Until the Z80 exists, fake the BIOS handshake: when the 68000 writes `$01` to `REG_SOUND`, put `$01` in the reply latch. That avoids "Z80 ERROR" on MVS.
13. The YM2610 timers drive the music. No timers, no music.

**Video**

14. The VRAM address modulo wraps inside its zone (`$0000–$7FFF` or `$8000–$87FF`).
15. LSPC byte writes to even addresses land in both halves of the word.
16. Palette byte writes: the wiki says the byte lands in both halves (the /WE lines are tied together); Geolith writes only the addressed half. Games rarely do it — log it, and decide when one does.
17. Sprite 0 is never drawn; only 1–381, later ones on top.
18. Chained sprites inherit Y, height and vertical shrink, but not horizontal shrink. X advances by the previous sprite's `hshrink + 1`.
19. Sprite height 33 means 32 tiles with looping borders when shrunk.
20. Enforce 96 sprites per line; some games rely on it.
21. Sprite Y: first line = `496 − Y` from the top of the visible area; X and Y wrap at 512.
22. The fix map is column-major, fix tiles only use palettes 0–15, the low nibble is the left pixel, and the column pairs are stored in the order `$10, $18, $00, $08`.
23. C ROM rows: right half first, bit 0 = leftmost pixel, interleaved byte order = bitplanes `[0, 2, 1, 3]`.
24. The backdrop is the last colour of the active bank; `$400000` must stay black.

**System**

25. AES vs MVS: `REG_STATUS_B` bit 7. The AES has no SFIX/SM1 (always the cartridge S/M1), no RTC and no backup RAM.
26. The MVS BIOS needs the uPD4990A's 1 Hz time pulse; a wrong stub produces clock/calendar errors.
27. Split long accesses into two word accesses (high word first), so I/O side effects happen in a sensible order.
28. Don't "fix" real-hardware quirks ([§4.11](#411-real-hardware-quirks-you-must-not-fix)).

---

## 13. References

**Hardware documentation**

- NeoGeo Development Wiki: <https://wiki.neogeodev.org>. Pages used: Display timing, Timer interrupt, Clock, Framerate, Watchdog, 68k memory map, Memory mapped registers, 68k interrupts, Vector table swap, 68k program header, System ROM, SFIX ROM, SM1, P ROM, V ROM, Bankswitching, VRAM, Sprites, Sticky bit, Sprite shrinking, L0 ROM, Auto animation, Fix layer, Palettes, Colors, Sprite graphics format, Fix graphics format, Z80, Z80 port map, Z80 bankswitching, Z80 interrupts, 68k/Z80 communication, YM2610, Z80/YM2610 interface, FM, SSG, ADPCM, NEO-CMC, NEO-SMA, NEO-PVC, NEO-PCM2, NEO-ZMC2, ROM-only boards.
- Rodrigo Copetti, *Neo Geo Architecture — A Practical Analysis*: <https://www.copetti.org/writings/consoles/neogeo/>
- Furrtek's FPGA core (chip-level Verilog, GPL-2.0): <https://github.com/MiSTer-devel/NeoGeo_MiSTer>
- Charles MacDonald's `mvstech.txt` (quoted throughout the wiki).
- Motorola M68000 Programmer's Reference Manual: <https://www.nxp.com/docs/en/reference-manual/M68000PRM.pdf>
- Motorola MC68000 User's Manual (timing): <https://www.nxp.com/docs/en/reference-manual/MC68000UM.pdf>
- Zilog Z80 CPU User Manual: <https://www.zilog.com/docs/z80/um0080.pdf>
- YM2610 datasheet (translated, via the wiki): <http://furrtek.free.fr/noclass/neogeo/YM2610.pdf>

**Reference emulators**

- Geolith (BSD-3-Clause): upstream <https://gitlab.com/jgemu/geolith>, libretro port <https://github.com/libretro/geolith-libretro>. Most useful files: `src/geo.c` (scheduler), `geo_m68k.c` (memory map, boards), `geo_lspc.c` (video), `geo_z80.c`, `geo_ymfm.c`, `geo_neo.c` (`.neo` + board detection), `geo_rtc.c`, `NONBUGS`.
- MAME Neo Geo driver (BSD-3-Clause files): <https://github.com/mamedev/mame/tree/master/src/mame/snk> (`neogeo.cpp`, `neogeo.h`, `neogeo_spr.cpp`, `neogeo_v.cpp`); protection devices in `src/devices/bus/neogeo/`.
- FinalBurn Neo: <https://github.com/finalburnneo/FBNeo>
- jgenesis (Rust, GPL-3.0; 68000, Z80 and YM2612 for the Mega Drive): <https://github.com/jsgroth/jgenesis>

**Rust crates**

- 68000: [`m68k`](https://github.com/benletchford/m68k-rs) · [`m68000`](https://github.com/Stovent/m68000) · [`r68k`](https://crates.io/crates/r68k)
- Z80: [`z80`](https://github.com/kirjavascript/z80) (port of [jgz80](https://github.com/carmiker/jgz80)) · [`iz80`](https://github.com/ivanizag/iz80) · [`cell80-z80`](https://crates.io/crates/cell80-z80)
- Sound: [ymfm](https://github.com/aaronsgiles/ymfm) · [`ymfm-sys`](https://github.com/h1romas4/ymfm-sys)
- Frontend: [egui / eframe](https://github.com/emilk/egui) · [`cpal`](https://github.com/RustAudio/cpal) · [`rtrb`](https://github.com/mgeier/rtrb) · [`gilrs`](https://gitlab.com/gilrs-project/gilrs) · [`pixels`](https://github.com/parasyte/pixels) · [`rubato`](https://crates.io/crates/rubato)
- State: [`postcard`](https://github.com/jamesmunns/postcard) · [bincode's tombstone notice](https://lib.rs/crates/bincode)

**Test content and tools**

- SingleStepTests: [68000](https://github.com/SingleStepTests/m68000) · [Z80](https://github.com/SingleStepTests/z80) · [original TomHarte ProcessorTests](https://github.com/TomHarte/ProcessorTests)
- ngdevkit: <https://github.com/dciabrin/ngdevkit> · examples: <https://github.com/dciabrin/ngdevkit-examples> · L0 generator: `nullbios/zoom-rom.py`
- Diagnostics BIOS: <https://github.com/jwestfall69/neogeo-diag-bios>
- Lithogen (MAME → `.neo`): <https://github.com/carmiker/lithogen>
- Universe BIOS: <http://unibios.free.fr/download.html>
- vgmrips (VGM soundtrack logs): <https://vgmrips.net>
- vasm / vlink: <http://sun.hasenbraten.de/vasm/> · <http://sun.hasenbraten.de/vlink/>
