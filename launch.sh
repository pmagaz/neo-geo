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

if [ ! -f config.mk ]; then
    echo "config.mk missing, running ./configure first"
    ./configure
fi

if [ "$BUILD" = yes ]; then
    gmake
fi

echo "starting $TARGET"
exec gmake "$TARGET"
