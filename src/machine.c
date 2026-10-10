/*
 * machine.c - the PC around the CPU: PIC, PIT, keyboard controller, speaker
 * gate, and getting interrupts into lifted code.
 *
 * Lifted code runs to completion inside C calls, so nothing can interrupt it
 * the way an IRQ interrupts a real CPU. Instead every loop back-edge counts
 * down a budget (RECOMP_TICK in cpu.h) and recomp_tick() runs here: it works
 * out how much emulated time has passed, delivers the timer interrupts that
 * fell due in order -- rendering audio up to each one, so a DSP command lands
 * in the stream where it was made -- then keyboard and Sound Blaster IRQs,
 * and gives the host a frame. Delivering at back-edges is also where a real
 * CPU would most likely have taken them: the game spins on flags its own
 * interrupt handlers set.
 *
 * See docs/architecture.md, "Time".
 */
#include "machine.h"
#ifdef _WIN32
#include <windows.h>
#ifdef EPIC_DEBUG
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif
#endif

CPU g_cpu;
int g_recomp_tick_budget = 1;
int g_tick_hold;                         /* no interrupts, no time */

/* ---- host clock ------------------------------------------------------------ */

uint64_t host_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart * 1000000 + (c.QuadPart % f.QuadPart) * 1000000 / f.QuadPart);
#else
    return 0;
#endif
}

static uint64_t t_start;
static uint64_t emu_now;              /* PIT ticks, what the guest has been shown */

uint64_t pit_now(void) { return emu_now; }

/* ---- PIC ----------------------------------------------------------------- */

static uint8_t pic_mask[2] = { 0xB8, 0xFF };   /* timer, keyboard, cascade, IRQ 5/7? set by the game */
static int in_irq;                     /* one interrupt at a time; no nesting */

/* ---- PIT ----------------------------------------------------------------- */

static uint32_t pit_div[3] = { 65536, 65536, 65536 };
static uint8_t  pit_mode_access[3] = { 3, 3, 3 };
static int      pit_flip[3];
static uint16_t pit_latch[3];
static int      pit_latched[3];
static uint64_t next_t0;
static int      t0_pending;

static uint16_t pit_count(int ch)
{
    uint64_t period = pit_div[ch];
    return (uint16_t)(period - (emu_now % period));
}

/* ---- keyboard controller ---------------------------------------------------- */

static uint8_t kq[64];
static int kq_head, kq_tail;
static uint8_t port60, port61 = 0x00;

void kbd_scancode(uint8_t sc)
{
    int n = (kq_tail + 1) & 63;
    if (n != kq_head) { kq[kq_tail] = sc; kq_tail = n; }
}

/* ---- ports ------------------------------------------------------------- */

int machine_port_in(uint16_t port, uint8_t *v)
{
    switch (port) {
    case 0x20: case 0xA0: *v = 0; return 1;
    case 0x21: *v = pic_mask[0]; return 1;
    case 0xA1: *v = pic_mask[1]; return 1;
    case 0x40: case 0x41: case 0x42: {
        int ch = port - 0x40;
        uint16_t c = pit_latched[ch] ? pit_latch[ch] : pit_count(ch);
        int acc = pit_mode_access[ch];
        if (acc == 1) *v = (uint8_t)c;
        else if (acc == 2) *v = (uint8_t)(c >> 8);
        else { *v = pit_flip[ch] ? (uint8_t)(c >> 8) : (uint8_t)c; pit_flip[ch] ^= 1;
               if (!pit_flip[ch]) pit_latched[ch] = 0; }
        return 1;
    }
    case 0x60: *v = port60; return 1;
    case 0x61: {
        static uint8_t refresh;
        refresh ^= 0x10;
        *v = (uint8_t)((port61 & 0x0F) | refresh);
        return 1;
    }
    case 0x64: *v = (uint8_t)(0x14 | (kq_head != kq_tail ? 1 : 0)); return 1;
    case 0x201: *v = 0xFF; return 1;           /* no joystick: the axis bits never fall */
    }
    return 0;
}

