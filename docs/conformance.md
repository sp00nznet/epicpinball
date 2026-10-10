# Conformance

```
py tools/conformance.py            # needs your game data in original/ and a build
py tools/conformance.py --update   # record the current count as the new baseline
```

Current figure (2026-10-10): **125/125.** The whole run takes about 25 seconds:
headless time is deterministic and runs far faster than real time
([architecture](architecture.md#time)). Frames, audio and logs of the last run
stay in `work/conformance/` to explain any FAIL.

## What it checks, and why that is the ground truth

There is no reference build to diff frames against — the original only runs
under DOSBox — so the checks are things the original visibly does, run headless
with scripted keys (`--keys`) and fixed-time frame grabs (`--shot-at`). Each
table is reached the way a player reaches it: BEGIN, down the list, ENTER, PLAY
GAME, then SPACE to start, the plunger every few seconds, and both flippers.

| Check | Fails when |
|---|---|
| lift *image*: every instruction form handled (16 images) | `tools/lift.py` met an instruction lift16 emitted as `UNHANDLED` |
| boot: clean exit, no dispatch misses | the process crashed, or the guest reached an address with no lifted function |
| boot: SETUP check answered, sound drivers placed | PINBALL did not get past its SETUP check, or never loaded the MASI mixer and SB driver |
| boot: menu on screen | 12 s in, the frame is not a graphics picture with the menu drawn |
| table *n*: clean exit, PINBALL ran EP*n*.EXE, no dispatch misses (×13) | the menu did not EXEC the table chosen, or anything along the way missed |
| table *n*: 320×240 playfield with 32+ colours | the table is not in its Mode X 320×240 view, or it is drawn in a handful of colours |
| table *n*: scoreboard in the split screen | the rows below line compare are dark: the split is wrong or the dot matrix was not drawn |
| table *n*: the view moves in play | under 2% of the playfield changed between 12 s into play and the end — no ball, no scrolling |
| table *n*: SB16 stereo stream started | the SB driver never started an SB16 stereo auto-init transfer |
| table *n*: audio | the WAV the run wrote is near silent |
| determinism: two runs, identical frames | the same script twice gave different frames — time leaked into the run |

## Without the game

The data is not redistributable, so on a machine without `original\PINBALL.EXE`
or without a build the harness prints

```
SKIP: no game data in original/ or no build -- conformance needs your own copy (README, Getting Started)
```

and exits 0. A count below `tests/conformance_baseline.json` exits 1.
