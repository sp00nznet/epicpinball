# Roadmap

## Next

- **Play it by hand.** Every table has been played by script for half a minute.
  A real session — multiball, every table's modes and bonus rounds, entering a
  high score — will find what scripts do not.
- **Listen to it.** The sound is verified by measurement (an SB16 stereo stream,
  two smooth channels, music-shaped spectrum), not by ear. It is interpolated
  linearly from 22 kHz to 44.1 kHz; a windowed resampler is the next step if
  that is not enough.
- **Try the remaster's hotkeys and menu by hand.** The whole table, the
  presenter and the settings file are checked headless and in a recorded
  window; F7-F12, the menu and the pad were not pressed by a test.
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
- **Gamepad**: `controller.ini`, GOG's mapping by default, untested on hardware.
  Rumble on the flippers and bumpers would be the remaster's next touch.
- **Portrait fullscreen**: the whole table on a monitor turned on its side
  fills it; rotating the picture for a sideways monitor left in landscape
  mode (cabinet style) is a shader change.
- **Saves**: high scores and `CONFIG.PIN` go to `saves\`; check that every
  table's high-score entry round-trips.

## Out of scope

- The `DEMO\` folder's shareware build (a different `PINBALL.EXE`/`EP1.EXE`).
- Changing the game: no hi-res playfield. The tables are drawn as 320-pixel
  bitmaps; there is nothing to redraw at higher resolution. The whole-table
  view shows more of the game's own picture, which is as far as it goes.
- Glow on the lamps, as the SimCity 2000 recomp has: the lamps are not the
  brightest colours on these tables, the plastics are (docs/remaster.md).