int machine_port_out(uint16_t port, uint8_t v)
{
    switch (port) {
    case 0x20: case 0xA0: return 1;            /* EOI: nothing is held in service */
    case 0x21: pic_mask[0] = v; return 1;
    case 0xA1: pic_mask[1] = v; return 1;
    case 0x43: {
        int ch = v >> 6;
        if (ch == 3) return 1;                 /* read-back: not used */
        if (((v >> 4) & 3) == 0) { pit_latch[ch] = pit_count(ch); pit_latched[ch] = 1; pit_flip[ch] = 0; return 1; }
        pit_mode_access[ch] = (v >> 4) & 3;
        pit_flip[ch] = 0;
        return 1;
    }
    case 0x40: case 0x41: case 0x42: {
        int ch = port - 0x40;
        static uint16_t acc[3];
        int a = pit_mode_access[ch];
        if (a == 1) acc[ch] = (acc[ch] & 0xFF00) | v;
        else if (a == 2) acc[ch] = (uint16_t)((acc[ch] & 0xFF) | v << 8);
        else if (!pit_flip[ch]) { acc[ch] = (acc[ch] & 0xFF00) | v; pit_flip[ch] = 1; return 1; }
        else { acc[ch] = (uint16_t)((acc[ch] & 0xFF) | v << 8); pit_flip[ch] = 0; }
        pit_div[ch] = acc[ch] ? acc[ch] : 65536;
        if (ch == 0) { next_t0 = emu_now + pit_div[0]; trace("[pit] timer 0 at %.1f Hz\n", PIT_HZ / pit_div[0]); }
        if (ch == 2) audio_speaker((port61 & 3) == 3, (uint16_t)pit_div[2]);
        return 1;
    }
    case 0x60: case 0x64: return 1;            /* LEDs, controller commands */
    case 0x61:
        port61 = v;
        audio_speaker((v & 3) == 3, (uint16_t)pit_div[2]);
        return 1;
    case 0x201: return 1;
    }
    return 0;
}

static int warned_ports[0x400];

uint8_t port_in8(CPU *cpu, uint16_t port)
{
    uint8_t v = 0xFF;
    (void)cpu;
    if (vga_port_in(port, &v) || audio_port_in(port, &v) || machine_port_in(port, &v)) return v;
    if (port < 0x400 && !warned_ports[port]++) trace("[port] in %03X\n", port);
    return 0xFF;
}

void port_out8(CPU *cpu, uint16_t port, uint8_t v)
{
    (void)cpu;
    if (vga_port_out(port, v) || audio_port_out(port, v) || machine_port_out(port, v)) return;
    if (port < 0x400 && !warned_ports[port]++) trace("[port] out %03X = %02X\n", port, v);
}

uint16_t port_in16(CPU *cpu, uint16_t port)
{
    return (uint16_t)(port_in8(cpu, port) | port_in8(cpu, (uint16_t)(port + 1)) << 8);
}

void port_out16(CPU *cpu, uint16_t port, uint16_t v)
{
    port_out8(cpu, port, (uint8_t)v);
    port_out8(cpu, (uint16_t)(port + 1), (uint8_t)(v >> 8));
}

/* ---- dispatch ----------------------------------------------------------- */

#define lookup images_lookup
#define miss images_miss

/* `jmp far [old]` / `pushf; call far [old]` into the BIOS: chain to the
 * built-in handler and return through its IRET. */
static void stub_iret(CPU *cpu, uint16_t off)
{
    bios_builtin(cpu, off);
    cpu->sp += 4;
    cpu->flags = pop16(cpu);
}

void recomp_dispatch(CPU *cpu, uint16_t seg, uint16_t off)
{
    if (seg == bios_stub_seg()) { stub_iret(cpu, off); return; }
    /* A routine that pops its own return address and jumps to it (Borland's
     * _setargv, which builds argv on the stack under itself) jumps to the
     * lifter's 0xFFFF sentinel: that is a return to the C caller, and the
     * guest has already taken the address off the stack. */
    if (off == 0xFFFF) return;
    void (*fn)(CPU *) = lookup(seg, off);
    if (fn) fn(cpu); else miss("jmp", seg, off);
}

void dispatch_near(CPU *cpu, uint16_t cs, uint16_t off)
{
    void (*fn)(CPU *) = lookup(cs, off);
    if (fn) fn(cpu); else { miss("call near", cs, off); cpu->sp += 2; }
}

void dispatch_far(CPU *cpu, uint16_t seg, uint16_t off)
{
    if (seg == bios_stub_seg()) { stub_iret(cpu, off); return; }
    void (*fn)(CPU *) = lookup(seg, off);
    if (fn) fn(cpu); else { miss("call far", seg, off); cpu->sp += 4; }
}

