/*
 * host.c - the modern machine's side: a window drawn by the presenter
 * (present.c: Direct3D 11 on Windows, GDI if that fails; SDL2 on Linux),
 * waveOut or SDL audio, keyboard as raw PC scancodes, an Xbox pad (pad.c) --
 * or, with --headless, none of that: frames are composed offscreen and, with
 * --record, piped to ffmpeg with the audio muxed in at the end. Headless
 * never touches the desktop, so it works over RDP and from CI (repo rules,
 * section 13).
 *
 * The remaster's settings (scaling, colour, the whole table, the bars beside
 * the picture, fullscreen, the window's place, smooth sound) are kept in
 * epicpinball.ini in the save folder, changed with F7, F9-F12 or the menu on
 * right-click / F10. Headless runs ignore the file: what they draw is set on
 * the command line only, so a test never depends on someone's preferences.
 *
 * Scripted input (--keys "ms:KEY,...") presses and releases keys at fixed
 * points in emulated time, which is how the harness walks the menus and
 * plays the tables.
 */
#include "machine.h"
#include <ctype.h>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <direct.h>
#define popen _popen
#define pclose _pclose
#else
#include <SDL.h>
#endif

#define FRAME_MAX (640 * 480)
static uint32_t frame[FRAME_MAX];
static uint32_t big[FRAME_MAX];
static int fw = 320, fh = 200;
static double frame_aspect = 4.0 / 3;

static Present S = { PRESENT_SHARP, 0, 1, 0 };
static const char *const mode_names[] = { "sharp", "smooth", "crt", "nearest", "integer" };

/* The game's picture as the monitor showed it: what --record and the
 * conformance shots see. */
static void compose(void)
{
    vga_compose(frame, &fw, &fh);
    frame_aspect = 4.0 / 3;
}

/* What the player sees: the whole table when that is on and a table is up */
static void compose_view(void)
{
    if (S.whole && !g_opt.classic && vga_compose_table(frame, &fw, &fh)) frame_aspect = (double)fw / fh;
    else compose();
}

void present_rect(int cw, int ch, int w, int h, double aspect, int mode, RECT *r);
#ifdef _WIN32
int  present_window(HWND hw, const uint32_t *f, int w, int h, double aspect, const Present *s);
#else
int  present_window(SDL_Window *wnd, const uint32_t *f, int w, int h, double aspect, const Present *s);
#endif
int  present_offscreen(const uint32_t *f, int w, int h, double aspect, const Present *s,
                       int ow, int oh, uint32_t *out);
void pad_load(void);
void pad_poll(void);
const char *pad_ini(void);

/* ---- recording ------------------------------------------------------------ */

#define REC_FPS 35
static FILE *rec_video, *rec_audio;
static uint64_t rec_frames, rec_samples;
static char rec_vpath[512], rec_apath[512];

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t data = frames * 4, riff = 36 + data, rate = AUDIO_RATE, br = AUDIO_RATE * 4, fmt = 16;
    uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

static int rec_w = 640, rec_h = 400;

static void rec_open(void)
{
    snprintf(rec_vpath, sizeof rec_vpath, "%s.video.mp4", g_opt.record);
    snprintf(rec_apath, sizeof rec_apath, "%s.audio.wav", g_opt.record);
    char cmd[1200];
    snprintf(cmd, sizeof cmd,
             "ffmpeg -y -loglevel error -f rawvideo -pixel_format bgra -video_size %dx%d "
             "-framerate %d -i - -vf scale=%d:%d:flags=neighbor -c:v libx264 -pix_fmt yuv420p "
             "-crf 18 \"%s\"", rec_w, rec_h, REC_FPS, rec_w < 1280 ? 1280 : rec_w,
             (rec_w < 1280 ? 1280 : rec_w) * 3 / 4, rec_vpath);
#ifdef _WIN32
    rec_video = popen(cmd, "wb");
#else
    rec_video = popen(cmd, "w");
#endif
    if (!rec_video) fatal("cannot start ffmpeg (is it on PATH?)");
    rec_audio = fopen(rec_apath, "wb");
    if (rec_audio) wav_header(rec_audio, 0);
}

