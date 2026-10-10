# Architecture

```
original/PINBALL.EXE ─┐
original/EP1..13.EXE ─┼─► tools/lift.py ──► work/gen/<image>/*.c ──┐
original/MDRV*.MUS ───┘     │ decode16 + lift16   (never committed)  ├─► MSVC ─► epicpinball.exe
                            │ (pcrecomp)                             │
work/layout.txt ────────────┤                       src/*.c ─────────┘
work/misses/*.txt ──────────┘                       (the DOS machine)
        ▲                                                  │
        └──────────── written by the runtime as it runs ───┘   tools/discover.py loops this
```

## Epic Pinball is several programs

The thing you run under DOS is `PINBALL.EXE`, but it is only the front end:

| Program | What it does | Code |
|---|---|---|
| `PINBALL.EXE` | intro, menus, high scores; loads the MASI sound system and the music; runs `SETUP.EXE -cd` and reads its answer; EXECs a table and keeps the music playing under it | ~6,700 instructions, hand-written asm |
| `EP1.EXE` … `EP13.EXE` | one table each: physics, playfield, scoreboard. Started with a command tail whose first character is the mode (`D` demo, `1`–`4` players) and whose digits are PINBALL's settings | ~6,000 instructions each, the same engine with each table's data |
| `MDRV000R.MUS` | MASI "Line Processing Routines": the mixer that plays the `.PSM` modules and the effects | raw 16-bit code, a `REAL16` header with its entry offset at 28h |
| `MDRV004R.MUS` | the MASI Sound Blaster driver (SB, Pro, 16) | the same format |
| `SETUP.EXE` | sound card setup (Borland C++) | not lifted: its `-cd` check is answered in `src/dos.c` ([ROADMAP](../ROADMAP.md)) |

All of them share one 1 MB address space, as they did under DOS, and call each
other: the table calls PINBALL's music interface through a far pointer PINBALL
put in its command tail, PINBALL calls the drivers through the entry offsets in
their headers, and each installs interrupt handlers the others chain to.

## Images

Each of those files is an **image**: lifted on its own by `tools/lift.py` into
`work/gen/<image>/`, with functions named `<image>_<linear>` (`ep1_32230`),
its own `SEG_xxxx` constants, and a table of its entry points. A relocated
segment in its code is baked in as `value + load segment`, so an image is lifted
*at* the segment it will run at.

Where that is depends on what was allocated before it: PINBALL's memory for its
song and effects decides where `EPn.EXE` goes, and the drivers go wherever
`INT 21h/48h` puts them. So the runtime records each placement as it happens
(`src/images.c` → `work/layout.txt`) and the lifter reads it back. The DOS
model is deterministic, so a placement, once seen, is the same every run; if it
ever differs the runtime refuses to run the image and records the new place.

At run time `src/images.c` keeps which images are **live**: PINBALL from boot, a
driver once PINBALL has read the whole file into memory (`src/dos.c` notices),
a table from its EXEC until it exits. Every indirect jump, indirect call and
interrupt goes through `images_lookup(seg, off)`, which asks the live image
covering that linear address. The thirteen tables overlap (each loads around
segment 2000–5400), which is why liveness, not just address, decides.

## DOS EXEC

`INT 21h/4B00h` (`dos_exec` in `src/dos.c`) does what DOS does: copies the
environment with the child's own path appended, gives the child the largest
free block, builds its PSP and command tail, loads and relocates the module
from `original/`, and starts it — as a nested C call. The child's
`INT 21h/4Ch` longjmps back. On the way out its memory is freed, its image
stops being live, and PINBALL's registers come back.

A table can quit from inside one of its own interrupt handlers, so the
longjmp also skips the rest of the interrupt delivery in `src/machine.c`;
`machine_unwound()` puts that state back, or no interrupt would ever be
delivered again.

`SETUP.EXE -cd <dir>` is the one EXEC that is answered rather than run: SETUP
writes `pincd=<dir>` into an unused corner of monochrome text memory
(`B000:FA00`) and PINBALL reads it back. The runtime writes the same string.