/* INT n: whatever the IVT holds, with the frame a real INT pushes. */
void call_vector(CPU *cpu, unsigned vec)
{
    uint8_t *v = &cpu->mem[vec * 4];
    uint16_t off = (uint16_t)(v[0] | v[1] << 8), seg = (uint16_t)(v[2] | v[3] << 8);
    if (seg == bios_stub_seg()) {
        if (!bios_builtin(cpu, off)) trace("[int] no handler for INT %02X\n", vec);
        return;
    }
    void (*fn)(CPU *) = lookup(seg, off);
    if (!fn) { miss("int", seg, off); return; }
    push16(cpu, cpu->flags);
    push16(cpu, seg);
    push16(cpu, 0xFFFF);
    cpu->flags &= ~(FLAG_IF | FLAG_TF);
    fn(cpu);                                   /* its IRET pops the frame */
}

void int_handler(CPU *cpu, unsigned num) { call_vector(cpu, num); }

static void irq(CPU *cpu, int line)
{
    in_irq = 1;
    call_vector(cpu, line < 8 ? 8 + line : 0x70 + line - 8);
    in_irq = 0;
}

static int irq_ok(CPU *cpu, int line)
{
    return !in_irq && (cpu->flags & FLAG_IF) && !(pic_mask[line >> 3] & (1 << (line & 7)));
}

int  sb_irq_pending(void);
int  sb_irq_line(void);
void sb_irq_ack(void);

/* Emulated time. Windowed it is the wall clock -- the game should feel like
 * a game. Headless it is deterministic: every interrupt poll (one per 2000
 * loop back-edges, or a DOS/BIOS call that waits) moves time on by POLL_US,
 * so a run depends on the guest's own progress and never on how busy the
 * host is. Scripted keys, frame grabs and recordings then land on the same
 * screens every time; with wall-clock time the conformance scripts raced
 * Planet Strike's title fade and lost. POLL_US is sized so the guest runs
 * at roughly the speed of a fast 386 (docs/architecture.md, "Time"). */
#define POLL_US 300
static uint64_t virt_us, polls;

uint64_t emu_us(void)
{
    return g_opt.headless && !g_opt.realtime ? virt_us : host_us() - t_start;
}

/* A port read that a guest polls in a loop costs emulated time, as an ISA
 * read costs about a microsecond on the real bus. Without it a loop inside
 * an interrupt handler -- Epic Pinball's timer ISR waits for vertical
 * retrace on 3DAh, to keep its PIT locked to the display -- spins forever in
 * deterministic time, because no interrupt poll runs (and so no time
 * passes) until the handler returns. */
void machine_port_wait(unsigned us) { virt_us += us; }

/* The host stopped the game for a while (its settings menu): that time did
 * not pass for the guest. */
void machine_paused(uint64_t us) { t_start += us; }

void machine_report(void)
{
    trace("[time] %llu polls, %.1f s emulated\n", (unsigned long long)polls, emu_us() / 1e6);
}

static int busy;                      /* inside recomp_tick: no re-entry */
int machine_busy(void) { return busy; }

/* A program ended (INT 21h/4Ch) from inside an interrupt handler -- Epic's
 * tables can (the demo does) -- and dos_exit's longjmp
 * skipped the rest of recomp_tick and irq(). Put their state back, or no
 * interrupt is ever delivered again. */
void machine_unwound(void) { busy = 0; in_irq = 0; }

void recomp_tick(CPU *cpu)
{
    g_recomp_tick_budget = 2000;
    if (busy || g_tick_hold) return;
    busy = 1;
    polls++;
    virt_us += POLL_US;

    uint64_t target = (uint64_t)((double)emu_us() * (PIT_HZ / 1e6));
    if (target > next_t0 + (uint64_t)(PIT_HZ / 10)) {
        /* far behind (a long load, a debugger): drop the backlog rather than
         * replay a storm of ticks the game would see as a time warp anyway */
        next_t0 = target - pit_div[0];
    }
    while (next_t0 <= target) {
        audio_advance(next_t0);
        emu_now = next_t0;
        next_t0 += pit_div[0];
        t0_pending = 1;
        if (irq_ok(cpu, 0)) { t0_pending = 0; irq(cpu, 0); }
        else break;                            /* masked or cli: wait, one pending at most */
    }
    if (t0_pending && irq_ok(cpu, 0)) { t0_pending = 0; irq(cpu, 0); }
    if (target > emu_now) { audio_advance(target); emu_now = target; }

    while (kq_head != kq_tail && irq_ok(cpu, 1)) {
        port60 = kq[kq_head];
        kq_head = (kq_head + 1) & 63;
        irq(cpu, 1);
    }
    if (sb_irq_pending() && irq_ok(cpu, sb_irq_line())) { sb_irq_ack(); irq(cpu, sb_irq_line()); }

    host_frame();
    busy = 0;
}