static void rec_close(void)
{
    if (!rec_video) return;
    pclose(rec_video);
    rec_video = NULL;
    if (rec_audio) { wav_header(rec_audio, (uint32_t)rec_samples); fclose(rec_audio); rec_audio = NULL; }
    char cmd[1600];
    snprintf(cmd, sizeof cmd, "ffmpeg -y -loglevel error -i \"%s\" -i \"%s\" -c:v copy -c:a aac -b:a 192k -shortest \"%s\"",
             rec_vpath, rec_apath, g_opt.record);
    if (system(cmd) == 0) { remove(rec_vpath); remove(rec_apath); }
    fprintf(stderr, "[rec] %s: %llu frames\n", g_opt.record, (unsigned long long)rec_frames);
}

/* Every frame to the encoder at one size, whatever mode the game is in */
static const uint32_t *as_rec(void)
{
    if (fw == rec_w && fh == rec_h) return frame;
    for (int y = 0; y < rec_h; y++)
        for (int x = 0; x < rec_w; x++)
            big[y * rec_w + x] = frame[(y * fh / rec_h) * fw + x * fw / rec_w];
    return big;
}

static FILE *wav_out;
static uint64_t wav_samples;

void host_snapshot(const char *path);

/* --shot-at "ms:file,...": frames at fixed points in emulated time, for the
 * conformance harness (tools/conformance.py) */
static void shots_due(uint64_t now)
{
    static const char *p;
    static int init;
    if (!init) { init = 1; p = g_opt.shots; }
    while (p && *p) {
        double ms; char path[260]; int n = 0;
        if (sscanf(p, "%lf:%259[^,]%n", &ms, path, &n) < 2) { p = NULL; return; }
        if (now < (uint64_t)(ms * PIT_HZ / 1000.0)) return;
        host_snapshot(path);
        p += n; if (*p == ',') p++;
    }
}

/* ---- scripted keys ----------------------------------------------------------- */

typedef struct { uint64_t at; uint8_t sc; uint8_t up; } KeyEv;
static KeyEv kev[512];
static int nkev, kev_i;

/* A key's scan code by name, | 0x100 when it is E0-prefixed (the arrows) */
int host_key_by_name(const char *s)
{
    static const struct { const char *n; int sc; } t[] = {
        {"ESC",1},{"ENTER",0x1C},{"SPACE",0x39},{"UP",0x48|0x100},{"DOWN",0x50|0x100},
        {"LEFT",0x4B|0x100},{"RIGHT",0x4D|0x100},{"CTRL",0x1D},{"ALT",0x38},{"SHIFT",0x2A},
        {"TAB",0x0F},{"BKSP",0x0E},{"Y",0x15},{"N",0x31},{"F1",0x3B},{"F2",0x3C},{"F3",0x3D},
        {"F10",0x44},{"LSHIFT",0x2A},{"RSHIFT",0x36},{"Z",0x2C},{"SLASH",0x35},{"1",2},{"2",3},{"3",4},{"4",5},{"5",6},{"6",7},{NULL,0}};
    for (int i = 0; t[i].n; i++) if (!_stricmp(s, t[i].n)) return t[i].sc;
    return (int)strtol(s, NULL, 16);
}

static void keys_parse(void)
{
    const char *p = g_opt.keys;
    while (p && *p && nkev < 500) {
        char name[32]; double ms; int hold = 120, n = 0;
        if (sscanf(p, "%lf:%31[^,]%n", &ms, name, &n) < 2) break;
        char *h = strchr(name, '+');            /* KEY+holdms */
        if (h) { *h = 0; hold = atoi(h + 1); }
        int sc = host_key_by_name(name);
        uint64_t at = (uint64_t)(ms * PIT_HZ / 1000.0);
        kev[nkev++] = (KeyEv){ at, (uint8_t)sc, 0 };
        kev[nkev++] = (KeyEv){ at + (uint64_t)(hold * PIT_HZ / 1000.0), (uint8_t)sc, 1 };
        p += n; if (*p == ',') p++;
    }
    /* sort by time (insertion; small) */
    for (int i = 1; i < nkev; i++)
        for (int j = i; j > 0 && kev[j].at < kev[j - 1].at; j--) { KeyEv t = kev[j]; kev[j] = kev[j - 1]; kev[j - 1] = t; }
}

static int ext_of(int sc) { return sc == 0x48 || sc == 0x50 || sc == 0x4B || sc == 0x4D; }

static void keys_due(void)
{
    while (kev_i < nkev && kev[kev_i].at <= pit_now()) {
        int sc = kev[kev_i].sc;
        if (ext_of(sc)) kbd_scancode(0xE0);
        kbd_scancode((uint8_t)(sc | (kev[kev_i].up ? 0x80 : 0)));
        kev_i++;
    }
}

