/* storm.c - instance class 80, the lightning rod, and the thunderstorm (docs/STORM.md).
 *
 * Class 80 (size 0x118, ctor 0x451a90, vtable 0x4ab0c4) is a FadeInst with four fields: +0x108 radius (default 1000),
 * +0x10c height (default 400), +0x110 glow, +0x114 "Woody is in my zone". Init 0x451b10 gives it type word 0x27
 * (category 7, which the camera's line of sight ignores). Its zone is a SPHERE of that radius around the middle of the
 * rod, pos + (0, height/2, 0) (0x451be0); the top of the rod is pos + (0, height, 0).
 *
 * The storm is not an instance: three globals (0x5e59ec on, 0x5e59e4 timer, 0x5e59e8 interval) run by 0x451cc0 at the
 * start of the Game tick. Message 1100 starts it (timer 1 s), 1101 or Woody's death stops it. Every `interval` seconds a
 * bolt comes down from 2000 above Woody: outside every zone it hits him (Kill 9) at his chest (+180), inside a zone it
 * hits the top of that rod and arcs crawl along the rod. 1.7 s before each strike a 2D thunder rumble (SoundFx 8) warns,
 * and the screen darkens for 2.5 s up to the strike, then flashes 2..3 times (0x46e030 / 0x46e0d0).
 *
 * WOODY_STORMLOG=1 logs start/stop, the warning, every strike and the rod glow per second. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storm.h"
#include "player.h"
#include "hud.h"
#include "audio.h"
#include "crtrand.h"

#define MAXZ 64                                             /* 0x451aad: list 0x5e58cc[0x40] */
typedef struct { Instance *inst; float r, h, glow; int inside; uint32_t upd; } Zone;   /* +0x108, +0x10c, +0x110, +0x114 */
static Zone g_z[MAXZ]; static int g_nz;
static struct { int on; float timer, interval; } S;         /* 0x5e59ec, 0x5e59e4, 0x5e59e8 */
/* the sky flash 0x46e030 / 0x46e0d0: 0x5e827c on, 0x5e8280 t (counts down), 0x5e8284 t0, 0x4b7a20 flashes, 0x4b7a24 k,
 * 0x4b7a2c rem (of the current flash), 0x4b7a30 dur, 0x4b7a28 intensity, 0x5e8288 the colour drawn */
static struct { int on, n, k; float t, t0, rem, dur, inten; uint32_t col; } F;
typedef struct { int on, dirty, n; float age, life, acc; Vec3 a, b, p[41]; int kind[41]; } Bolt;   /* particle 0x46def0 -> 0x46d520 */
typedef struct { int on, dirty, phase; float age, life, delay; Vec3 p1, p2, p3, loc[14]; } Arc;   /* particle 0x46df80 -> 0x46da00 */
static Bolt g_bolt[16]; static Arc g_arc[40];
static float g_T;                                           /* [0x5e85d0]: += dt, modulo 2 s (0x46d040) */
static int g_log = -1;
static int g_actor_next;
static uint32_t g_upd;                                             /* logic frames that ran storm_update (the rods' glow steps once per such frame) */
static float g_rod_dt;

