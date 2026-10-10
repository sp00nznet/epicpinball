# Roadmap

## Next

- **Play it by hand.** Every table has been played by script for half a minute.
  A real session — multiball, every table's modes and bonus rounds, entering a
  high score — will find what scripts do not.
- **Listen to it.** The sound is verified by measurement (an SB16 stereo stream,
  two smooth channels, music-shaped spectrum), not by ear. The DSP output is a
  zero-order hold from 22 kHz to 44.1 kHz; a linear or windowed resampler is the
  obvious upgrade once someone has listened.
- **Merge the toolkit change.** The build needs pcrecomp's 32-bit addressing in
  decode16/lift16 (branch `feat/decode16-addr32`); once merged, pin Setup and the
  README to the merge commit.
- **A netlab recipe**, so builds and conformance run on the farm like the other
  recomps (repo rules, section 8). Needs the MSVC build to run on the clang-cl
  builder, or an MSVC builder.
- **CI**: build the runtime without game data and run conformance, which skips.

## Later

- **Lift `SETUP.EXE`** (Borland C++ 3.x, like Blake Stone's executables) so its
  sound-card menu works and the `-cd` check stops being answered by hand. Until
  then the card is whatever `SOUNDCRD.INF` says; GOG's says Sound Blaster 16.
- **The other MASI drivers**: Gravis Ultrasound (`MDRV003R`), Pro Audio Spectrum
  (`MDRV005R`), PC speaker and no-sound. Each needs its card modelled; the
  SB16 covers everyone today.
- **Gamepad**: implemented with GOG's mapping, untested on hardware.
- **Saves**: high scores and `CONFIG.PIN` go to `saves\`; check that every
  table's high-score entry round-trips.

## Out of scope

- The `DEMO\` folder's shareware build (a different `PINBALL.EXE`/`EP1.EXE`).
- Changing the game: no widescreen, no hi-res playfield. The tables are drawn
  as 320×240 bitmaps; there is nothing to redraw at higher resolution.