/* ---- settings ------------------------------------------------------------------ */

static int mode_by_name(const char *s)
{
    for (int i = 0; i < PRESENT_MODES; i++) if (!_stricmp(s, mode_names[i])) return i;
    return PRESENT_SHARP;
}

/* epicpinball.ini and controller.ini: [section], key = value, ; comments */
const char *ini_get(const char *path, const char *sec, const char *key, const char *def, char *out, int n)
{
    FILE *f = fopen(path, "r");
    char line[256], cur[64] = "";
    snprintf(out, n, "%s", def);
    while (f && fgets(line, sizeof line, f)) {
        char *s = line, *e;
        if ((e = strchr(s, ';')) != NULL) *e = 0;
        while (isspace((unsigned char)*s)) s++;
        for (e = s + strlen(s); e > s && isspace((unsigned char)e[-1]); ) *--e = 0;
        if (*s == '[') {
            if ((e = strchr(s, ']')) != NULL) *e = 0;
            snprintf(cur, sizeof cur, "%s", s + 1);
            continue;
        }
        if (_stricmp(cur, sec) || (e = strchr(s, '=')) == NULL) continue;
        char *v = e + 1;
        while (e > s && isspace((unsigned char)e[-1])) e--;
        *e = 0;
        while (isspace((unsigned char)*v)) v++;
        if (!_stricmp(s, key)) { snprintf(out, n, "%s", v); break; }
    }
    if (f) fclose(f);
    return out;
}

static char ini[512];
static Present saved;                /* what the ini says: the command line never reaches it */
static int saved_full;
static RECT win_rect;                /* the window's place when not fullscreen */

static void settings_save(void)
{
    FILE *f = *ini ? fopen(ini, "w") : NULL;
    if (!f) return;
    fprintf(f, "[display]\nscale=%s\nlook=%s\nbars=%s\nwhole_table=%d\nfullscreen=%d\n",
            mode_names[saved.mode], saved.look ? "vivid" : "natural", saved.bars ? "blur" : "black",
            saved.whole, saved_full);
    if (win_rect.right > win_rect.left)
        fprintf(f, "window=%ld,%ld,%ld,%ld\n", (long)win_rect.left, (long)win_rect.top,
                (long)(win_rect.right - win_rect.left), (long)(win_rect.bottom - win_rect.top));
    fprintf(f, "[sound]\noutput=%s\n", g_audio_smooth ? "smooth" : "original");
    fclose(f);
}

static void settings_load(void)
{
    char v[64];
    snprintf(ini, sizeof ini, "%s/epicpinball.ini", g_opt.savedir);
    S.mode = mode_by_name(ini_get(ini, "display", "scale", "sharp", v, sizeof v));
    S.look = !_stricmp(ini_get(ini, "display", "look", "natural", v, sizeof v), "vivid");
    S.bars = _stricmp(ini_get(ini, "display", "bars", "blur", v, sizeof v), "black") != 0;
    S.whole = atoi(ini_get(ini, "display", "whole_table", "0", v, sizeof v));
    saved = S;
    saved_full = atoi(ini_get(ini, "display", "fullscreen", "0", v, sizeof v));
    if (saved_full) g_opt.fullscreen = 1;
    long x, y, w, h;
    if (sscanf(ini_get(ini, "display", "window", "", v, sizeof v), "%ld,%ld,%ld,%ld", &x, &y, &w, &h) == 4
        && w > 100 && h > 100) {
        RECT r = { x, y, x + w, y + h };
#ifdef _WIN32
        if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL))   /* still on a screen */
#endif
            win_rect = r;
    }
    g_audio_smooth = _stricmp(ini_get(ini, "sound", "output", "smooth", v, sizeof v), "original") != 0;
}

/* The command line wins over the ini, and is all a headless run reads */
static void settings_args(void)
{
    if (g_opt.scale) S.mode = mode_by_name(g_opt.scale);
    if (g_opt.look) S.look = !_stricmp(g_opt.look, "vivid");
    if (g_opt.bars) S.bars = _stricmp(g_opt.bars, "black") != 0;
    if (g_opt.whole) S.whole = 1;
    if (g_opt.classic) { S.mode = PRESENT_NEAREST; S.look = S.bars = S.whole = 0; }
}

