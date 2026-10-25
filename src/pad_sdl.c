/* pad_sdl.c - pad.h on SDL2's game controller API, for the builds outside Windows (the Windows build has src/pad.c with its
 * own Sony HID and XInput code). SDL knows the DualSense / DualShock 4 (rumble and light bar over USB and Bluetooth), Xbox
 * pads and the Steam Deck's controls (through Steam Input), all with the same button names as pad.h. Every pad acts as one,
 * input and rumble pause while the window is not in front, as in pad.c. The on-screen pad of a touch screen (touch.c) is
 * merged in last, and a real pad in use hides it. */
#ifndef _WIN32
#include "pad.h"
#include "touch.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *penv(const char *n) { const char *v = getenv(n); return v && *v ? v : NULL; }
static double pad_clock(void) { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

/* ---- rumble: up to 8 running effects, the strongest one is what the motors get (as pad.c) ---------------------------- */
static struct { float s[8]; double until[8]; float scale; int log; } g_rum = { .scale = 1.0f, .log = -1 };
void pad_set_strength(float s) { g_rum.scale = s < 0 ? 0 : s > 1 ? 1 : s; }
void pad_rumble(float strength, float seconds)
{
    if (g_rum.log < 0) g_rum.log = penv("WOODY_PADLOG") != NULL;
    if (g_rum.log) printf("pad: rumble %.2f for %.2f s (x %.2f)\n", strength, seconds, g_rum.scale);
    double now = pad_clock(); int k = 0;
    for (int i = 1; i < 8; i++) if (g_rum.until[i] < g_rum.until[k]) k = i;
    g_rum.s[k] = strength; g_rum.until[k] = now + seconds;
}
static float rum_level(double now)
{
    float m = 0;
    for (int i = 0; i < 8; i++) if (now < g_rum.until[i] && g_rum.s[i] > m) m = g_rum.s[i];
    return m * g_rum.scale;
}

/* ---- the pads -------------------------------------------------------------------------------------------------------- */
#define NPADS 8
static struct {
    int inited, kind_last, vpad; unsigned seen; double vpad_t0; SDL_JoystickID vpad_id;
    struct { SDL_GameController *c; SDL_JoystickID id; int kind, sent; double sent_at; PadState st; } p[NPADS];
} P;

/* testing: WOODY_VPAD="T:LX:LY:BUTTONS[:D] ..." attaches an SDL virtual game controller and sets its left stick (-1..1) and
 * buttons (1 << SDL_CONTROLLER_BUTTON_*) for D s (default 0.08) from T s after the first poll - the whole SDL path, unlike
 * WOODY_PAD (main_engine.c), which fakes the merged state */
static void vpad_attach(void)
{
    P.vpad = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
    P.vpad_t0 = pad_clock(); if (P.vpad >= 0) P.vpad_id = SDL_JoystickGetDeviceInstanceID(P.vpad);
    printf("pad: virtual controller %s\n", P.vpad >= 0 ? "attached" : SDL_GetError());
}
static void vpad_drive(double now)
{
    SDL_Joystick *j = NULL;
    for (int i = 0; i < NPADS && !j; i++) if (P.p[i].c && P.p[i].id == P.vpad_id) j = SDL_GameControllerGetJoystick(P.p[i].c);
    if (!j) return;
    double t = now - P.vpad_t0; float lx = 0, ly = 0; unsigned m = 0;
    for (const char *s = penv("WOODY_VPAD"); s && *s; ) {
        double t0, d = 0.08; float x, y; unsigned b; int used;
        if (sscanf(s, " %lf:%f:%f:%i%n", &t0, &x, &y, (int *)&b, &used) != 4) break;
        s += used; if (*s == ':' && sscanf(s, ":%lf%n", &d, &used) == 1) s += used;
        if (t >= t0 && t < t0 + d) { lx = x; ly = y; m = b; }
    }
    SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_LEFTX, (Sint16)(lx * 32767)); SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_LEFTY, (Sint16)(ly * 32767));
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) SDL_JoystickSetVirtualButton(j, b, (Uint8)(m >> b & 1));
}

