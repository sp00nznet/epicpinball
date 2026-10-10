# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[SemVer](https://semver.org/).

## [Unreleased]

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