/* ---- window ---------------------------------------------------------------- */

static int quit_req, is_full;
static void set_fullscreen(int on);
static void open_file(const char *path);

/* PrtSc: what is on screen, to screenshots/epic-NNN.bmp */
static void screenshot_key(void)
{
    char p[256];
    _mkdir("screenshots");
    for (int n = 0; n < 1000; n++) {
        snprintf(p, sizeof p, "screenshots/epic-%03d.bmp", n);
        FILE *f = fopen(p, "rb");
        if (!f) { host_snapshot(p); return; }
        fclose(f);
    }
}

enum { ID_MODE = 100, ID_LOOK = 120, ID_WHOLE = 130, ID_BARS_BLUR, ID_BARS_BLACK, ID_FULL,
       ID_SOUND_SMOOTH, ID_SOUND_ORIG, ID_SHOT, ID_PAD };

/* A change made in the window: shown, and remembered */
static void command(int id)
{
    if (id >= ID_MODE && id < ID_MODE + PRESENT_MODES) S.mode = saved.mode = id - ID_MODE;
    else if (id == ID_LOOK || id == ID_LOOK + 1) S.look = saved.look = id - ID_LOOK;
    else if (id == ID_WHOLE) S.whole = saved.whole = !S.whole;
    else if (id == ID_BARS_BLUR || id == ID_BARS_BLACK) S.bars = saved.bars = id == ID_BARS_BLUR;
    else if (id == ID_FULL) { set_fullscreen(!is_full); saved_full = is_full; }
    else if (id == ID_SOUND_SMOOTH || id == ID_SOUND_ORIG) g_audio_smooth = id == ID_SOUND_SMOOTH;
    else if (id == ID_SHOT) screenshot_key();
    else if (id == ID_PAD) open_file(pad_ini());
    else return;
    settings_save();
}

#ifdef _WIN32
static HWND wnd;
static BITMAPINFO bmi;

static void open_file(const char *path) { ShellExecuteA(wnd, "open", path, NULL, NULL, SW_SHOWNORMAL); }

