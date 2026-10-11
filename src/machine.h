/*
 * machine.h - the DOS machine under the lifted game: shared state between
 * src/*.c. The generated code never includes this; it sees lifted.h only.
 */
#pragma once
#include "lifted.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32                        /* Linux: the few MSVC names the machine uses */
#include <strings.h>
#include <sys/stat.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _mkdir(d) mkdir(d, 0777)
typedef struct { long left, top, right, bottom; } RECT;   /* Win32's, for the presenter */
#endif

extern CPU g_cpu;
extern uint16_t g_psp_seg;           /* the running program (dos.c) */

typedef struct {
    const char *datadir;      /* the game's files (read) */
    const char *savedir;      /* where CONFIG and saved games go (read+write) */
    int headless;             /* no window: render offscreen */
    const char *record;       /* --record out.mp4 */
    double seconds;           /* stop after this long (0 = never) */
    const char *keys;         /* scripted input: "ms:key,ms:key" */
    const char *shots;        /* --shot-at "ms:file.bmp,..." */
    const char *wav;          /* --wav out.wav: the audio, no ffmpeg needed */
    int mute;
    int trace;                /* RECOMP_TRACE-ish logging of DOS calls */
    const char *args;         /* game command line tail */
    int window;               /* window size, multiples of 320x240 */
    int fullscreen;
    const char *scale;        /* --scale MODE: sharp | smooth | crt | nearest | integer */
    const char *look;         /* --look natural | vivid */
    const char *bars;         /* --bars blur | black */
    int whole;                /* --whole-table */
    int classic;              /* --classic: the game's own picture, no remaster */
    int present_w, present_h; /* --present WxH: --shot-at frames through the presenter */
    int realtime;             /* headless, but on the wall clock */
} Options;
extern Options g_opt;

/* The remaster's picture (host.c keeps it in epicpinball.ini, present.c draws it) */
enum { PRESENT_SHARP, PRESENT_SMOOTH, PRESENT_CRT, PRESENT_NEAREST, PRESENT_INTEGER, PRESENT_MODES };
typedef struct {
    int mode;                 /* PRESENT_* */
    int look;                 /* 0 natural, 1 vivid */
    int bars;                 /* beside the picture: 0 black, 1 a blur of it */
    int whole;                /* the whole table, no scrolling (vga_compose_table) */
} Present;
extern int g_audio_smooth;    /* audio.c: interpolate the DSP's samples (else hold them) */

uint64_t host_us(void);
void     trace(const char *fmt, ...);
void     fatal(const char *fmt, ...);

/* dos.c */
void dos_boot(CPU *cpu);
void dos_exit(int code);              /* longjmps back to main */
uint16_t bios_stub_seg(void);
int  bios_builtin(CPU *cpu, unsigned num);   /* the BIOS/DOS behind an unhooked vector */
void bios_key_push(uint8_t scan, uint8_t ascii);

/* images.c: which lifted program or driver owns an address */
void images_placed(const char *file, const uint8_t *raw, uint32_t rawlen, uint16_t seg, uint32_t len);
void images_gone(uint16_t seg);
void (*images_lookup(uint16_t seg, uint16_t off))(CPU *);
void images_miss(const char *how, uint16_t seg, uint16_t off);

/* machine.c: PIC, PIT, keyboard controller, speaker, IRQ delivery */
void machine_init(void);
void kbd_scancode(uint8_t sc);        /* from the host: one byte of a make/break */
int  machine_port_in(uint16_t port, uint8_t *v);
int  machine_port_out(uint16_t port, uint8_t v);
void call_vector(CPU *cpu, unsigned vec);    /* run whatever is in the IVT, as INT would */
uint64_t pit_now(void);
uint64_t emu_us(void);                /* see machine.c: wall clock, or deterministic */
void machine_port_wait(unsigned us);  /* a polled port read costs emulated time */
void machine_paused(uint64_t us);     /* wall-clock time the guest did not see */
void machine_report(void);               /* emulated time in PIT ticks (1.193182 MHz) */
#define PIT_HZ 1193182.0

/* vga.c */
void vga_init(void);
void vga_start(void);
void vga_set_mode(int m);
int  vga_mode(void);
int  vga_port_in(uint16_t port, uint8_t *v);
int  vga_port_out(uint16_t port, uint8_t v);
void vga_dac_set(int i, uint8_t r, uint8_t g, uint8_t b);
void vga_dac_get(int i, uint8_t *r, uint8_t *g, uint8_t *b);
void vga_compose(uint32_t *out, int *w, int *h);
int  vga_compose_table(uint32_t *out, int *w, int *h);   /* the whole table, or 0 */
extern int g_tick_hold, g_recomp_tick_budget;

/* audio.c: Sound Blaster DSP + 8237 DMA, PC speaker */
void audio_init(void);
void audio_advance(uint64_t pit_ticks);    /* render up to this emulated time */
int  audio_port_in(uint16_t port, uint8_t *v);
int  audio_port_out(uint16_t port, uint8_t v);
void audio_speaker(int gate, uint16_t divisor);
void audio_shutdown(void);

/* host.c: window or offscreen, input, presentation, recording */
void host_init(void);
void host_frame(void);                /* present + pump, at most once per refresh */
void host_audio(const int16_t *stereo, int frames);
void host_shutdown(void);
#define AUDIO_RATE 44100
