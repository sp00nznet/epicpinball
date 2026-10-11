#!/bin/bash
# Linux setup: the same steps as Setup.cmd (scripts/setup.ps1), for a native
# Linux program -- no Wine, no DOSBox.
#
#   1. check the tools (gcc, CMake, Python 3, git, SDL2), and say which
#      packages to install when one is missing
#   2. find your copy of Epic Pinball: an installed copy (GOG's, or any
#      folder holding PINBALL.EXE), or GOG's offline installer, which it
#      unpacks with innoextract (nothing is run)
#   3. copy the game files into original/
#   4. fetch the pcrecomp toolkit next to this folder
#   5. tools/discover.py: lift, build (build-linux/), play every table
#      headless, lift again, until nothing new turns up
#   6. leave a launcher here: epicpinball.sh
#
#   ./setup.sh                    # look for the game
#   ./setup.sh ~/games/pinball    # the folder holding PINBALL.EXE, or GOG's setup_*.exe
#
# Every step is skipped when its result is already there, so running it
# again carries on from where it stopped.
set -e
cd "$(dirname "$0")"
ROOT=$PWD
# The toolkit revision this game needs (README, "Building from source").
PCRECOMP_URL=https://github.com/sp00nznet/pcrecomp
PCRECOMP_REF=1dfdaef3a51c670e5931b0e5ef1a3b6e6cd9383e  # pcrecomp#74, #75

say()  { printf '%s\n' "$*"; }
step() { printf '\n\033[36m%s\033[0m\n' "$*"; }
fail() { printf '\n\033[31mSetup stopped: %s\033[0m\n' "$*" >&2; exit 1; }

# ---- 1. tools -------------------------------------------------------------------
step "Checking the tools"
need=()
for t in gcc cmake python3 git pkg-config; do command -v "$t" >/dev/null || need+=("$t"); done
command -v pkg-config >/dev/null && ! pkg-config --exists sdl2 && need+=("SDL2's development files")
if [ ${#need[@]} -gt 0 ]; then
  say "  Missing: ${need[*]}. From your package manager, for example:"
  say "    Debian, Ubuntu: sudo apt install gcc cmake python3 git pkg-config libsdl2-dev innoextract"
  say "    Fedora:         sudo dnf install gcc cmake python3 git pkgconf sdl2-compat-devel innoextract"
  say "    Arch:           sudo pacman -S gcc cmake python git pkgconf sdl2 innoextract"
  say "  (innoextract only to unpack GOG's installer; ffmpeg only for --record.)"
  fail "install them, then run ./setup.sh again."
fi
say "  ok"

# ---- 2, 3. the game -----------------------------------------------------------------
# The folder under $1 holding PINBALL.EXE with the tables beside it (not
# the shareware one in DEMO/), any case
game_in() {
  find "$1" -iname PINBALL.EXE 2>/dev/null | while read -r f; do
    d=$(dirname "$f")
    if [ -n "$(find "$d" -maxdepth 1 -iname EP1.EXE)" ]; then echo "$d"; break; fi
  done
}

step "Finding Epic Pinball"
dest=$ROOT/original
if [ -n "$(game_in "$dest")" ]; then
  say "  already in original/"
else
  src=
  inst=
  if [ -n "$1" ]; then
    if [ -d "$1" ]; then src=$(game_in "$1"); else inst=$1; fi
  else
    for d in "$HOME/GOG Games" "$HOME/Games" "$HOME/games"; do
      [ -d "$d" ] && src=$(game_in "$d") && [ -n "$src" ] && break
    done
    [ -n "$src" ] || inst=$(ls "$HOME"/Downloads/setup_epic_pinball*.exe "$ROOT"/setup_epic_pinball*.exe 2>/dev/null | head -1)
  fi
  if [ -z "$src" ] && [ -n "$inst" ]; then
    say "  GOG's installer: $inst"
    command -v innoextract >/dev/null || fail "innoextract unpacks it; install it (see above), then run ./setup.sh again."
    mkdir -p work/gog
    say "  unpacking it into work/gog (nothing is run)"
    innoextract --silent --output-dir work/gog "$inst"
    src=$(game_in work/gog)
  fi
  [ -n "$src" ] || fail "Epic Pinball was not found. Run ./setup.sh /the/folder/holding/PINBALL.EXE (or GOG's setup_epic_pinball*.exe)."
  say "  copying from $src"
  mkdir -p "$dest"
  # the game's own files, named as DOS names them -- not the DOSBox GOG wraps it in
  for f in "$src"/*; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    case "${b,,}" in *.conf|*.pdf|*.map|*.bat|*.ico|*.info|*.hashdb|*.script|*.ini|*.dll|*.sh|*.desktop) continue ;; esac
    cp "$f" "$dest/${b^^}"
  done
fi

# ---- 4. the toolkit -----------------------------------------------------------------
step "The pcrecomp toolkit"
if [ -n "$PCRECOMP_HOME" ] && [ -f "$PCRECOMP_HOME/tools/lift/lift16.py" ]; then
  say "  $PCRECOMP_HOME (PCRECOMP_HOME)"
else
  PCRECOMP_HOME=$(cd .. && pwd)/pcrecomp
  [ -f "$PCRECOMP_HOME/tools/lift/lift16.py" ] || git clone --quiet "$PCRECOMP_URL" "$PCRECOMP_HOME"
  git -C "$PCRECOMP_HOME" fetch --quiet origin
  git -C "$PCRECOMP_HOME" checkout --quiet "$PCRECOMP_REF"
  say "  $PCRECOMP_HOME at ${PCRECOMP_REF:0:7}"
fi
export PCRECOMP_HOME

# ---- 5. lift + build ----------------------------------------------------------------
step "Lifting and building, then playing each table headless to find the rest"
python3 tools/discover.py
[ -x build-linux/epicpinball ] || fail "the build finished but build-linux/epicpinball is missing."

# ---- 6. launcher --------------------------------------------------------------------
printf '#!/bin/sh\ncd "$(dirname "$0")" && exec build-linux/epicpinball "$@"\n' > epicpinball.sh
chmod +x epicpinball.sh
step "Ready: ./epicpinball.sh"
say "Shift keys flip, Space plunges, Alt+Enter toggles fullscreen, F10 or right-click for settings."