static void set_fullscreen(int on)
{
    if (on == is_full) return;
    DWORD st = GetWindowLong(wnd, GWL_STYLE);
    if (on) {
        MONITORINFO mi = { sizeof mi };
        GetWindowRect(wnd, &win_rect);
        GetMonitorInfo(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLong(wnd, GWL_STYLE, st & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLong(wnd, GWL_STYLE, st | WS_OVERLAPPEDWINDOW);
        SetWindowPos(wnd, NULL, win_rect.left, win_rect.top, win_rect.right - win_rect.left,
                     win_rect.bottom - win_rect.top, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    is_full = on;
}

/* GDI, when Direct3D is not there: a stretch, nearest or halftone, no extras */
static void paint_gdi(HDC dc)
{
    RECT c, r; GetClientRect(wnd, &c);
    present_rect(c.right, c.bottom, fw, fh, frame_aspect, S.mode, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    PatBlt(dc, 0, 0, c.right, r.top, BLACKNESS); PatBlt(dc, 0, r.bottom, c.right, c.bottom - r.bottom, BLACKNESS);
    PatBlt(dc, 0, 0, r.left, c.bottom, BLACKNESS); PatBlt(dc, r.right, 0, c.right - r.right, c.bottom, BLACKNESS);
    bmi.bmiHeader.biWidth = fw; bmi.bmiHeader.biHeight = -fh;
    SetStretchBltMode(dc, S.mode == PRESENT_SMOOTH ? HALFTONE : COLORONCOLOR);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchDIBits(dc, r.left, r.top, w, h, 0, 0, fw, fh, frame, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

static void paint(void)
{
    if (!g_opt.classic && present_window(wnd, frame, fw, fh, frame_aspect, &S)) return;
    HDC dc = GetDC(wnd); paint_gdi(dc); ReleaseDC(wnd, dc);
}

static void menu(int x, int y)
{
    static const char *const mode_labels[] = { "Sharp", "Smooth", "CRT", "Nearest (original pixels)", "Integer (whole multiples)" };
    HMENU m = CreatePopupMenu(), sc = CreatePopupMenu(), look = CreatePopupMenu(),
          bars = CreatePopupMenu(), snd = CreatePopupMenu();
    for (int i = 0; i < PRESENT_MODES; i++)
        AppendMenuA(sc, MF_STRING | (S.mode == i ? MF_CHECKED : 0), ID_MODE + i, mode_labels[i]);
    AppendMenuA(look, MF_STRING | (!S.look ? MF_CHECKED : 0), ID_LOOK, "Natural");
    AppendMenuA(look, MF_STRING | (S.look ? MF_CHECKED : 0), ID_LOOK + 1, "Vivid");
    AppendMenuA(bars, MF_STRING | (S.bars ? MF_CHECKED : 0), ID_BARS_BLUR, "Blurred");
    AppendMenuA(bars, MF_STRING | (!S.bars ? MF_CHECKED : 0), ID_BARS_BLACK, "Black");
    AppendMenuA(snd, MF_STRING | (g_audio_smooth ? MF_CHECKED : 0), ID_SOUND_SMOOTH, "Smooth");
    AppendMenuA(snd, MF_STRING | (!g_audio_smooth ? MF_CHECKED : 0), ID_SOUND_ORIG, "Original (stepped)");
    AppendMenuA(m, MF_STRING | (S.whole ? MF_CHECKED : 0), ID_WHOLE, "Whole table\tF7");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)look, "Colour\tF9");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sc, "Scaling\tF12");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)bars, "Beside the picture");
    AppendMenuA(m, MF_STRING | (is_full ? MF_CHECKED : 0), ID_FULL, "Fullscreen\tF11");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_POPUP, (UINT_PTR)snd, "Sound");
    AppendMenuA(m, MF_STRING, ID_PAD, "Controller mapping...");
    AppendMenuA(m, MF_STRING, ID_SHOT, "Screenshot\tPrtSc");
    uint64_t t = host_us();
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, wnd, NULL);
    machine_paused(host_us() - t);                 /* the game stood still meanwhile */
    DestroyMenu(m);                                /* and its submenus */
    command(id);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CLOSE: quit_req = 1; return 0;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); paint(); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SYSKEYDOWN: case WM_KEYDOWN: case WM_SYSKEYUP: case WM_KEYUP: {
        int up = (m == WM_KEYUP || m == WM_SYSKEYUP);
        if (wp == VK_RETURN && (lp & (1 << 29))) { if (!up) command(ID_FULL); return 0; }
        if (wp == VK_F4 && (lp & (1 << 29))) { quit_req = 1; return 0; }
        if (wp == VK_SNAPSHOT) { if (up) screenshot_key(); return 0; }   /* Windows sends PrtSc's up only */
        if (wp == VK_F7 || (wp >= VK_F9 && wp <= VK_F12)) {
            if (up) return 0;
            if (wp == VK_F7) command(ID_WHOLE);
            if (wp == VK_F9) command(ID_LOOK + !S.look);
            if (wp == VK_F10) { RECT r; GetWindowRect(h, &r); menu(r.left + 40, r.top + 40); }
            if (wp == VK_F11) command(ID_FULL);
            if (wp == VK_F12) command(ID_MODE + (S.mode + 1) % PRESENT_MODES);
            return 0;
        }
        if (!up && (lp & (1 << 30))) return 0;      /* the game does its own repeat */
        int sc = (lp >> 16) & 0xFF;
        if (lp & (1 << 24)) kbd_scancode(0xE0);
        kbd_scancode((uint8_t)(sc | (up ? 0x80 : 0)));
        return 0;
    }
    case WM_SYSCHAR: return 0;                       /* no menu beep on Alt */
    case WM_CONTEXTMENU: {                           /* the game has no mouse: right-click is ours */
        POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
        if (p.x == -1 && p.y == -1) { RECT r; GetWindowRect(h, &r); p.x = r.left + 40; p.y = r.top + 40; }
        menu(p.x, p.y);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) pad_load();   /* controller.ini may have been edited */
        break;
    case WM_EXITSIZEMOVE:
        if (!is_full && !IsZoomed(h)) { GetWindowRect(h, &win_rect); settings_save(); }
        break;
    }
    return DefWindowProcA(h, m, wp, lp);
}

/* waveOut: a ring the main thread fills and a few buffers cycling under it */
#define WBUF 8
#define WLEN 1024
static HWAVEOUT wout;
static WAVEHDR whdr[WBUF];
static int16_t wdata[WBUF][WLEN * 2];
static int wfill, wcur;