const char *pad_kind_name(int kind) { return kind == PADK_DS5 ? "DualSense" : kind == PADK_DS4 ? "DualShock 4" : kind == PADK_XBOX ? "Xbox controller" : kind == PADK_SWITCH ? "Switch controller" : "none"; }

static void open_new(void)
{
    for (int j = 0; j < SDL_NumJoysticks(); j++) {
        if (!SDL_IsGameController(j)) continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j); int have = 0, slot = -1;
        for (int i = 0; i < NPADS; i++) { if (P.p[i].c && P.p[i].id == id) have = 1; if (!P.p[i].c && slot < 0) slot = i; }
        if (have || slot < 0) continue;
        SDL_GameController *c = SDL_GameControllerOpen(j); if (!c) continue;
        int t = SDL_GameControllerGetType(c), kind = t == SDL_CONTROLLER_TYPE_PS5 ? PADK_DS5 : t == SDL_CONTROLLER_TYPE_PS4 ? PADK_DS4 : PADK_XBOX;
        if (t == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO) kind = PADK_SWITCH;   /* SDL goes by the labels: A = the right button */
#if SDL_VERSION_ATLEAST(2, 24, 0)
        if (t == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) kind = PADK_SWITCH;
#endif
#ifdef __SWITCH__
        kind = PADK_SWITCH;                                             /* the Joy-Cons, handheld mode, a Pro Controller */
#endif
        P.p[slot].c = c; P.p[slot].id = id; P.p[slot].kind = kind; P.p[slot].sent = -1; memset(&P.p[slot].st, 0, sizeof P.p[slot].st);
#ifdef __SWITCH__
        if (slot == 0) printf("pad: %s (SDL; every player slot is open, whether a controller is there or not)\n", pad_kind_name(kind));   /* SDL always lists 8 */
#else
        printf("pad: %s connected (\"%s\", SDL)\n", pad_kind_name(kind), SDL_GameControllerName(c));
#endif
        if ((kind == PADK_DS4 || kind == PADK_DS5) && !penv("WOODY_PADNOLIGHT")) {               /* Woody red, the middle player light (as pad.c) */
            SDL_GameControllerSetLED(c, 0xd0, 0x10, 0x08); SDL_GameControllerSetPlayerIndex(c, 0);
        }
    }
}
static void close_gone(void)
{
    for (int i = 0; i < NPADS; i++) if (P.p[i].c && !SDL_GameControllerGetAttached(P.p[i].c)) {
        printf("pad: %s disconnected\n", pad_kind_name(P.p[i].kind));
        SDL_GameControllerClose(P.p[i].c); memset(&P.p[i], 0, sizeof P.p[i]);
    }
}
static float ax(SDL_GameController *c, SDL_GameControllerAxis a) { int v = SDL_GameControllerGetAxis(c, a); return v < 0 ? v / 32768.0f : v / 32767.0f; }
static void read_pad(int i)
{
    static const struct { int sdl, b; } map[] = {
        { SDL_CONTROLLER_BUTTON_A, PAD_A }, { SDL_CONTROLLER_BUTTON_B, PAD_B }, { SDL_CONTROLLER_BUTTON_X, PAD_X }, { SDL_CONTROLLER_BUTTON_Y, PAD_Y },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PAD_LB }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PAD_RB }, { SDL_CONTROLLER_BUTTON_BACK, PAD_BACK },
        { SDL_CONTROLLER_BUTTON_START, PAD_START }, { SDL_CONTROLLER_BUTTON_LEFTSTICK, PAD_LS }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, PAD_RS },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, PAD_UP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, PAD_DOWN }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, PAD_LEFT },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PAD_RIGHT }, { SDL_CONTROLLER_BUTTON_GUIDE, PAD_GUIDE }, { SDL_CONTROLLER_BUTTON_TOUCHPAD, PAD_TOUCH } };
    SDL_GameController *c = P.p[i].c; PadState *st = &P.p[i].st; uint32_t m = 0;
    for (unsigned k = 0; k < sizeof map / sizeof map[0]; k++) if (SDL_GameControllerGetButton(c, (SDL_GameControllerButton)map[k].sdl)) m |= 1u << map[k].b;
    st->lx = ax(c, SDL_CONTROLLER_AXIS_LEFTX); st->ly = ax(c, SDL_CONTROLLER_AXIS_LEFTY);       /* SDL: y down = +, as pad.h */
    st->rx = ax(c, SDL_CONTROLLER_AXIS_RIGHTX); st->ry = ax(c, SDL_CONTROLLER_AXIS_RIGHTY);
    st->lt = ax(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT); st->rt = ax(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    if (st->lt >= 0.25f) m |= 1u << PAD_LT;
    if (st->rt >= 0.25f) m |= 1u << PAD_RT;
    st->kind = P.p[i].kind;
    if (m != st->buttons) { st->buttons = m; if (m) P.kind_last = P.p[i].kind; }
}
static void rumble(int i, int v, double now)        /* SDL rumble effects run for a duration: send changes, and refresh a running one */
{
    if (v == P.p[i].sent && (v == 0 || now - P.p[i].sent_at < 0.5)) return;
    Uint16 a = (Uint16)(v * 257);
    if (SDL_GameControllerRumble(P.p[i].c, a, a, v ? 1000 : 0) == 0 || v == 0) { P.p[i].sent = v; P.p[i].sent_at = now; }
}
void pad_close(void)
{
    for (int i = 0; i < NPADS; i++) if (P.p[i].c) { SDL_GameControllerRumble(P.p[i].c, 0, 0, 0); SDL_GameControllerClose(P.p[i].c); }
    memset(P.p, 0, sizeof P.p);
}
void pad_poll(PadState *st, int focused, unsigned devchanges)
{
    memset(st, 0, sizeof *st);
    if (!P.inited) {
        P.inited = 1;
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1"); SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");   /* rumble over Bluetooth too */
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "1");
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)) { printf("pad: SDL game controllers unavailable: %s\n", SDL_GetError()); P.inited = 2; return; }
        P.vpad = -1; if (penv("WOODY_VPAD")) vpad_attach();
        atexit(pad_close); open_new(); P.seen = devchanges;
    }
    if (P.inited == 2) { touch_pad(st, 0); return; }
    if (devchanges != P.seen) { P.seen = devchanges; close_gone(); open_new(); }
    double now = pad_clock();
    if (P.vpad >= 0) vpad_drive(now);
    SDL_GameControllerUpdate(); int lvl = focused ? (int)(rum_level(now) * 255.0f + 0.5f) : 0;
    float best_l = 0, best_r = 0; int any = 0, kinds = 0;
    for (int i = 0; i < NPADS; i++) {
        if (!P.p[i].c) continue;
        rumble(i, lvl, now);
        if (!any) any = P.p[i].kind;
        kinds |= 1 << P.p[i].kind;
        if (!focused) continue;
        read_pad(i); const PadState *p = &P.p[i].st;
        st->buttons |= p->buttons;
        float ml = p->lx * p->lx + p->ly * p->ly, mr = p->rx * p->rx + p->ry * p->ry;
        if (ml > best_l) { best_l = ml; st->lx = p->lx; st->ly = p->ly; }
        if (mr > best_r) { best_r = mr; st->rx = p->rx; st->ry = p->ry; }
        if (p->lt > st->lt) st->lt = p->lt;
        if (p->rt > st->rt) st->rt = p->rt;
    }
    if (!(kinds >> P.kind_last & 1)) P.kind_last = any;      /* the pad used last went away */
    st->kind = P.kind_last;
    touch_pad(st, st->buttons || st->lx * st->lx + st->ly * st->ly > 0.25f);
}
#endif
