/*
 * pad.c - an Xbox (XInput) controller, mapped by controller.ini in the save
 * folder, as in the Gunman Chronicles recomp. Nothing in the game changes:
 * each pad input presses a key the game already reads. The file is written
 * with the defaults on first run -- GOG's DOSBox mapping (its controls.txt):
 * triggers flip, bumpers nudge, A plunges -- and read at start and whenever
 * the settings menu says so. The pad only acts while the window is in front.
 * On Linux the pad is SDL2's GameController, which names any pad's buttons
 * the Xbox way, so the same file maps it.
 */
#include "machine.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <SDL.h>
#endif

static const struct { const char *name; uint16_t bit; } k_buttons[] = {
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
static char ini[512];

int host_key_by_name(const char *s);  /* host.c */
const char *ini_get(const char *path, const char *sec, const char *key, const char *def, char *out, int n);

const char *pad_ini(void) { return ini; }

void pad_load(void)
{
    snprintf(ini, sizeof ini, "%s/controller.ini", g_opt.savedir);
    FILE *f = fopen(ini, "rb");
    if (f) fclose(f);
    else if ((f = fopen(ini, "wb")) != NULL) { fputs(k_default, f); fclose(f); }
    char v[32];
    for (int i = 0; i < IN_COUNT; i++) {
        ini_get(ini, "controller", i < NBUTTONS ? k_buttons[i].name : k_extra[i - NBUTTONS], "none", v, sizeof v);
        map[i] = _stricmp(v, "none") ? host_key_by_name(v) : 0;
    }
    trigger = atoi(ini_get(ini, "controller", "trigger", "64", v, sizeof v));
    stick = atoi(ini_get(ini, "controller", "stick", "12000", v, sizeof v));
}

/* XINPUT_STATE's layout */
typedef struct { uint32_t packet; uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; } XState;

#ifdef _WIN32
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XState *);

static int get_state(XState *s)
{
    static XInputGetState_t get;
    static int tried;
    if (!tried) {
        HMODULE m;
        tried = 1;
        m = LoadLibraryA("xinput1_4.dll");
        if (!m) m = LoadLibraryA("xinput9_1_0.dll");
        if (m) get = (XInputGetState_t)GetProcAddress(m, "XInputGetState");
    }
    return get && get(0, s) == ERROR_SUCCESS;
}
#else
/* The first pad SDL knows, read into XInput's shape */
static int get_state(XState *s)
{
    static SDL_GameController *gc;
    static const SDL_GameControllerButton b[NBUTTONS] = {     /* in k_buttons' order */
        SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
        SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
        SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y };
    if (gc && !SDL_GameControllerGetAttached(gc)) { SDL_GameControllerClose(gc); gc = NULL; }
    for (int i = 0; !gc && i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) gc = SDL_GameControllerOpen(i);
    if (!gc) return 0;
    s->buttons = 0;
    for (int i = 0; i < NBUTTONS; i++) if (SDL_GameControllerGetButton(gc, b[i])) s->buttons |= k_buttons[i].bit;
    s->lt = (uint8_t)(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    s->rt = (uint8_t)(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
    s->lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
    int y = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);   /* XInput's up is +, SDL's - */
    s->ly = (int16_t)(y > 32767 ? 32767 : y);
    return 1;
}
#endif

void pad_poll(void)
{
    static uint8_t held[256];        /* how many pad inputs hold each key down */
    static uint32_t was;
    XState s;

    if (!get_state(&s)) return;

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
