#!/bin/sh
# Build the cartridge and run it in a single, clean emulator instance.
#
# Stacked emulator windows are easy to create and very confusing to debug: the
# oldest window keeps running an old build, so fixes appear to do nothing. This
# script kills every previous instance before starting a new one.
#
# Usage:
#   ./launch.sh            run as an AES (home console)
#   ./launch.sh mvs        run as an MVS (arcade)
#   ./launch.sh --no-build skip the build, just launch what is already built
set -e

cd "$(dirname "$0")"

BREW_PREFIX=$(brew --prefix 2>/dev/null || echo /opt/homebrew)
PATH="$BREW_PREFIX/opt/python@3.14/bin:$BREW_PREFIX/bin:$PATH"
export PATH

TARGET=gngeo
BUILD=yes
for arg in "$@"; do
    case "$arg" in
        mvs)        TARGET=gngeo-mvs ;;
        aes)        TARGET=gngeo ;;
        mame)       TARGET=mame ;;
        mame-mvs)   TARGET=mame-mvs ;;
        --no-build) BUILD=no ;;
        *) echo "usage: $0 [aes|mvs|mame|mame-mvs] [--no-build]" >&2; exit 1 ;;
    esac
done

# Kill anything left over from a previous run, and wait for it to actually go.
if pgrep -f "gngeo|mame.*neogeo" >/dev/null 2>&1; then
    echo "stopping previous emulator instances..."
    pkill -9 -f "gmake gngeo" 2>/dev/null || true
    pkill -9 -f gngeo 2>/dev/null || true
    pkill -9 -f "mame.*neogeo" 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        pgrep -f "gngeo|mame.*neogeo" >/dev/null 2>&1 || break
        sleep 0.3
    done
fi

# This script does not install or configure anything - ./install.sh does that.
# It only checks that it was run, because the failures otherwise appear a long
# way from the cause: an empty path in config.mk is run by make as a command,
# and a missing sox surfaced as vromtool complaining about absent WAV files.
if [ ! -f config.mk ]; then
    echo "error: config.mk is missing. Run ./install.sh first." >&2
    exit 1
fi

for var in PYTHON SOX CONVERT M68KGCC Z80SDAS TILETOOL VROMTOOL ROMTOOL GNGEO; do
    if [ -z "$(sed -n "s/^$var=//p" config.mk | head -1)" ]; then
        echo "error: config.mk has no path for $var." >&2
        echo "       A tool was missing when it was written." >&2
        echo "       Run ./install.sh to install it and reconfigure." >&2
        exit 1
    fi
done

if [ "$BUILD" = yes ]; then
    gmake
fi

echo "starting $TARGET"
exec gmake "$TARGET"
