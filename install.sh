#!/bin/sh
# Install everything needed to build and run this game, then configure it.
#
# Run once on a new machine. After this, ./launch.sh builds and runs.
#
#   ./install.sh          install what is missing, then configure
#   ./install.sh --check  report what is missing and change nothing
#
# macOS only for now: everything comes from Homebrew.
set -e

cd "$(dirname "$0")"

CHECK_ONLY=no
[ "$1" = "--check" ] && CHECK_ONLY=yes

if ! command -v brew >/dev/null 2>&1; then
    echo "error: Homebrew is not installed." >&2
    echo "       get it from https://brew.sh and run this again." >&2
    exit 1
fi

BREW_PREFIX=$(brew --prefix)
PATH="$BREW_PREFIX/opt/python@3.14/bin:$BREW_PREFIX/bin:$PATH"
export PATH

# Each line: the command to look for, then the formula that provides it.
# The command is what matters - a formula can be installed while its binary is
# missing from PATH, and it is the binary the build actually needs.
TOOLS="
m68k-neogeo-elf-gcc:ngdevkit
z80-neogeo-ihx-sdasz80:ngdevkit
tiletool.py:ngdevkit
ngdevkit-gngeo:ngdevkit-gngeo
gmake:make
pkg-config:pkg-config
magick:imagemagick
sox:sox
zip:zip
rsync:rsync
python3:python@3.14
"

missing_formulae=""
missing_tools=""
for entry in $TOOLS; do
    tool=${entry%%:*}
    formula=${entry##*:}
    if ! command -v "$tool" >/dev/null 2>&1; then
        missing_tools="$missing_tools $tool"
        case " $missing_formulae " in
            *" $formula "*) ;;
            *) missing_formulae="$missing_formulae $formula" ;;
        esac
    fi
done

if [ -z "$missing_tools" ]; then
    echo "all required tools are present"
else
    echo "missing:$missing_tools"
    echo "provided by:$missing_formulae"
fi

if [ "$CHECK_ONLY" = yes ]; then
    [ -z "$missing_tools" ] || exit 1
    exit 0
fi

if [ -n "$missing_formulae" ]; then
    # ngdevkit lives in its own tap, which newer Homebrew also wants trusted
    # before it will run the formula.
    case "$missing_formulae" in
        *ngdevkit*)
            echo "adding the ngdevkit tap..."
            brew tap dciabrin/ngdevkit
            brew trust dciabrin/ngdevkit 2>/dev/null || true
            ;;
    esac

    echo "installing:$missing_formulae"
    # shellcheck disable=SC2086
    brew install $missing_formulae
fi

# MAME is not required to play, only to capture screenshots and to drive the
# game from a script for testing, so a missing one is worth a note, not a stop.
if ! command -v mame >/dev/null 2>&1; then
    echo
    echo "note: mame is not installed. The game runs without it, in GnGeo."
    echo "      It is needed for './launch.sh mame' and for the headless"
    echo "      screenshot and playtest tooling: brew install mame"
fi

echo
echo "writing config.mk..."
./configure

echo
echo "done. Build and run with: ./launch.sh"