static void wave_open(void)
{
    WAVEFORMATEX f = { WAVE_FORMAT_PCM, 2, AUDIO_RATE, AUDIO_RATE * 4, 4, 16, 0 };
    if (waveOutOpen(&wout, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { wout = NULL; return; }
    for (int i = 0; i < WBUF; i++) {
        whdr[i].lpData = (LPSTR)wdata[i]; whdr[i].dwBufferLength = WLEN * 4;
        waveOutPrepareHeader(wout, &whdr[i], sizeof whdr[i]);
        whdr[i].dwFlags |= WHDR_DONE;
    }
}

static void wave_push(const int16_t *s, int frames)
{
    while (frames > 0) {
        WAVEHDR *h = &whdr[wcur];
        if (!(h->dwFlags & WHDR_DONE)) return;       /* all queued: drop rather than block */
        int n = WLEN - wfill; if (n > frames) n = frames;
        memcpy(&wdata[wcur][wfill * 2], s, n * 4);
        wfill += n; s += n * 2; frames -= n;
        if (wfill == WLEN) {
            h->dwFlags &= ~WHDR_DONE;
            waveOutWrite(wout, h, sizeof *h);
            wfill = 0; wcur = (wcur + 1) % WBUF;
        }
    }
}

static void window_open(void)
{
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "epicpinball";
    wc.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1));
    RegisterClassA(&wc);
    int x = win_rect.left, y = win_rect.top, w = win_rect.right - win_rect.left, h = win_rect.bottom - win_rect.top;
    if (g_opt.window || w <= 0) {                  /* --window N, or no place remembered */
        int s = g_opt.window ? g_opt.window : 3;
        RECT r = { 0, 0, 320 * s, 240 * s };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        x = y = CW_USEDEFAULT; w = r.right - r.left; h = r.bottom - r.top;
    }
    wnd = CreateWindowA("epicpinball", "Epic Pinball", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                        x, y, w, h, NULL, NULL, wc.hInstance, NULL);
    GetWindowRect(wnd, &win_rect);
    if (g_opt.fullscreen) set_fullscreen(1);
    if (!g_opt.mute) wave_open();
}

/* Messages, the pad, and the picture */
static void window_frame(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (GetForegroundWindow() == wnd) pad_poll();
    if (quit_req) dos_exit(0);
    compose_view();
    paint();
}

static void window_audio(const int16_t *s, int frames) { if (wout) wave_push(s, frames); }

static void window_close(void)
{
    if (wout) { waveOutReset(wout); waveOutClose(wout); }
    if (wnd) { settings_save(); DestroyWindow(wnd); }
}

#else /* SDL2: Linux */

static SDL_Window *wnd;
static SDL_AudioDeviceID aud;

static void open_file(const char *path)
{
    char cmd[600];
    snprintf(cmd, sizeof cmd, "xdg-open \"%s\" >/dev/null 2>&1 &", path);
    if (system(cmd)) {}
}

/* The window's place, kept for the ini while it is not fullscreen or maximised */
static void remember_place(void)
{
    if (is_full || (SDL_GetWindowFlags(wnd) & SDL_WINDOW_MAXIMIZED)) return;
    int x, y, w, h;
    SDL_GetWindowPosition(wnd, &x, &y);
    SDL_GetWindowSize(wnd, &w, &h);
    win_rect = (RECT){ x, y, x + w, y + h };
}

