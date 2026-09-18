# Neo Geo game - build with `gmake`, run with `gmake gngeo`.

all: cart bios

# location of generated and compiled content
BUILDDIR=build
# directories scanned for source to compile
SRCDIRS=assets
CFLAGS=-I$(BUILDDIR) -Iassets -std=c99 -fomit-frame-pointer -O2 -g
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
$(CROM1): $(BUILDDIR)/assets/hero.c1
$(CROM2): $(BUILDDIR)/assets/hero.c2
$(CROM1): $(BUILDDIR)/assets/stage.c1
$(CROM2): $(BUILDDIR)/assets/stage.c2

# Regenerate the tile sheet and its palette from the source art. Every
# animation shares one palette, so they must be converted in one go.
SHEET=assets/dfbf14334572aaf4dccdf18cf2a1a234.png
assets/hero.gif assets/hero.h: $(SHEET) tools/sheet2neo.py
	PYTHONPATH=tools $(PYTHON) tools/sheet2neo.py $(SHEET) \
	    -o assets/hero.gif --header assets/hero.h --name hero \
	    --anim walk:1 --anim attack:2 --anim jump:4:4-8 --anim crouch:5:2-4

# The stage is drawn rather than converted, since the Neo Geo has no
# background layer and it has to be built from sprite tiles anyway.
assets/stage.gif assets/stage.h: tools/make_stage.py
	PYTHONPATH=tools $(PYTHON) tools/make_stage.py \
	    -o assets/stage.gif --header assets/stage.h --name stage

$(BUILDDIR)/main.o: assets/hero.h assets/stage.h


# sound driver ROM: ngdevkit's stock driver, enough to satisfy the BIOS ---
SOUND_DRIVER=$(BUILDDIR)/assets/base-sound-driver.ihx
$(MROM1): $(SOUND_DRIVER)


# sample ROM: no audio samples yet, so the V ROM stays empty --------------


# assets/square.gif is committed, so nothing to pre-generate here.
CUSTOM_GENERATE_TARGETS=
