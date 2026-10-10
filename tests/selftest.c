/*
 * selftest.c - the runtime's own pieces, checked without any game data, so CI
 * has something real to run (the conformance harness needs your copy of the
 * game and skips without it).
 *
 * One check per behaviour the game is known to depend on; each comment says
 * which. Built by CMake as `selftest`, with no lifted code at all.
 */
#undef NDEBUG                     /* the asserts ARE the test, in every build */
#include "machine.h"
#include <assert.h>

CPU g_cpu;
Options g_opt;
uint64_t host_us(void) { return 0; }
void trace(const char *fmt, ...) { (void)fmt; }
void fatal(const char *fmt, ...) { (void)fmt; abort(); }
static int16_t heard[4096 * 2];
static int nheard;
void host_audio(const int16_t *s, int frames)
{
    for (int i = 0; i < frames && nheard < 4096; i++, nheard++) { heard[nheard * 2] = s[i * 2]; heard[nheard * 2 + 1] = s[i * 2 + 1]; }
}
void host_frame(void) {}
void host_backtrace(void) {}
uint64_t pit_now(void) { return 0; }
uint64_t emu_us(void) { return 0; }
void machine_port_wait(unsigned us) { (void)us; }
void recomp_tick(CPU *c) { (void)c; }
int machine_port_in(uint16_t p, uint8_t *v) { (void)p; (void)v; return 0; }
void call_vector(CPU *c, unsigned v) { (void)c; (void)v; }
int machine_busy(void) { return 0; }
void machine_unwound(void) {}

/* the image list tools/lift.py would generate: one fake image, two functions */
static int ran;
static void f10(CPU *c) { (void)c; ran = 0x10; }
static void f2345(CPU *c) { (void)c; ran = 0x2345; }
static const RecompFunc fake_funcs[] = { {0x10, f10}, {0x2345, f2345} };
static const LiftedImage fake = { "ep1", "EP1.EXE", 0x4000, 16, 0x69691905u /* FNV-1a of 16 zeros */, 2, fake_funcs };
const LiftedImage *const g_lifted[] = { &fake, NULL };

static void out(uint16_t port, uint8_t v) { assert(vga_port_out(port, v) || audio_port_out(port, v)); }

static void test_split_screen(void)
{
    /* the tables' scoreboard: CRTC line compare restarts scanout at address 0 */
    static uint32_t frame[640 * 480]; int w, h;
    vga_init();
    vga_set_mode(0x13);
    out(0x3C4, 4); out(0x3C5, 0x06);                     /* Mode X */
    out(0x3D4, 0x12); out(0x3D5, 0xDF);                  /* 480 lines ... */
    out(0x3D4, 0x07); out(0x3D5, 0x3E);                  /* ... with the overflow bits */
    out(0x3D4, 0x09); out(0x3D5, 0x01);                  /* double scan */
    out(0x3D4, 0x18); out(0x3D5, 0xB9);                  /* line compare 1B9h: scanline 441 */
    out(0x3D4, 0x0C); out(0x3D5, 0x06);                  /* start 0640h: playfield below the DMD */
    out(0x3D4, 0x0D); out(0x3D5, 0x40);
    out(0x3C4, 2); out(0x3C5, 0x01);                     /* plane 0: pixel 0 of each group */
    mem_write8(&g_cpu, 0xA000, 0x0000, 3);               /* the scoreboard's first pixel */
    mem_write8(&g_cpu, 0xA000, 0x0640, 5);               /* the playfield's first pixel */
    vga_dac_set(3, 63, 0, 0); vga_dac_set(5, 0, 63, 0);
    vga_compose(frame, &w, &h);
    assert(w == 320 && h == 240);
    assert(frame[0] == 0xFF00FF00u);                     /* row 0: the start address */
    assert(frame[221 * 320] == 0xFFFF0000u);             /* row 221: address 0 */
}

static void test_sb16_stereo(void)
{
    /* MASI on a Sound Blaster 16: DSP 4.05, rate 22050 by 41h, then C6h mode
     * 20h -- 8-bit stereo unsigned auto-init -- on DMA channel 1 */
    static uint8_t buf[8] = { 0x80 + 64, 0x80 - 64, 0x80 + 64, 0x80 - 64, 0x80 + 64, 0x80 - 64, 0x80 + 64, 0x80 - 64 };
    memcpy(g_cpu.mem + 0x12000, buf, sizeof buf);
    audio_init();
    out(0x226, 1); out(0x226, 0);
    out(0x22C, 0xD1);                                    /* speaker on */
    out(0x0A, 0x05);                                     /* mask ch1 */
    out(0x0C, 0); out(0x02, 0x00); out(0x02, 0x20);      /* address 2000h */
    out(0x0C, 0); out(0x03, 7); out(0x03, 0);            /* 8 bytes */
    out(0x83, 0x01);                                     /* page 1 */
    out(0x0B, 0x59);                                     /* auto-init, read, ch1 */
    out(0x0A, 0x01);                                     /* unmask */
    out(0x22C, 0x41); out(0x22C, 0x56); out(0x22C, 0x22);
    out(0x22C, 0xC6); out(0x22C, 0x20); out(0x22C, 7); out(0x22C, 0);
    /* render 2 ms; every frame must have the left channel high and the right
     * low -- played as mono, the two would alternate in one channel instead */
    nheard = 0;
    audio_advance((uint64_t)(PIT_HZ / 500));
    int got = 0;
    for (int i = 0; i < nheard; i++)
        if (heard[i * 2] || heard[i * 2 + 1]) { assert(heard[i * 2] > 0 && heard[i * 2 + 1] < 0); got++; }
    assert(got > 50);
}

static void test_images(void)
{
    /* placed at its lifted segment, live, found by address; a table loaded
     * over it later is a different image (the tables share segments) */
    uint8_t raw[16] = {0};
    images_placed("EP1.EXE", raw, 16, 0x4000, 0x3000);
    void (*fn)(CPU *) = images_lookup(0x4000 + 0x234, 0x5);  /* linear 0x2345 inside */
    assert(fn == f2345);
    fn(&g_cpu); assert(ran == 0x2345);
    assert(images_lookup(0x4001, 0) == f10);
    images_gone(0x4000);
    assert(images_lookup(0x4001, 0) == NULL);
}

int main(void)
{
    g_cpu.mem = (uint8_t *)calloc(1, MEM_SIZE);
    test_split_screen();
    test_sb16_stereo();
    test_images();
    printf("selftest: ok\n");
    return 0;
}