static int log_on(void) { if (g_log < 0) g_log = wenv("WOODY_STORMLOG") != NULL; return g_log; }
static Vec3 v3(float x, float y, float z) { return (Vec3){ x, y, z }; }
static Vec3 vadd(Vec3 a, Vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static Vec3 vsub(Vec3 a, Vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static Vec3 vmul(Vec3 a, float k) { return v3(a.x * k, a.y * k, a.z * k); }
static float vlen(Vec3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
static Vec3 vnorm(Vec3 a) { float l = vlen(a); return l > 0 ? vmul(a, 1.0f / l) : a; }
static Vec3 vcross(Vec3 a, Vec3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static int ftoi_round(float f) { return (int)floorf(f + 0.5f); }   /* fistp in the default rounding mode */
static Zone *zone_of(const Instance *in) { for (int i = 0; i < g_nz; i++) if (g_z[i].inst == in) return &g_z[i]; return NULL; }

/* 0x46d320: an orthonormal frame around the direction w; u, v span the plane across it */
static void basis(Vec3 w, Vec3 *u, Vec3 *v)
{
    if (fabsf(w.x) < 0.001f && fabsf(w.z) < 0.001f) { *v = vnorm(v3(0, w.z, -w.y)); *u = vcross(*v, w); }
    else { *u = vnorm(v3(w.z, 0, -w.x)); *v = vcross(w, *u); }
}

void storm_reset(void)
{
    memset(&S, 0, sizeof S); memset(&F, 0, sizeof F); g_nz = 0;
    memset(g_bolt, 0, sizeof g_bolt); memset(g_arc, 0, sizeof g_arc); g_actor_next = 0;
}
void storm_add(Instance *in)
{
    if (!in || zone_of(in) || g_nz >= MAXZ) return;
    Zone *z = &g_z[g_nz++]; z->inst = in; z->r = 1000.0f; z->h = 400.0f; z->glow = 0; z->inside = 0;   /* Init 0x451b10 */
    if (log_on()) printf("STORM rod %u at %.0f %.0f %.0f\n", in->index, in->position.x, in->position.y, in->position.z);
}
void storm_zone_param(Instance *in, int mode, int v)
{
    Zone *z = zone_of(in); if (!z) return;
    if (mode == 1) z->r = (float)v; else if (mode == 2) z->h = (float)v;   /* fild: raw, no x0.01 */
    if (log_on()) printf("STORM rod %u: radius %.0f height %.0f\n", in->index, z->r, z->h);
}

/* 0x46e030(t, first): the flash cycle restarts; t is the time until the next strike */
static void flash_new(void)
{
    float d = crt_rnd() * 0.1f; if (d < 0.05f) d -= 0.1f; d += 0.3f;   /* 0.2..0.25 or 0.35..0.4 s */
    F.rem = F.dur = d; F.inten = crt_rnd() * 40.0f;
}
static void flash_start(float t, int first)
{
    if (first) { F.t = 2.5f; F.t0 = t; } else F.t = F.t0 = t;
    F.n = 2 - (int)(crt_rnd() * -2.0f);                               /* _ftol truncates: 2 or 3 flashes */
    F.k = 1; flash_new(); F.on = 1;
}
static void flash_colour(void)                                    /* 0x46e2a0 */
{
    float d = F.dur, r = F.rem, grey, a;
    if (d * 0.6666667f < r) { float v = d - r; grey = v * 384.0f / d; a = v * F.inten * 3.0f / d; }   /* rising third */
    else if (d * 0.3333333f < r) { grey = 128.0f; a = F.inten; }                                    /* plateau */
    else { grey = r * 384.0f / d; a = F.inten * r * 3.0f / d; }                                     /* falling third */
    uint32_t g = (uint32_t)ftoi_round(grey) & 255, al = (uint32_t)ftoi_round(a + 160.0f) & 255;
    F.col = al << 24 | g << 16 | g << 8 | g;
}
void storm_overlay_draw(int paused, float dt)                      /* 0x46e0d0, RectVirtual 640x480, flag 8 (alpha blend) */
{
    if (!F.on) return;
    if (!paused) {
        float x = F.t0 - 1.8f;
        if (F.t < x) {                                              /* between the flashes: darken over the last 2.5 s */
            float a = 0;
            if (F.t < 2.5f) a = (2.5f - F.t) * 200.0f * 0.4f; else if (F.t < 0) F.on = 0;   /* (the second test can never hold) */
            F.col = ((uint32_t)ftoi_round(a) & 255) << 24;
        } else if (F.k < F.n) {                                     /* the first 1.8 s: flashes back to back ... */
            flash_colour(); F.rem -= dt;
            if (F.rem <= 0) { flash_new(); F.k++; }
        } else if (F.rem > 0 && x + F.rem + 0.5f > F.t) { flash_colour(); F.rem -= dt; }   /* ... and the last one at the end */
        else F.col = 0;
        F.t -= dt;
    }
    if (F.col >> 24 >= 2) hud_rect(F.col);
}

/* 0x451be0: the first rod whose sphere (radius +0x108 around pos + (0, h/2, 0)) holds p; *top = the top of that rod */
static Zone *zone_at(Vec3 p, Vec3 *top)
{
    for (int i = 0; i < g_nz; i++) {
        Zone *z = &g_z[i]; Vec3 c = z->inst->position;
        Vec3 d = vsub(p, v3(c.x, c.y + z->h * 0.5f, c.z));
        if (d.x * d.x + d.y * d.y + d.z * d.z < z->r * z->r) { *top = v3(c.x, c.y + z->h, c.z); return z; }
    }
    return NULL;
}

static void bolt_spawn(Vec3 a, Vec3 b)                             /* 0x46def0: life 1.8 s */
{
    for (int i = 0; i < 16; i++) if (!g_bolt[i].on) { Bolt *o = &g_bolt[i]; memset(o, 0, sizeof *o); o->on = 1; o->life = 1.8f; o->a = a; o->b = b; o->dirty = 1; return; }
}
static void arc_spawn(Vec3 p1, Vec3 p2, Vec3 p3, float delay)       /* 0x46df80: life 0.75 s after `delay` */
{
    for (int i = 0; i < 40; i++) if (!g_arc[i].on) { Arc *o = &g_arc[i]; memset(o, 0, sizeof *o); o->on = 1; o->life = 0.75f; o->p1 = p1; o->p2 = p2; o->p3 = p3; o->delay = delay; o->dirty = 1; return; }
}

void storm_start(float interval)                                   /* 0x451ba0 */
{
    S.interval = interval; S.timer = 1.0f; S.on = 1;
    flash_start(interval, 1);
    if (log_on()) printf("STORM start: interval %.2f s, first strike in 1 s\n", interval);
}
void storm_stop(void)                                              /* 0x451bd0 */
{
    if (S.on && log_on()) printf("STORM stop\n");
    F.on = 0; S.on = 0;                                             /* 0x46e3a0: the flash overlay off */
}

/* actor list 1 (0x4c52d8, count 0x4c531c) is a COPY: the actors append themselves to 0x4c5218 (0x40c080, 8 entries) during
 * their updates, and the app frame 0x401ab0 copies that list over at its very start (0x40bf60 at 0x401bd9) and empties it.
 * So the storm, first thing in the Game tick, sees who registered during the PREVIOUS frame. The Perso registers in its
 * update 0x44b530 when not frozen (+0x690, 0x44b68b), not dead (+0x26c = the death kind, 0 = alive: written by Kill 0x44c453 /
 * 0x44c6d3, cleared by Reset 0x44ab54) and not in state 5 (+0x21c, 0x44b6a3). The port's player_update runs before
 * storm_update, so the flag taken here is the one of this frame's Perso update, and it is used on the next frame. */
void storm_update(float dt, Player *pl, int frozen)
{
    g_T = fmodf(g_T + dt, 2.0f); g_upd++; g_rod_dt = dt;
    int actor = g_actor_next && pl && pl->inst;                    /* registered during the previous frame */
    g_actor_next = pl && pl->inst && !frozen && !pl->dead_kind && !pl->script_act;
    if (S.on) {                                                    /* 0x451cc0 */
        int above = S.timer > 1.7f;
        S.timer -= dt;
        if (!(S.timer > 1.7f) && above) { audio_fx(8, NULL, NULL); if (log_on()) printf("STORM warning (thunder), strike in %.2f s\n", S.timer); }
        Vec3 top;
        if (actor) { Zone *z = zone_at(pl->pos, &top); if (z) z->inside = 1; }
        if (S.timer <= 0) {
            S.timer += S.interval;
            flash_start(S.timer, 0);
            if (actor) {
                Vec3 pos = pl->pos; Zone *z = zone_at(pos, &top);
                if (!z) {
                    top = v3(pos.x, pos.y + 180.0f, pos.z);
                    player_kill(pl, 9);                             /* vt[38](9): the lightning death */
                    audio_fx(7, pl->inst, &pl->inst->position.x);   /* 0x451e53: the Perso's own position; the voice keeps the pointer (not the local copy) */
                }
                Vec3 sky = v3(pos.x, pos.y + 2000.0f, pos.z);
                bolt_spawn(sky, top); bolt_spawn(sky, top);
                if (z) {
                    Vec3 c = z->inst->position; float span = z->h - 80.0f;
                    audio_fx(7, z->inst, &z->inst->position.x);     /* 0x451ebb: the rod's position, live like every 3D voice's */
                    for (int i = 0; i < 3; i++) {                   /* three arcs between random points of the rod, 0.2..1.2 s later */
                        Vec3 A = v3(c.x, crt_rnd() * span + c.y + 80.0f, c.z), B = v3(c.x, crt_rnd() * span + c.y + 80.0f, c.z), C = v3(c.x, crt_rnd() * span + c.y + 80.0f, c.z);
                        arc_spawn(B, C, A, crt_rnd() + 0.2f);
                    }
                    Vec3 lo = v3(c.x, c.y + 80.0f, c.z), hi = v3(c.x, c.y + z->h, c.z);
                    arc_spawn(hi, hi, lo, 0); arc_spawn(lo, lo, hi, 0.4f);   /* down the rod at once, back up after 0.4 s */
                }
                if (log_on()) printf("STORM strike at %.0f %.0f %.0f: %s\n", pos.x, pos.y, pos.z, z ? "sheltered by a rod" : "HIT (Kill 9)");
            } else if (log_on()) printf("STORM strike (no actor)\n");
        }
    }
    if (log_on() && g_nz && (int)(g_T * 1) != (int)((g_T - dt) * 1) && S.on) {
        for (int i = 0; i < g_nz; i++) if (g_z[i].glow > 0) printf("STORM rod %u glow %.2f%s\n", g_z[i].inst->index, g_z[i].glow, g_z[i].inst->drawn ? "" : " (not drawn: held)");
    }
}
/* vt[26] 0x452010, the render colour of a rod, called by the renderer for each rod it DRAWS: an added grey pulsing 3..253
 * with a 2 s period; while Woody is in its zone the red goes out of it within 1 s (and comes back as slowly), leaving a
 * cyan glow. The glow and the inside flag +0x114 only move here, so a rod off screen keeps both until it is drawn again
 * (its flag set by 0x451cc0 stays up meanwhile). dt = the frame's (once per logic frame; nothing moves while paused). */
void storm_rod_drawn(Instance *in)
{
    if (in->type != 80) return;
    Zone *z = zone_of(in); if (!z) return;
    if (z->upd != g_upd) {                                         /* the logic frame whose dt it already took */
        z->upd = g_upd; float dt = g_rod_dt;
        if (z->inside) { z->glow += dt; if (z->glow > 1.0f) z->glow = 1.0f; z->inside = 0; }
        else if (z->glow > 0) z->glow -= dt;
    }
    int k = ftoi_round(g_T * 0.5f * 512.0f) & 0x1ff;
    float pulse = cosf(k * 0.012271846f) * 250.0f * 0.5f + 128.0f;
    float rgb[3] = { pulse, pulse, pulse };
    if (z->glow > 0) { float g = z->glow > 1.0f ? 1.0f : z->glow; rgb[0] = (1.0f - g) * pulse; rgb[1] = pulse * 0.84313726f; }
    in->tint_mode = 2;                                             /* [0x5ac850] = 2: added to the lit colour */
    for (int q = 0; q < 3; q++) in->tint_rgb[q] = rgb[q] / 255.0f;
}

static void bolt_draw(Bolt *o, const float *eye, float dt)          /* 0x46d520 */
{
    o->age += dt; o->acc += dt;
    if (o->age / o->life >= 1.0f) { o->on = 0; return; }
    Vec3 d = vsub(o->b, o->a); float len = vlen(d);
    int n = (int)(len * 0.0066666668f); float step;
    if (n > 40) { n = 40; step = 0.025f; } else step = n > 0 ? 1.0f / n : 0;
    if (o->acc > o->life * 0.1f) { o->acc -= o->life * 0.1f; o->dirty = 1; }   /* a new zigzag every 0.18 s */
    if (o->dirty) {
        Vec3 w = vnorm(d), u, v; basis(w, &u, &v);
        o->p[0] = o->a;
        for (int i = 0; i < n; i++) {
            Vec3 p;
            if (i == n - 1) p = o->b;
            else {
                float ox = crt_rnd() * 180.0f - 90.0f, oy = crt_rnd() * 180.0f - 90.0f;
                p = vadd(vadd(o->a, vmul(w, (float)(i + 1) * step * len)), vadd(vmul(u, ox), vmul(v, oy)));
            }
            o->p[i + 1] = p; o->kind[i + 1] = (int)(crt_rnd() * 4.0f);
        }
        o->n = n; o->dirty = 0;
    }
    float alpha = (crt_rnd() + 1.0f) * 0.5f;                          /* one flicker value per frame */
    static const float white[3] = { 1, 1, 1 }, glow[3] = { 0.8f, 0.8f, 1.0f };
    for (int i = 0; i < o->n; i++) {
        /* line 0x471a10 flags 0xe00: bank 0 image 30 (0x1001e), half width 30, (1,1,1,alpha) at both ends, additive;
         * the texture mirrored by the stored kind (0x470d80(kind, 1): the LINE's vertex set; 1 = v, 2 = u, 3 = both, 4 (rnd = 1.0)
         * turned), which stays set for every later line until the next bolt segment (hud.c k_uvset) */
        hud_world_streak_flip(30, &o->p[i].x, &o->p[i + 1].x, eye, 30.0f, white, alpha, alpha, o->kind[i + 1]);
        /* and at the struck point, once per segment: sprite 0x470f10(3) bank 0 image 6, colour (.8,.8,1) alpha .7, size 25..45.
         * S+0x260 = 0x12 is the SHAPE of the quad: the index into the corner-angle table [0x5e823c]+0x800 (0x470f1e..0x470f24,
         * filled at 0x4024bb: atan(2^(0x12/8 - 0x12%8)) = 45 deg), i.e. the plain square every other sprite uses; flags 3 =
         * camera facing + own colour, no flag 8 = additive ONE/ONE (0x4719c0). So the glow is drawn exactly like this. */
        hud_world_spr_mode(0x12, 6, &o->b.x, crt_rnd() * 20.0f + 25.0f, 0, glow, 0.7f, 3, NULL, 0);
    }
}
static void arc_draw(Arc *o, const float *eye, float dt)            /* 0x46da00 */
{
    o->age += dt;
    if (o->age < o->delay) return;
    float u = (o->age - o->delay) / o->life;
    if (u >= 1.0f) { o->on = 0; return; }
    int ph = u <= 0.25f ? 1 : u <= 0.5f ? 2 : u <= 0.75f ? 3 : 4;  /* a new shape each quarter of its life */
    if (ph != o->phase) { o->phase = ph; o->dirty = 1; }
    if (o->dirty) {
        float ox0 = crt_rnd() * 100.0f - 50.0f, oy0 = crt_rnd() * 100.0f - 50.0f;
        for (int i = 0; i < 14; i++) {
            if (i == 0) { o->loc[i] = v3(0, 0, 0); continue; }
            if (i == 13) { o->loc[i] = v3(0, 0, 1); continue; }
            float t = i * 0.07692308f; int m = (int)(t * -256.0f);   /* cos[(128 - m) & 511] = -sin(pi t) */
            float c = cosf((float)((128 - m) & 0x1ff) * 0.012271846f);
            float x = crt_rnd() * 10.0f - ox0 * c - 5.0f, y = crt_rnd() * 10.0f - oy0 * c - 5.0f;
            o->loc[i] = v3(x, y, t);
        }
        o->dirty = 0;
    }
    Vec3 P = vadd(o->p2, vmul(vsub(o->p3, o->p2), u));             /* the far end slides from p2 to p3 */
    Vec3 D = vsub(P, o->p1); float L = vlen(D);
    float alpha = (crt_rnd() + 1.0f) * 0.5f;
    if (L <= 0) return;
    Vec3 w = vmul(D, 1.0f / L), bu, bv; basis(w, &bu, &bv);
    static const float col[3] = { 0.55f, 0.85f, 0.92f };
    Vec3 prev = o->p1;
    for (int i = 1; i < 14; i++) {                                   /* line flags 0xe00, bank 0 image 0, half width 3, additive */
        Vec3 l = o->loc[i], p = vadd(o->p1, vadd(vadd(vmul(bu, l.x), vmul(bv, l.y)), vmul(w, l.z * L)));
        hud_world_streak(0, &prev.x, &p.x, eye, 3.0f, col, alpha, alpha);
        prev = p;
    }
}
void storm_fx_draw(const float *eye, float dt)
{
    for (int i = 0; i < 16; i++) if (g_bolt[i].on) bolt_draw(&g_bolt[i], eye, dt);
    for (int i = 0; i < 40; i++) if (g_arc[i].on) arc_draw(&g_arc[i], eye, dt);
}
