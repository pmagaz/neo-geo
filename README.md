# Neo Geo game

A Neo Geo AES/MVS cartridge game, built from source with
[ngdevkit](https://github.com/dciabrin/ngdevkit).

A title screen, a character who walks, jumps, crouches and attacks, a
three-layer parallax stage that scrolls endlessly, and sound.

## Install

Once per machine. Everything comes from Homebrew, and no copyrighted ROMs are
needed: ngdevkit ships an open source BIOS, which the build copies next to the
cartridge for you.

```sh
./install.sh            # install what is missing, then write config.mk
./install.sh --check    # report what is missing, change nothing
```

## Build and run

```sh
./launch.sh     # build and run
```

`launch.sh` only builds and runs — it never installs or configures. If a tool
is missing it says so and points back at `./install.sh`, because the failures
otherwise appear a long way from their cause: make runs an empty tool path as
a command, and a missing `sox` once surfaced as the sample packer complaining
about absent WAV files.

It also kills any emulator left over from a previous run before starting a
new one. Stacked windows are easy to create and very confusing to debug, since
the oldest window keeps running an old build and fixes appear to do nothing.

| Command | What it does |
|---|---|
| `./launch.sh` | Build and run as an AES (home console) |
| `./launch.sh mvs` | Build and run as an MVS (arcade) |
| `./launch.sh mame` | Run in MAME instead (needs `brew install mame`) |
| `./launch.sh --no-build` | Launch what is already built |
| `gmake` | Build only |
| `gmake clean` | Remove compiled objects |
| `gmake distclean` | Remove the whole `build/` directory |

## Controls

| Key | Action |
|---|---|
| `W` `A` `S` `D` | Move |
| `J` `K` `L` `I` | Buttons A B C D |
| `Enter` | Start |
| `5` | Insert coin (MVS) |
| `Esc` | GnGeo menu |

Movement is on letter keys rather than the arrows because GnGeo's built-in
defaults use SDL 1.2 keycodes (`UP=K273`), while this build runs on SDL2 via
`sdl2-compat`, where the arrow keys moved to `1073741903`-`1073741906`. Passing
those values explicitly did not work either, so the mapping in `Makefile` uses
letter keys, whose ASCII codes are the same under both SDL versions.

## Layout

| Path | What it is |
|---|---|
| `main.c` | The game |
| `assets/` | Source art. `square.gif` is the 16x16 sprite tile |
| `rom.mk` | Cartridge layout: which ROM chips exist and how big |
| `Makefile` | Which assets go into which ROM chip |
| `build.mk`, `emu.mk` | ngdevkit's generic build and emulator rules |
| `configure` | Writes `config.mk` (machine-specific, not committed) |
| `build/rom/` | The built cartridge, plus the BIOS |
| `docs/` | Research notes on emulating the hardware |

The cartridge is split across chips the way real hardware is: `-p1` is the
68000 program, `-c1`/`-c2` the sprite tiles, `-s1` the 8x8 text tiles, `-m1` the
Z80 sound driver and `-v1` the ADPCM samples.

## Licensing note

The game links against ngdevkit's runtime, which is LGPL-3.0, and `build.mk`,
`emu.mk` and `setup/ngdevkit-assets/` come from ngdevkit's example project under
the same licence. The `LICENSE` file at the root (Apache-2.0) was chosen for the
emulator research in `docs/`; settle the licence for the game itself before
distributing a binary.