#if defined(RECOMP_TRACE) && defined(_WIN32)
/* Trace builds: if the guest stops polling for interrupts for 3 s of host
 * time it is spinning somewhere no back-edge ticks (or deadlocked); say
 * where, from the lifted-call ring. */
static HANDLE main_thread;

/* Where the main thread is right now, as generated-C file:line -- each line
 * of lifted C carries its guest instruction as a comment. */
static void where(void)
{
    HANDLE p = GetCurrentProcess();
    CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
    static int init;
    if (!init) { init = 1; SymSetOptions(SYMOPT_LOAD_LINES); SymInitialize(p, NULL, TRUE); }
    for (int k = 0; k < 4; k++) {          /* a few samples: a loop shows its range */
        SuspendThread(main_thread);
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(main_thread, &ctx)) {
            STACKFRAME64 sf = {0};
            sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
            sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
            sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
            fprintf(stderr, "  [stuck]");
            for (int d = 0; d < 24 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, p, main_thread, &sf, &ctx, NULL,
                                                 SymFunctionTableAccess64, SymGetModuleBase64, NULL); d++) {
                char buf[sizeof(SYMBOL_INFO) + 128]; SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
                IMAGEHLP_LINE64 ln = { sizeof ln }; DWORD disp = 0;
                si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 127;
                const char *fn = SymFromAddr(p, sf.AddrPC.Offset, NULL, si) ? si->Name : "?";
                if (SymGetLineFromAddr64(p, sf.AddrPC.Offset, &disp, &ln)) {
                    const char *b = strrchr(ln.FileName, '\\');
                    fprintf(stderr, " %s(%s:%lu)", fn, b ? b + 1 : ln.FileName, ln.LineNumber);
                } else fprintf(stderr, " %s", fn);
            }
            fprintf(stderr, "\n");
        }
        ResumeThread(main_thread);
        Sleep(7);
    }
}

static DWORD WINAPI watchdog(LPVOID arg)
{
    uint64_t last = (uint64_t)-1; int quiet = 0;
    (void)arg;
    for (;;) {
        Sleep(1000);
        if (polls != last) { last = polls; quiet = 0; continue; }
        if (++quiet == 3) {
            void recomp_trace_dump(const char *);
            fprintf(stderr, "[watchdog] no interrupt poll for 3 s (%llu polls, busy %d, in_irq %d, budget %d)\n",
                    (unsigned long long)polls, busy, in_irq, g_recomp_tick_budget);
            recomp_trace_dump("stuck");
            where();
            fflush(stderr);
        }
    }
}
#endif

void machine_init(void)
{
    t_start = host_us();
    next_t0 = pit_div[0];
#if defined(RECOMP_TRACE) && defined(_WIN32)
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread,
                    0, FALSE, DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
#endif
}

void recomp_div0(const char *what)
{
    static unsigned n;
    if (n++ < 20) {
        fprintf(stderr, "[cpu] divide by zero (%s)\n", what);
        { void host_backtrace(void); host_backtrace(); }
    }
    call_vector(&g_cpu, 0);
}

int cpu_lar(CPU *cpu, uint16_t sel, uint16_t *out) { (void)cpu; (void)sel; (void)out; return 0; }
int cpu_lsl(CPU *cpu, uint16_t sel, uint16_t *out) { (void)cpu; (void)sel; (void)out; return 0; }

int cpu_alloc_mem(CPU *cpu)
{
    cpu->mem = (uint8_t *)calloc(1, MEM_SIZE);
    return cpu->mem ? 0 : -1;
}

/* ---- debugging: EPIC_WATCH=<linear hex> reports every write to that byte
 * with the lifted functions most recently entered (build with
 * -DEPIC_TRACE=ON so the lifted code records them). */
