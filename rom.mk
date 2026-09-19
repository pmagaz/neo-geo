# -*- makefile -*-
#
# Layout of the game cartridge: which ROM chips exist and how big they are.

BUILDDIR?=build
ROM?=$(BUILDDIR)/rom

GAMEROM?=square

# program ROM: the 68000 code
PROMSIZE=1048576
PROM1=$(ROM)/$(GAMEROM)-p1.p1

# sprite ROM: 16x16 tiles, split across two chips (odd/even bitplanes)
CROMSIZE=2097152
CROM1=$(ROM)/$(GAMEROM)-c1.c1
CROM2=$(ROM)/$(GAMEROM)-c2.c2

# fix ROM: the 8x8 tiles of the text layer
SROMSIZE=131072
SROM1=$(ROM)/$(GAMEROM)-s1.s1

# sound driver ROM: the Z80 program
MROMSIZE=131072
MROM1=$(ROM)/$(GAMEROM)-m1.m1

# sample ROM: ADPCM audio
VROMSIZE=524288
VROM1=$(ROM)/$(GAMEROM)-v1.v1
# Defining this switches the build to packing the V ROM with vromtool from
# assets/sound/samples-map.yaml. Without it the makefile would simply
# concatenate the prerequisites, which would put the YAML in the ROM.
VROMTEMPLATE=$(ROM)/$(GAMEROM)-vX.vX
