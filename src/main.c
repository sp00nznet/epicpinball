/*
 * main.c - boot PINBALL.EXE, lifted, on the DOS machine. It EXECs the tables
 * itself, exactly as it would under DOS (src/dos.c, src/images.c).
 *
 *   epicpinball [options] [-- arguments for PINBALL.EXE]
 *
 *   --data DIR        the game's files (default: original)
 *   --save DIR        CONFIG.PIN, high scores, sound setup (default: saves)
 *   --headless        no window, no audio device; time is deterministic
 *   --realtime        headless, but run on the wall clock
 *   --record OUT.mp4  record video + audio through ffmpeg (combine with
 *                     --headless for an unattended capture)
 *   --seconds N       quit after N seconds of emulated time
 *   --keys LIST       scripted input: "ms:KEY[+holdms],..." e.g. "4000:SPACE,6000:LSHIFT+300"
 *   --mute            silence
 *   --scale N         window size, multiples of 320x240 (default 3)
 *   --fullscreen      start fullscreen (Alt+Enter toggles)
 *   --display MODE    sharp (4:3, default), pixel (whole multiples) or crt (F11 cycles)
 *   --trace           log DOS calls to stderr
 *   --shot FILE.bmp   write the last frame at exit
 *   --shot-at LIST    frames at emulated times: "ms:file.bmp,..."
 *   --wav OUT.wav     write the audio (no ffmpeg needed)
 */
#include "machine.h"
#include <setjmp.h>
#include <stdarg.h>
#ifdef _WIN32
#include <direct.h>
#endif

Options g_opt;
extern jmp_buf g_exit_jmp;
extern int g_exit_code;
void host_snapshot(const char *path);

void trace(const char *fmt, ...)
{
    if (!g_opt.trace) return;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

void fatal(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "error: "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static void mkdirs(const char *p)
{
    char b[512]; snprintf(b, sizeof b, "%s", p);
    for (char *s = b + 1; *s; s++)
        if (*s == '/' || *s == '\\') { char c = *s; *s = 0; _mkdir(b); *s = c; }
    _mkdir(b);
}

int main(int argc, char **argv)
{
    static char tail[256];
    const char *shot = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--data") && v) { g_opt.datadir = v; i++; }
        else if (!strcmp(a, "--save") && v) { g_opt.savedir = v; i++; }
        else if (!strcmp(a, "--headless")) g_opt.headless = 1;
        else if (!strcmp(a, "--record") && v) { g_opt.record = v; i++; }
        else if (!strcmp(a, "--seconds") && v) { g_opt.seconds = atof(v); i++; }
        else if (!strcmp(a, "--keys") && v) { g_opt.keys = v; i++; }
        else if (!strcmp(a, "--shot-at") && v) { g_opt.shots = v; i++; }
        else if (!strcmp(a, "--wav") && v) { g_opt.wav = v; i++; }
        else if (!strcmp(a, "--mute")) g_opt.mute = 1;
        else if (!strcmp(a, "--trace")) g_opt.trace = 1;
        else if (!strcmp(a, "--scale") && v) { g_opt.scale = atoi(v); i++; }
        else if (!strcmp(a, "--fullscreen")) g_opt.fullscreen = 1;
        else if (!strcmp(a, "--realtime")) g_opt.realtime = 1;
        else if (!strcmp(a, "--display") && v) { g_opt.display = v; i++; }
        else if (!strcmp(a, "--shot") && v) { shot = v; i++; }
        else if (!strcmp(a, "--")) {
            for (i++; i < argc; i++) {
                if (tail[0]) strncat(tail, " ", sizeof tail - strlen(tail) - 1);
                strncat(tail, argv[i], sizeof tail - strlen(tail) - 1);
            }
        } else { fprintf(stderr, "unknown option %s (see the top of src/main.c)\n", a); return 2; }
    }
    if (!g_opt.datadir) g_opt.datadir = "original";
    if (!g_opt.savedir) g_opt.savedir = "saves";
    g_opt.args = tail;
    mkdirs(g_opt.savedir);

    if (cpu_alloc_mem(&g_cpu) != 0) return 1;
    vga_init();
    audio_init();
    machine_init();
    host_init();
    dos_boot(&g_cpu);
    vga_start();

    if (!setjmp(g_exit_jmp)) {
        void (*entry)(CPU *) = images_lookup(g_cpu.cs, g_cpu.ip);
        if (!entry) fatal("PINBALL.EXE is not lifted -- run tools/lift.py, then build");
        entry(&g_cpu);
        fprintf(stderr, "[dos] the program returned without exiting\n");
    }
    machine_report();
    { void vgaprof_report(void); vgaprof_report(); }
    if (shot) host_snapshot(shot);
    host_shutdown();
    return g_exit_code;
}