#ifdef RECOMP_TRACE
static const char *ring[64];
static unsigned ring_n;
/* EPIC_CALLS=from-to (ms of game time): each lifted function entered in
 * that window, once, in order of first entry -- diff two runs to find the
 * code a key press reaches */
void recomp_enter(const char *fn)
{
    ring[ring_n++ & 63] = fn;
    { void images_check(void); images_check(); }
    static int init;
    static unsigned from, to;
    static const char *seen[8192];
    static int nseen;
    if (!init) { init = 1; const char *e = getenv("EPIC_CALLS"); if (!e || sscanf(e, "%u-%u", &from, &to) != 2) to = 0; }
    if (!to) return;
    uint64_t ms = emu_us() / 1000;
    if (ms < from || ms >= to) return;
    for (int i = 0; i < nseen; i++) if (seen[i] == fn) return;
    if (nseen < 8192) seen[nseen++] = fn;
    fprintf(stderr, "[call] %s\n", fn);
}
void recomp_trace_dump(const char *why)
{
    fprintf(stderr, "  [%s] last functions:", why);
    for (unsigned i = ring_n > 12 ? ring_n - 12 : 0; i < ring_n; i++) fprintf(stderr, " %s", ring[i & 63]);
    fprintf(stderr, "\n");
}
#endif

/* The host stack, symbolised (debug builds carry a PDB): lifted functions
 * keep their fn_ names, so this is the guest call chain. */
void host_backtrace(void)
{
#if defined(EPIC_DEBUG) && defined(_WIN32)
    static int init;
    HANDLE p = GetCurrentProcess();
    if (!init) { init = 1; SymInitialize(p, NULL, TRUE); }
    void *fr[40]; USHORT n = CaptureStackBackTrace(1, 40, fr, NULL);
    char buf[sizeof(SYMBOL_INFO) + 128]; SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 127;
    fprintf(stderr, "   ");
    for (USHORT i = 0; i < n; i++)
        if (SymFromAddr(p, (DWORD64)(size_t)fr[i], NULL, si)) fprintf(stderr, " %s", si->Name);
    fprintf(stderr, "\n");
#endif
}

void watch_write(uint32_t a, uint8_t v)
{
    static uint32_t w = 1;
    if (w == 1) { const char *e = getenv("EPIC_WATCH"); w = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
    if (!w || (a != w && a != w + 1)) return;
    fprintf(stderr, "[watch] %05X = %02X\n", a, v);
    host_backtrace();
}

/* EPIC_VGAPROF=1 (debug builds): which lifted functions write video memory,
 * by return address, symbolised at exit. Finds the wall scalers, the plane
 * drawer and the sprite scalers without reading every routine. */
#if defined(EPIC_DEBUG) && defined(_WIN32)
#include <intrin.h>
static struct { void *ra; unsigned long n; } vp[256];
static int vp_on = -1;
void vgaprof_note(void *ra)
{
    if (vp_on < 0) vp_on = getenv("EPIC_VGAPROF") != NULL;
    if (!vp_on) return;
    for (int i = 0; i < 256; i++) {
        if (vp[i].ra == ra) { vp[i].n++; return; }
        if (!vp[i].ra) { vp[i].ra = ra; vp[i].n = 1; return; }
    }
}
void vgaprof_report(void)
{
    if (vp_on <= 0) return;
    HANDLE p = GetCurrentProcess();
    SymInitialize(p, NULL, TRUE);
    char buf[sizeof(SYMBOL_INFO) + 128]; SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    unsigned long tot[256] = {0}; char names[256][64]; int nn = 0;
    for (int i = 0; i < 256 && vp[i].ra; i++) {
        si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 127;
        const char *nm = SymFromAddr(p, (DWORD64)(size_t)vp[i].ra, NULL, si) ? si->Name : "?";
        int k = 0;
        for (; k < nn; k++) if (!strcmp(names[k], nm)) break;
        if (k == nn) { strncpy(names[nn], nm, 63); names[nn][63] = 0; nn++; }
        tot[k] += vp[i].n;
    }
    for (int k = 0; k < nn; k++) fprintf(stderr, "[vgaprof] %10lu %s\n", tot[k], names[k]);
}
#else
void vgaprof_report(void) {}
#endif