## Memory

A flat 1 MB + 64 KB array, real-mode addressing (`seg * 16 + off`).

| Linear | What |
|---|---|
| `00000` | interrupt vectors — every one points into `F000`, a segment no code lives in, so `F000:nn` *means* "the BIOS/DOS handler for vector nn" |
| `00400` | BIOS data area |
| `00E00` | PINBALL's environment, with `BLASTER=A220 I7 D1 H5 T6` |
| `01000` | PINBALL's PSP (`g_psp_seg` = `0100`), its image at `01100` |
| `0A270`, `0C8D0` | the Sound Blaster driver and the mixer, where PINBALL's allocations put them |
| … | the songs and effects, one DOS block per sample — over 60 for one song |
| `20000`–`55000` | a table's PSP and image, depending on the table |
| `A0000` | VGA window — the planar model in `src/vga.c` |

DOS's arena is a host-side list of blocks (1024 of them: PINBALL allocates one
per sample, and a 64-entry list overflowed into the image registry).

## Time

Lifted code runs to completion inside C calls, so nothing can interrupt it the
way an IRQ does. The lifter puts `RECOMP_TICK` on every loop back-edge; every
2000 of them `recomp_tick()` (`src/machine.c`) turns emulated time into PIT
ticks, delivers the timer interrupts that fell due (rendering audio up to each
one), then keyboard and Sound Blaster IRQs, and gives the host a frame.

The tables lock their timer to the display: the timer ISR waits for vertical
retrace on port `3DAh` before reprogramming the PIT. Inside an ISR no tick runs
— so in deterministic (headless) time nothing would ever advance and the wait
would spin forever. Each read of `3DAh` therefore costs a microsecond of
emulated time, as an ISA read does on the real bus.

Windowed, emulated time is the wall clock. Headless it is deterministic: each
poll advances it 300 µs, so two runs with the same arguments produce the same
frames byte for byte (the conformance harness checks this), and a minute of
play takes about two seconds.

## Video

The tables run in Mode X at **320×240**: mode 13h with chain-4 off and the
CRTC reprogrammed for 480 scanlines, double-scanned. `src/vga.c` models planar
memory, the latches, write modes, the DAC, and two things the tables need:

- **the start address**, which is how the playfield scrolls and how pages flip
  (two copies of the playfield, at `0640h` and around `8F20h`);
- **line compare** (CRTC 18h plus its overflow bits in 07h and 09h): at
  scanline 441 the CRTC starts over from address 0, which is where the
  dot-matrix scoreboard lives. That is the bar across the bottom of every table.

PINBALL's own screens are mode 13h at 320×200.

## Sound

The MASI Sound Blaster driver asks the DSP its version and, given a Sound
Blaster 16 (DSP 4.05, which is what GOG's DOSBox setup emulates and what
`SOUNDCRD.INF` was written for), plays **8-bit stereo at 22,050 Hz**, auto-init
DMA on channel 1, 2 KB blocks, IRQ 7 — DSP commands `41h` and `C6h`. `src/audio.c`
models that DSP (and the older SB/SB Pro commands), the 8237 DMA channel and the
PC speaker, and mixes to 44.1 kHz stereo for waveOut, a WAV, or ffmpeg,
drawing a line between the DSP's samples (or holding each, as an option;
[remaster](remaster.md#sound)). There is no FM chip to model: MASI mixes
samples itself.

## Host

Windowed: a Win32 window drawn by the Direct3D 11 presenter (`src/present.c`,
GDI if that fails), scancodes from `WM_KEYDOWN`, an XInput pad mapped by
`controller.ini` (`src/pad.c`), and the remaster's settings menu and hotkeys
([remaster](remaster.md)). The game never asks for a mouse driver, and gets
none.
Headless (`--headless`): no window and no audio device, so it works over RDP
and in CI; `--record` pipes frames to ffmpeg, `--shot-at` writes frames at fixed
emulated times (through the presenter, offscreen, with `--present WxH`),
`--keys` presses keys at fixed emulated times.
