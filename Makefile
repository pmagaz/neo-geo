# Neo Geo game - build with `gmake`, run with `gmake gngeo`.

all: cart bios

# location of generated and compiled content
BUILDDIR=build
# directories scanned for source to compile
SRCDIRS=assets src
CFLAGS=-I$(BUILDDIR) -Iassets/images/sprites -Iassets/images/stages -std=c99 -fomit-frame-pointer -O2 -g
LDFLAGS=
Z80FLAGS=
Z80LDFLAGS=

# paths to the ngdevkit toolchain on this machine
include config.mk

# cartridge layout
GAMEROM=square
GAMETITLE=Moving square
include rom.mk

# generic build rules (68k, Z80, assets, run)
include build.mk

# Keyboard mapping for GnGeo.
#
# GnGeo's built-in defaults use SDL 1.2 keycodes (UP=K273 and friends) but this
# build runs on SDL2, where the arrow keys moved to 1073741903-906. Letter keys
# kept their ASCII codes either way, so movement is bound to W/A/S/D, which
# works regardless of which SDL version is underneath.
#
# W=119 A=97 S=115 D=100 | buttons J=106 K=107 L=108 I=105 | Enter=13 5=53
EXTRAOPTS+=--p1control="UP=K119,LEFT=K97,DOWN=K115,RIGHT=K100,A=K106,B=K107,C=K108,D=K105,START=K13,COIN=K53"

# emulator targets: gngeo, gngeo-mvs, mame, mame-mvs
include emu.mk


# program ROM -----------------------------------------------------------
ELF=$(BUILDDIR)/rom.elf
$(ELF): $(BUILDDIR)/main.o
$(PROM1): $(ELF)


# fix ROM: the 8x8 text tiles ngdevkit provides ---------------------------
$(SROM1): $(BUILDDIR)/assets/base-srom-text-shadow.fix


# sprite ROM: BIOS eye-catcher tiles (0-255), then our own ----------------
# The hero's animations land at tile 256.
$(CROM1): $(BUILDDIR)/assets/base-crom-logo.c1
$(CROM2): $(BUILDDIR)/assets/base-crom-logo.c2
$(CROM1): $(BUILDDIR)/assets/images/sprites/hero.c1
$(CROM2): $(BUILDDIR)/assets/images/sprites/hero.c2
$(CROM1): $(BUILDDIR)/assets/images/stages/stage-sky.c1
$(CROM2): $(BUILDDIR)/assets/images/stages/stage-sky.c2
$(CROM1): $(BUILDDIR)/assets/images/stages/stage-hills.c1
$(CROM2): $(BUILDDIR)/assets/images/stages/stage-hills.c2
$(CROM1): $(BUILDDIR)/assets/images/stages/stage-ground.c1
$(CROM2): $(BUILDDIR)/assets/images/stages/stage-ground.c2

# Regenerate the tile sheet and its palette from the source art. Every
# animation shares one palette, so they must be converted in one go.
SHEET=assets/images/sprites/hero-sheet.png
PREPPED=$(BUILDDIR)/assets/hero-sheet.png

# The sheet arrives as a JPEG with the transparency checkerboard painted into
# it, so the backdrop is keyed out and the art scaled down before conversion.
# Depends on the makefile too, so changing --height actually rebuilds it.
$(PREPPED): $(SHEET) tools/prep_sheet.py Makefile | $(BUILDDIR)/assets
	$(PYTHON) tools/prep_sheet.py $(SHEET) -o $@ --height 64

assets/images/sprites/hero.gif assets/images/sprites/hero.h: $(PREPPED) tools/sheet2neo.py tools/neogeo_color.py
	PYTHONPATH=tools $(PYTHON) tools/sheet2neo.py $(PREPPED) \
	    -o assets/images/sprites/hero.gif \
	    --header assets/images/sprites/hero.h --name hero \
	    --anim walk:1 --anim attack:2 --anim jump:4:4-8 --anim crouch:5:2-4

# The stage is drawn rather than converted, since the Neo Geo has no
# background layer and it has to be built from sprite tiles anyway.
STAGE_LAYERS=$(addprefix assets/images/stages/,stage-sky.gif stage-hills.gif stage-ground.gif)
$(STAGE_LAYERS) assets/images/stages/stage.h: tools/make_stage.py tools/neogeo_color.py
	PYTHONPATH=tools $(PYTHON) tools/make_stage.py \
	    --outdir assets/images/stages \
	    --header assets/images/stages/stage.h --name stage

$(BUILDDIR)/main.o: assets/images/sprites/hero.h assets/images/stages/stage.h


# sound driver ROM: the Z80 program ---------------------------------------
#
# The 68000 cannot reach the sound chip. It writes a command byte to
# REG_SOUND, which fires an NMI on the Z80, and the Z80 plays the sample.
# src/user_commands.s is the table of what each command number means.
SOUND_DRIVER=$(BUILDDIR)/game-sound-driver.ihx
$(MROM1): $(SOUND_DRIVER)
$(SOUND_DRIVER): $(BUILDDIR)/src/user_commands.rel

# Both of these exist because the "generate" pass runs once and records that
# it is done, so a build/ carried over from an older checkout never produces
# them again. Depending on them directly makes the build correct whatever
# state build/ is in:
#
#   the directory, or the assembler cannot write its listing file
#   samples.inc, or the .include on line 44 of user_commands.s fails
$(BUILDDIR)/src/user_commands.rel: $(BUILDDIR)/assets/samples.inc | $(BUILDDIR)/src


# sample ROM: the ADPCM-A sound effects ------------------------------------
#
# The YM2610's ADPCM-A channels run at a fixed 18.5 kHz, so the sources are
# resampled to that and made mono. The silence filters trim the dead air at
# each end - generously at the tail, so a sound's decay is not clipped off.
SFX=coin-pickup jump punch
SFXWAV=$(SFX:%=$(BUILDDIR)/assets/sfx/%.wav)

$(BUILDDIR)/assets/sfx/%.wav: assets/sound/%.mp3 | $(BUILDDIR)/assets
	mkdir -p $(dir $@)
	$(SOX) -V1 $< -c 1 -r 18500 $@ \
	    silence 1 0.01 0.1% reverse silence 1 0.15 0.03% reverse

$(VROM1): assets/sound/samples-map.yaml


# Sample offsets have to exist before the Z80 source is assembled, so they
# are generated in the "generate" pass that runs ahead of the build proper.
CUSTOM_GENERATE_TARGETS=generate-sfx
generate-sfx: $(BUILDDIR)/assets/samples.inc

$(BUILDDIR)/assets/samples.inc: assets/sound/samples-map.yaml $(SFXWAV)
	$(VROMTOOL) --asm -s $(VROMSIZE) $< -o $(VROM1) -m $@