static void set_fullscreen(int on)
{
    if (on == is_full) return;
    remember_place();
    SDL_SetWindowFullscreen(wnd, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    is_full = on;
}

/* SDL has no popup menus: the settings and keys, with buttons for what no key does */
static void menu(void)
{
    char msg[300];
    snprintf(msg, sizeof msg,
             "Whole table: %s (F7)\nColour: %s (F9)\nScaling: %s (F12)\nFullscreen: F11, Alt+Enter\n"
             "Beside the picture: %s\nSound: %s\nScreenshot: PrtSc",
             S.whole ? "on" : "off", S.look ? "vivid" : "natural", mode_names[S.mode],
             S.bars ? "blur" : "black", g_audio_smooth ? "smooth" : "original");
    const SDL_MessageBoxButtonData b[] = {
        { 0, S.bars ? ID_BARS_BLACK : ID_BARS_BLUR, S.bars ? "Black bars" : "Blurred bars" },
        { 0, g_audio_smooth ? ID_SOUND_ORIG : ID_SOUND_SMOOTH, g_audio_smooth ? "Original sound" : "Smooth sound" },
        { 0, ID_PAD, "Controller mapping..." },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Close" },
    };
    const SDL_MessageBoxData d = { SDL_MESSAGEBOX_INFORMATION, wnd, "Epic Pinball settings", msg, 4, b, NULL };
    int id = 0;
    uint64_t t = host_us();
    SDL_ShowMessageBox(&d, &id);
    machine_paused(host_us() - t);                 /* the game stood still meanwhile */
    command(id);
}

/* SDL's scancodes (USB usage numbers) as the PC's set 1, | 0x100 for E0 */
static int pc_scancode(int s)
{
    static const uint8_t t[101] = {
        [4]  = 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,   /* A-M */
               0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,   /* N-Z */
        [30] = 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,                     /* 1-0 */
        [40] = 0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28,   /* Enter... */
               0x29, 0x33, 0x34, 0x35, 0x3A,                                                   /* ...Caps */
        [58] = 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,         /* F1-F12 */
        [71] = 0x46, 0, 0x52, 0x47, 0x49, 0x53, 0x4F, 0x51, 0x4D, 0x4B, 0x50, 0x48,           /* ScrLk; Ins-Up */
        [83] = 0x45, 0x35, 0x37, 0x4A, 0x4E, 0x1C, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D,         /* keypad */
               0x47, 0x48, 0x49, 0x52, 0x53, 0x56,
    };
    switch (s) {
    case SDL_SCANCODE_LCTRL:  return 0x1D;
    case SDL_SCANCODE_LSHIFT: return 0x2A;
    case SDL_SCANCODE_LALT:   return 0x38;
    case SDL_SCANCODE_RCTRL:  return 0x11D;
    case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_RALT:   return 0x138;
    }
    if (s < 0 || s > 100 || !t[s]) return 0;
    return t[s] | ((s >= SDL_SCANCODE_INSERT && s <= SDL_SCANCODE_UP) || s == SDL_SCANCODE_KP_DIVIDE
                   || s == SDL_SCANCODE_KP_ENTER ? 0x100 : 0);
}

static void key(const SDL_KeyboardEvent *k)
{
    int up = k->type == SDL_KEYUP, s = k->keysym.scancode;
    if (s == SDL_SCANCODE_RETURN && (k->keysym.mod & KMOD_ALT)) { if (!up && !k->repeat) command(ID_FULL); return; }
    if (s == SDL_SCANCODE_PRINTSCREEN) { if (up) screenshot_key(); return; }
    if (s == SDL_SCANCODE_F7 || (s >= SDL_SCANCODE_F9 && s <= SDL_SCANCODE_F12)) {
        if (up || k->repeat) return;
        if (s == SDL_SCANCODE_F7) command(ID_WHOLE);
        if (s == SDL_SCANCODE_F9) command(ID_LOOK + !S.look);
        if (s == SDL_SCANCODE_F10) menu();
        if (s == SDL_SCANCODE_F11) command(ID_FULL);
        if (s == SDL_SCANCODE_F12) command(ID_MODE + (S.mode + 1) % PRESENT_MODES);
        return;
    }
    if (k->repeat) return;                          /* the game does its own repeat */
    int sc = pc_scancode(s);
    if (!sc) return;
    if (sc & 0x100) kbd_scancode(0xE0);
    kbd_scancode((uint8_t)((sc & 0xFF) | (up ? 0x80 : 0)));
}

static void window_open(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) fatal("SDL: %s", SDL_GetError());
    int x = win_rect.left, y = win_rect.top, w = win_rect.right - win_rect.left, h = win_rect.bottom - win_rect.top;
    if (g_opt.window || w <= 0) {                  /* --window N, or no place remembered */
        int s = g_opt.window ? g_opt.window : 3;
        x = y = SDL_WINDOWPOS_UNDEFINED; w = 320 * s; h = 240 * s;
    }
    wnd = SDL_CreateWindow("Epic Pinball", x, y, w, h, SDL_WINDOW_RESIZABLE);
    if (!wnd) fatal("SDL window: %s", SDL_GetError());
    remember_place();
    if (g_opt.fullscreen) set_fullscreen(1);
    if (!g_opt.mute && SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want = { 0 };
        want.freq = AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
        aud = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
        if (aud) SDL_PauseAudioDevice(aud, 0);
    }
}

