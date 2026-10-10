/*
 * pad.c - an Xbox (XInput) controller, mapped by controller.ini in the save
 * folder, as in the Gunman Chronicles recomp. Nothing in the game changes:
 * each pad input presses a key the game already reads. The file is written
 * with the defaults on first run -- GOG's DOSBox mapping (its controls.txt):
 * triggers flip, bumpers nudge, A plunges -- and read at start and whenever
 * the settings menu says so. The pad only acts while the window is in front.
 */
#include "machine.h"
#include <windows.h>

static const struct { const char *name; WORD bit; } k_buttons[] = {
    { "dpad_up", 0x0001 }, { "dpad_down", 0x0002 }, { "dpad_left", 0x0004 }, { "dpad_right", 0x0008 },
    { "start", 0x0010 }, { "back", 0x0020 }, { "left_stick", 0x0040 }, { "right_stick", 0x0080 },
    { "lb", 0x0100 }, { "rb", 0x0200 }, { "a", 0x1000 }, { "b", 0x2000 }, { "x", 0x4000 }, { "y", 0x8000 },
};
#define NBUTTONS (int)(sizeof k_buttons / sizeof k_buttons[0])
enum { IN_LT = NBUTTONS, IN_RT, IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT, IN_COUNT };
static const char *const k_extra[] = { "lt", "rt", "stick_up", "stick_down", "stick_left", "stick_right" };

static const char k_default[] =
"; Epic Pinball recompiled -- Xbox controller mapping.\r\n"
"; Each pad input presses a key. Keys: ESC ENTER SPACE UP DOWN LEFT RIGHT\r\n"
"; LSHIFT RSHIFT Z SLASH CTRL ALT TAB Y N F1 F2 F3 1-6, a scan code in hex,\r\n"
"; or none. Delete this file to get the defaults back.\r\n"
"[controller]\r\n"
"lt = LSHIFT\r\nrt = RSHIFT\r\n"
"lb = Z\r\nrb = SLASH\r\n"
"a = SPACE\r\nb = N\r\nx = ENTER\r\ny = Y\r\n"
"back = ESC\r\nstart = ESC\r\n"
"dpad_up = UP\r\ndpad_down = DOWN\r\ndpad_left = LEFT\r\ndpad_right = RIGHT\r\n"
"stick_up = UP\r\nstick_down = DOWN\r\nstick_left = LEFT\r\nstick_right = RIGHT\r\n"
"left_stick = none\r\nright_stick = none\r\n"
"; how far a trigger (0-255) and the stick (0-32767) go before they count\r\n"
"trigger = 64\r\nstick = 12000\r\n";

static int map[IN_COUNT];            /* scan code, | 0x100 for E0-prefixed; 0 = none */
static int trigger = 64, stick = 12000;
static char ini[MAX_PATH];

int host_key_by_name(const char *s);  /* host.c */

const char *pad_ini(void) { return ini; }

void pad_load(void)
{
    snprintf(ini, sizeof ini, "%s\\controller.ini", g_opt.savedir);
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) {
        FILE *f = fopen(ini, "wb");
        if (f) { fputs(k_default, f); fclose(f); }
    }
    char v[32];
    for (int i = 0; i < IN_COUNT; i++) {
        GetPrivateProfileStringA("controller", i < NBUTTONS ? k_buttons[i].name : k_extra[i - NBUTTONS],
                                 "none", v, sizeof v, ini);
        map[i] = _stricmp(v, "none") ? host_key_by_name(v) : 0;
    }
    trigger = GetPrivateProfileIntA("controller", "trigger", 64, ini);
    stick = GetPrivateProfileIntA("controller", "stick", 12000, ini);
}

typedef struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XState;
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XState *);

void pad_poll(void)
{
    static XInputGetState_t get;
    static int tried;
    static uint8_t held[256];        /* how many pad inputs hold each key down */
    static uint32_t was;
    XState s;

    if (!tried) {
        HMODULE m;
        tried = 1;
        m = LoadLibraryA("xinput1_4.dll");
        if (!m) m = LoadLibraryA("xinput9_1_0.dll");
        if (m) get = (XInputGetState_t)GetProcAddress(m, "XInputGetState");
    }
    if (!get || get(0, &s) != ERROR_SUCCESS) return;

    uint32_t now = 0;
    for (int i = 0; i < NBUTTONS; i++) if (s.buttons & k_buttons[i].bit) now |= 1u << i;
    if (s.lt > trigger) now |= 1u << IN_LT;
    if (s.rt > trigger) now |= 1u << IN_RT;
    if (s.ly >  stick) now |= 1u << IN_UP;
    if (s.ly < -stick) now |= 1u << IN_DOWN;
    if (s.lx < -stick) now |= 1u << IN_LEFT;
    if (s.lx >  stick) now |= 1u << IN_RIGHT;

    /* two inputs on one key (the d-pad and the stick) press it once */
    for (int i = 0; i < IN_COUNT; i++) {
        int sc = map[i];
        if (!sc || !((now ^ was) & (1u << i))) continue;
        int down = (now >> i) & 1, k = sc & 0xFF;
        if (down ? held[k]++ : --held[k]) continue;
        if (sc & 0x100) kbd_scancode(0xE0);
        kbd_scancode((uint8_t)(k | (down ? 0 : 0x80)));
    }
    was = now;
}
