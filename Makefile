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

# Mirror the source tree's directories into build/ every time make runs.
#
# None of the compilers create their own output directory, and the directories
# are otherwise made by a one-shot pass that records itself as done - so a
# build/ carried over from an earlier layout is missing whatever was added
# since, and the build fails with "cannot create" or "No such file or
# directory" somewhere far from the cause. This has now happened three times:
# when src/ was added, and when the assets were split into images/sprites and
# images/stages. Doing it at parse time costs a few milliseconds and cannot go
# stale, whatever state build/ is in and whatever directories are added later.
BUILD_DIRS := $(BUILDDIR) $(BUILDDIR)/rom $(BUILDDIR)/assets/sfx \
              $(patsubst %,$(BUILDDIR)/%,$(shell find assets src -type d 2>/dev/null))
$(shell mkdir -p $(BUILD_DIRS))

# The same pass also copies in ngdevkit's own assets - the font tiles and the
# eye-catcher the BIOS needs. It writes a marker when it finishes and is
# skipped ever after, so if those files are removed while the marker survives,
# make has no rule left that produces them and stops. Dropping the marker when
# any of them is missing makes the pass run again.
BASE_ASSETS := $(BUILDDIR)/assets/base-crom-logo.c1 \
               $(BUILDDIR)/assets/base-crom-logo.c2 \
               $(BUILDDIR)/assets/base-srom-text-shadow.fix \
               $(BUILDDIR)/assets/base-sound-driver.ihx
$(shell for f in $(BASE_ASSETS); do \
            [ -f "$$f" ] || { rm -f $(BUILDDIR)/.generated; break; }; \
        done)

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


# fix ROM: ngdevkit's 8x8 text tiles, then our own ------------------------
# The font occupies tiles 0-1279, so the solid block the screen transition
# paints with lands at 1280. main.c has that number as SOLID_TILE.
$(SROM1): $(BUILDDIR)/assets/base-srom-text-shadow.fix
$(SROM1): $(BUILDDIR)/assets/images/fix/solid.fix

assets/images/fix/solid.gif: tools/make_fixtile.py
	$(PYTHON) tools/make_fixtile.py -o $@


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

# The character. Two sets of art are in the tree; HERO picks between them.
#
#   new  animated GIFs in assets/new, one per animation, at their own size
#   old  the single hand-laid sheet, scaled down to 80 pixels tall
#
HERO?=new

ifeq ($(HERO),new)
# Each animation is its own GIF with far more frames than a 60 Hz game needs,
# so they are sampled down and laid out as a sheet. No scaling: this art is
# used at the size it was drawn. There is no crouch art, and the jump borrows
# mid-stride frames from the run as a stand-in.
#
# The tolerance is tuned to this art. Its backdrop is (91,112,117); the ground
# shadow drawn under the character sits 45 away from that per channel and the
# nearest colour the character itself uses is 50, so 46 takes the shadow and
# leaves the character whole.
# Listed rather than globbed: one of the files in there has spaces in
# its name, which make cannot carry through a prerequisite list.
NEWGIFS=assets/new/runing.gif assets/new/attack.gif
PREPPED=$(BUILDDIR)/assets/hero-sheet-$(HERO).png

$(PREPPED): $(NEWGIFS) tools/gifs2sheet.py Makefile | $(BUILDDIR)/assets
	$(PYTHON) tools/gifs2sheet.py -o $@ --bg-tolerance 46 \
	    --anim "walk=assets/new/runing.gif:2-43:8" \
	    --anim "attack=assets/new/attack.gif:1-26:8" \
	    --anim "jump=assets/new/runing.gif:4-12:4"

HERO_ANIMS=--anim walk:0 --anim attack:1 --anim jump:2
else
SHEET=assets/images/sprites/hero-sheet.png
PREPPED=$(BUILDDIR)/assets/hero-sheet-$(HERO).png

# That sheet is a JPEG with the transparency checkerboard painted into it, so
# the backdrop is keyed out and the art scaled down before conversion.
$(PREPPED): $(SHEET) tools/prep_sheet.py Makefile | $(BUILDDIR)/assets
	$(PYTHON) tools/prep_sheet.py $(SHEET) -o $@ --height 80

HERO_ANIMS=--anim walk:1 --anim attack:2 --anim jump:4:4-8 --anim crouch:5:2-4
endif

assets/images/sprites/hero.gif assets/images/sprites/hero.h: $(PREPPED) tools/sheet2neo.py tools/neogeo_color.py
	PYTHONPATH=tools $(PYTHON) tools/sheet2neo.py $(PREPPED) \
	    -o assets/images/sprites/hero.gif \
	    --header assets/images/sprites/hero.h --name hero $(HERO_ANIMS)

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

# user_commands.s includes the generated sample offsets, so they must exist
# before it is assembled. The "generate" pass produces them, but only runs
# once per build tree, so name the file directly rather than trust the pass.
$(BUILDDIR)/src/user_commands.rel: $(BUILDDIR)/assets/samples.inc


# sample ROM: the ADPCM-A sound effects ------------------------------------
#
# The YM2610's ADPCM-A channels run at a fixed 18.5 kHz, so the sources are
# resampled to that and made mono. The silence filters trim the dead air at
# each end - generously at the tail, so a sound's decay is not clipped off.
SFX=coin-pickup jump punch
SFXWAV=$(SFX:%=$(BUILDDIR)/assets/sfx/%.wav)

$(BUILDDIR)/assets/sfx/%.wav: assets/sound/%.mp3
	"$(SOX)" -V1 $< -c 1 -r 18500 $@ \
	    silence 1 0.01 0.1% reverse silence 1 0.15 0.03% reverse

$(VROM1): assets/sound/samples-map.yaml


# Sample offsets have to exist before the Z80 source is assembled, so they
# are generated in the "generate" pass that runs ahead of the build proper.
CUSTOM_GENERATE_TARGETS=generate-sfx
generate-sfx: $(BUILDDIR)/assets/samples.inc

$(BUILDDIR)/assets/samples.inc: assets/sound/samples-map.yaml $(SFXWAV)
	$(VROMTOOL) --asm -s $(VROMSIZE) $< -o $(VROM1) -m $@
