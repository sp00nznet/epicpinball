/*
 * host.c - the modern machine's side: a Win32 window drawn by the presenter
 * (present.c, Direct3D 11; GDI if that fails), waveOut audio, keyboard as raw
 * PC scancodes, an Xbox pad (pad.c) -- or, with --headless, none of that:
 * frames are composed offscreen and, with --record, piped to ffmpeg with the
 * audio muxed in at the end. Headless never touches the desktop, so it works
 * over RDP and from CI (repo rules, section 13).
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

#ifdef _WIN32
void present_rect(int cw, int ch, int w, int h, double aspect, int mode, RECT *r);
int  present_window(HWND hw, const uint32_t *f, int w, int h, double aspect, const Present *s);
int  present_offscreen(const uint32_t *f, int w, int h, double aspect, const Present *s,
                       int ow, int oh, uint32_t *out);
#endif
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
    rec_video = _popen(cmd, "wb");
#endif
    if (!rec_video) fatal("cannot start ffmpeg (is it on PATH?)");
    rec_audio = fopen(rec_apath, "wb");
    if (rec_audio) wav_header(rec_audio, 0);
}

static void rec_close(void)
{
    if (!rec_video) return;
#ifdef _WIN32
    _pclose(rec_video);
#endif
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

#ifdef _WIN32
static char ini[MAX_PATH];
static HWND wnd;
static int is_full;
static Present saved;                /* what the ini says: the command line never reaches it */
static int saved_full;
static RECT win_rect;                /* the window's place when not fullscreen */

static void ini_put(const char *sec, const char *k, int v)
{
    char s[16]; snprintf(s, sizeof s, "%d", v);
    WritePrivateProfileStringA(sec, k, s, ini);
}

static void settings_save(void)
{
    if (!*ini) return;
    char v[64];
    WritePrivateProfileStringA("display", "scale", mode_names[saved.mode], ini);
    WritePrivateProfileStringA("display", "look", saved.look ? "vivid" : "natural", ini);
    WritePrivateProfileStringA("display", "bars", saved.bars ? "blur" : "black", ini);
    ini_put("display", "whole_table", saved.whole);
    ini_put("display", "fullscreen", saved_full);
    if (win_rect.right > win_rect.left) {
        snprintf(v, sizeof v, "%ld,%ld,%ld,%ld", win_rect.left, win_rect.top,
                 win_rect.right - win_rect.left, win_rect.bottom - win_rect.top);
        WritePrivateProfileStringA("display", "window", v, ini);
    }
    WritePrivateProfileStringA("sound", "output", g_audio_smooth ? "smooth" : "original", ini);
}

static void settings_load(void)
{
    char v[64];
    snprintf(ini, sizeof ini, "%s\\epicpinball.ini", g_opt.savedir);
    GetPrivateProfileStringA("display", "scale", "sharp", v, sizeof v, ini); S.mode = mode_by_name(v);
    GetPrivateProfileStringA("display", "look", "natural", v, sizeof v, ini); S.look = !_stricmp(v, "vivid");
    GetPrivateProfileStringA("display", "bars", "blur", v, sizeof v, ini); S.bars = _stricmp(v, "black") != 0;
    S.whole = GetPrivateProfileIntA("display", "whole_table", 0, ini);
    saved = S;
    saved_full = GetPrivateProfileIntA("display", "fullscreen", 0, ini);
    if (saved_full) g_opt.fullscreen = 1;
    GetPrivateProfileStringA("display", "window", "", v, sizeof v, ini);
    long x, y, w, h;
    if (sscanf(v, "%ld,%ld,%ld,%ld", &x, &y, &w, &h) == 4 && w > 100 && h > 100) {
        RECT r = { x, y, x + w, y + h };
        if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) win_rect = r;   /* still on a screen */
    }
    GetPrivateProfileStringA("sound", "output", "smooth", v, sizeof v, ini);
    g_audio_smooth = _stricmp(v, "original") != 0;
}
#endif

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

#ifdef _WIN32
static int quit_req;
static BITMAPINFO bmi;

/* PrtSc: what is on screen, to screenshots\epic-NNN.bmp */
static void screenshot_key(void)
{
    char p[256];
    CreateDirectoryA("screenshots", NULL);
    for (int n = 0; n < 1000; n++) {
        snprintf(p, sizeof p, "screenshots/epic-%03d.bmp", n);
        if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) { host_snapshot(p); return; }
    }
}

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
    else if (id == ID_PAD) ShellExecuteA(wnd, "open", pad_ini(), NULL, NULL, SW_SHOWNORMAL);
    else return;
    settings_save();
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
#endif

void host_audio(const int16_t *s, int frames)
{
    if (rec_audio) { fwrite(s, 4, frames, rec_audio); rec_samples += frames; }
    if (wav_out) { fwrite(s, 4, frames, wav_out); wav_samples += frames; }
#ifdef _WIN32
    if (wout) wave_push(s, frames);
#endif
}

void host_init(void)
{
    keys_parse();
    if (g_opt.record) rec_open();
    if (g_opt.wav && (wav_out = fopen(g_opt.wav, "wb")) != NULL) wav_header(wav_out, 0);
#ifdef _WIN32
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    if (g_opt.headless) { settings_args(); return; }
    settings_load();
    settings_args();
    pad_load();
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
#else
    settings_args();
#endif
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
#ifdef _WIN32
    if (!wnd) return;
    if (now - last_present < (uint64_t)(PIT_HZ / 70)) return;
    last_present = now;
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (GetForegroundWindow() == wnd) pad_poll();
    if (quit_req) dos_exit(0);
    compose_view();
    paint();
#endif
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
#ifdef _WIN32
    if (g_opt.present_w > 0) {
        size_t n = (size_t)g_opt.present_w * g_opt.present_h;
        uint32_t *out = malloc(n * 4);
        if (out && present_offscreen(frame, fw, fh, frame_aspect, &S, g_opt.present_w, g_opt.present_h, out))
            write_bmp(path, out, g_opt.present_w, g_opt.present_h);
        else
            fprintf(stderr, "[present] %s: no Direct3D 11, not written\n", path);
        free(out);
        return;
    }
#endif
    write_bmp(path, frame, fw, fh);
}

void host_shutdown(void)
{
    rec_close();
    if (wav_out) { wav_header(wav_out, (uint32_t)wav_samples); fclose(wav_out); wav_out = NULL; }
#ifdef _WIN32
    if (wout) { waveOutReset(wout); waveOutClose(wout); }
    if (wnd) { settings_save(); DestroyWindow(wnd); }
#endif
}