/* Events, the pad, and the picture */
static void window_frame(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) quit_req = 1;
        else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) key(&e.key);
        else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_RIGHT) menu();
        else if (e.type == SDL_WINDOWEVENT) {
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) pad_load();   /* controller.ini may have been edited */
            if (e.window.event == SDL_WINDOWEVENT_MOVED || e.window.event == SDL_WINDOWEVENT_RESIZED) remember_place();
        }
    }
    if (SDL_GetWindowFlags(wnd) & SDL_WINDOW_INPUT_FOCUS) pad_poll();
    if (quit_req) dos_exit(0);
    compose_view();
    present_window(wnd, frame, fw, fh, frame_aspect, &S);
}

/* A queue the main thread fills, as long as waveOut's ring: drop rather than block */
static void window_audio(const int16_t *s, int frames)
{
    if (aud && SDL_GetQueuedAudioSize(aud) < 8 * 1024 * 4) SDL_QueueAudio(aud, s, (Uint32)frames * 4);
}

static void window_close(void)
{
    if (aud) SDL_CloseAudioDevice(aud);
    if (wnd) { settings_save(); SDL_DestroyWindow(wnd); }
    SDL_Quit();
}
#endif

void host_audio(const int16_t *s, int frames)
{
    if (rec_audio) { fwrite(s, 4, frames, rec_audio); rec_samples += frames; }
    if (wav_out) { fwrite(s, 4, frames, wav_out); wav_samples += frames; }
    if (wnd) window_audio(s, frames);
}

void host_init(void)
{
    keys_parse();
    if (g_opt.record) rec_open();
    if (g_opt.wav && (wav_out = fopen(g_opt.wav, "wb")) != NULL) wav_header(wav_out, 0);
    if (g_opt.headless) { settings_args(); return; }
    settings_load();
    settings_args();
    pad_load();
    window_open();
}

void host_frame(void)
{
    static uint64_t last_present;
    keys_due();
    uint64_t now = pit_now();
    shots_due(now);
    if (g_opt.seconds > 0 && now >= (uint64_t)(g_opt.seconds * PIT_HZ)) dos_exit(0);
    if (rec_video) {
        uint64_t per = (uint64_t)(PIT_HZ / REC_FPS);
        if ((rec_frames + 1) * per <= now) {
            compose();
            const uint32_t *f = as_rec();
            while ((rec_frames + 1) * per <= now) { fwrite(f, 4, (size_t)rec_w * rec_h, rec_video); rec_frames++; }
        }
    }
    if (!wnd) return;
    if (now - last_present < (uint64_t)(PIT_HZ / 70)) return;
    last_present = now;
    window_frame();
}

static void write_bmp(const char *path, const uint32_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t sz = 54 + w * h * 4, off = 54, hs = 40, zero = 0, img = w * h * 4;
    int32_t bw = w, bh = -h; uint16_t planes = 1, bpp = 32;
    fwrite("BM", 1, 2, f); fwrite(&sz, 4, 1, f); fwrite(&zero, 4, 1, f); fwrite(&off, 4, 1, f);
    fwrite(&hs, 4, 1, f); fwrite(&bw, 4, 1, f); fwrite(&bh, 4, 1, f); fwrite(&planes, 2, 1, f);
    fwrite(&bpp, 2, 1, f); fwrite(&zero, 4, 1, f); fwrite(&img, 4, 1, f);
    fwrite(&zero, 4, 1, f); fwrite(&zero, 4, 1, f); fwrite(&zero, 4, 1, f); fwrite(&zero, 4, 1, f);
    fwrite(px, 4, (size_t)w * h, f);
    fclose(f);
}

/* The picture as a BMP: the game's own frame, or with --present the
 * presenter's output at that size -- a run nobody watched still says where
 * it got to. */
void host_snapshot(const char *path)
{
    compose_view();
    if (g_opt.present_w > 0) {
        size_t n = (size_t)g_opt.present_w * g_opt.present_h;
        uint32_t *out = malloc(n * 4);
        if (out && present_offscreen(frame, fw, fh, frame_aspect, &S, g_opt.present_w, g_opt.present_h, out))
            write_bmp(path, out, g_opt.present_w, g_opt.present_h);
        else
            fprintf(stderr, "[present] %s: no presenter, not written\n", path);
        free(out);
        return;
    }
    write_bmp(path, frame, fw, fh);
}

void host_shutdown(void)
{
    rec_close();
    if (wav_out) { wav_header(wav_out, (uint32_t)wav_samples); fclose(wav_out); wav_out = NULL; }
    window_close();
}
