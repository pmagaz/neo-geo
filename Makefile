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
$(CROM1): $(BUILDDIR)/assets/stage-sky.c1
$(CROM2): $(BUILDDIR)/assets/stage-sky.c2
$(CROM1): $(BUILDDIR)/assets/stage-hills.c1
$(CROM2): $(BUILDDIR)/assets/stage-hills.c2
$(CROM1): $(BUILDDIR)/assets/stage-ground.c1
$(CROM2): $(BUILDDIR)/assets/stage-ground.c2

# Regenerate the tile sheet and its palette from the source art. Every
# animation shares one palette, so they must be converted in one go.
SHEET=assets/Gemini_Generated_Image_6x6ayw6x6ayw6x6a.jpeg
PREPPED=$(BUILDDIR)/assets/hero-sheet.png

# The sheet arrives as a JPEG with the transparency checkerboard painted into
# it, so the backdrop is keyed out and the art scaled down before conversion.
# Depends on the makefile too, so changing --height actually rebuilds it.
$(PREPPED): $(SHEET) tools/prep_sheet.py Makefile | $(BUILDDIR)/assets
	$(PYTHON) tools/prep_sheet.py $(SHEET) -o $@ --height 64

assets/hero.gif assets/hero.h: $(PREPPED) tools/sheet2neo.py tools/neogeo_color.py
	PYTHONPATH=tools $(PYTHON) tools/sheet2neo.py $(PREPPED) \
	    -o assets/hero.gif --header assets/hero.h --name hero \
	    --anim walk:0 --anim attack:1 --anim jump:2:1-4 --anim crouch:2:0-1

# The stage is drawn rather than converted, since the Neo Geo has no
# background layer and it has to be built from sprite tiles anyway.
STAGE_LAYERS=assets/stage-sky.gif assets/stage-hills.gif assets/stage-ground.gif
$(STAGE_LAYERS) assets/stage.h: tools/make_stage.py tools/neogeo_color.py
	PYTHONPATH=tools $(PYTHON) tools/make_stage.py \
	    --outdir assets --header assets/stage.h --name stage

$(BUILDDIR)/main.o: assets/hero.h assets/stage.h


# sound driver ROM: ngdevkit's stock driver, enough to satisfy the BIOS ---
SOUND_DRIVER=$(BUILDDIR)/assets/base-sound-driver.ihx
$(MROM1): $(SOUND_DRIVER)


# sample ROM: no audio samples yet, so the V ROM stays empty --------------


# assets/square.gif is committed, so nothing to pre-generate here.
CUSTOM_GENERATE_TARGETS=
