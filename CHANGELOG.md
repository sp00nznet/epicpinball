# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[SemVer](https://semver.org/).

## [Unreleased]

## [0.2.0-alpha.1] - 2026-10-22

### Added
- **The installer**, for Windows and Linux: recomp-launcher built from
  `launcher/launcher.json` by `.github/workflows/installer.yml`. Every push
  to main leaves it as an artifact; a `v*` tag puts it on that release, and
  the tag is the version it shows. It finds the game, builds, and plays.
- `setup.sh` takes `--yes --game-dir DIR`; both setups print `[n/5]` steps,
  end a failure with `Setup stopped:`, find GOG's installer in the folder
  they are given, and need no git when pcrecomp is already beside them (the
  installer downloads it, at the pinned commit).

### Fixed
- `setup.sh` given a folder took the shareware demo in its `DEMO/` (it has
  `EP1.EXE` too) when `find` listed it first; it takes the shallowest now.
- **Linux**, natively: the same lifted C and DOS machine built with gcc,
  with SDL2 for the window, sound and pad (`src/host.c`, `src/present.c`,
  `src/pad.c`); `setup.sh` (the tools, the game, pcrecomp, lift, build, a
  launcher) and `build-linux.sh`, after the Red Alert 2 recomp's. Every
  remaster feature works there; the settings menu is a dialog. Conformance
  133/133 on Debian 13, from a clean `./setup.sh`, and its frames are
  bit-identical to the Windows build's.
- The remaster (docs/remaster.md), all optional and off with `--classic`:
  - **The whole table** (F7): `vga_compose_table` draws the page of video
    memory the game scrolls over -- the full table, ball included -- with the
    scoreboard under it, from the CRTC registers, for every table.
  - `src/present.c`: a Direct3D 11 presenter, from the Gunman Chronicles
    recomp: sharp-bilinear, smooth, CRT, nearest, integer (F12), a vivid look
    (F9), a dark blur of the picture beside it (from Red Alert 2), GDI if
    Direct3D will not start. `--present WxH` renders `--shot-at` frames
    through it headless.
  - A settings menu (right-click, F10) and `saves\epicpinball.ini`, which
    keeps the choices and the window's place; F11 and Alt+Enter fullscreen,
    PrtSc screenshots. Command-line choices win for the run and are not saved.
  - Smooth sound: the DSP's 22 kHz samples interpolated to 44.1 kHz (Settings
    → Sound → Original for the held steps).
  - `src/pad.c`: the Xbox controller mapped by `saves\controller.ini`, GOG's
    mapping by default.
- Conformance: the whole table and the presenter on a table with each
  scoreboard height; selftest: the page arithmetic and the interpolation.

### Changed
- `--scale` is now the scaling mode; the window size is `--window N`.
  `--display` is gone (F12 and `--scale` replace it).

### Removed
- The INT 33h mouse driver and mouse capture, left from Blake Stone: the game
  never asks for a mouse. Right-click is the settings menu.

### Changed
- Pinned to pcrecomp 1dfdaef, now that the toolkit changes this needs are merged
  (pcrecomp#74, #75).

## [0.1.0] - 2026-10-10

### Added
- `tools/lift.py`: lifts `PINBALL.EXE`, the thirteen table EXEs and the MASI
  mixer and Sound Blaster driver, each as its own image at the segment it runs
  at. Finds code through decoded calls and jumps, bounded and unbounded jump
  tables (`mov reg, cs:[bx+T] / jmp reg`), unrolled-loop entry tables, routines
  stored in called-through variables, the MASI driver header, and stops at
  `int 21h/4Ch`.
- `tools/discover.py`: lift, build, play every table headless, repeat until
  every address reached is lifted (four rounds from nothing).
- `src/images.c`: which lifted program or driver owns an address, which are
  live, and where each was placed (`work/layout.txt`, `work/misses/`).
- `src/dos.c`: DOS EXEC, so PINBALL runs the tables; program loading from the
  game folder; the `SETUP.EXE -cd` check answered; drivers noticed as PINBALL
  reads them in; a 1024-block arena.
- `src/vga.c`: CRTC line compare, for the tables' split-screen scoreboard.
- `src/audio.c`: the Sound Blaster 16 DSP (4.05: `41h` rate, `C0h`–`C6h` 8-bit
  mono/stereo, signed/unsigned) with stereo output; IRQ 7.
- Polled `3DAh` reads cost emulated time, so retrace waits inside the timer
  interrupt end in deterministic runs.
- Interrupt state reset when a program exits from inside an interrupt handler.
- Trace builds: lifted code at `/Od` (seconds to build) and a watchdog that
  prints the lifted-call ring and the main thread's generated-C line when the
  guest stops polling.
- `tools/conformance.py`: boot, menu, all thirteen tables played, scoreboard,
  scrolling, SB16 stream, audio, determinism — 125/125.
- `tests/selftest.c` (split screen, SB16 stereo, the image registry; no game
  data) and CI on Windows: lint, selftest, conformance (which skips there).
- `Setup.cmd` / `scripts/setup.ps1`: the Quick start, from a GOG install or
  GOG's offline installer (unpacked with innoextract) to a shortcut.
- The DOS machine started from the Blake Stone recomp's (same author, MIT),
  without its hi-res renderer and OPL2.
