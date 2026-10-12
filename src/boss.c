/* boss.c - enemy class 14, Buzz Buzzard (docs/BOSS14.md): the end boss of W1B (mode 1, his flying machine) and of
 * W2D / W3D / WWS (mode 2, hopping). ctor 0x40eb50, vtable 0x4a98a8, Update 0x40eec0.
 * The second half of the file: classes 15 and 16, Buzz's crusher fight (W2D, W3D) and his pad fight (W3D), docs/BOSS15_16.md.
 *
 * The class does nothing until the level script writes 1 or 2 into its mailbox variable (message 60); every update it
 * answers -1, and 3 once it is beaten. Mode 1: it chases the player HIGH (at its placement height, invulnerable), shakes
 * 0.25 s over him and drops with gravity (a cone under it takes a heart), then hovers LOW (floor + 290) where a hit costs
 * it exactly 1 of its 5 hp; after a hit, or after it touched the player, it goes up again. The coupled instance
 * (message 59, W1B 404 = its machine) gets its position, rotation and animation record every frame.
 *
 * Simplified: no actor avoidance; the obstacle sensor runs (enemy.c, docs/OBSTACLE.md 3) but with P+0x2c/0x30 = 15000 it
 * never reports a direction blocked, so it only quantises the wander directions to its 16 slots; mode 2 (W2D/W3D/WWS) is ported with the same
 * state machine (its dust is the smoke ring 0x476140(pos - 50 up, up, 0, 1.5, 6.0), docs/PARTICLES.md 3) and it is not verified in those levels. */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include "enemy.h"
#include "player.h"
#include "audio.h"
#include "crtrand.h"

#define PI_F 3.14159265f

/* AnimCtrl records 0x4b1958 (34 x 0x1c bytes, dumped from the exe; all priority 1000, restart 1) */
typedef struct { int sub[4]; float speed; } BRec;
static const BRec g_br[34] = {
    { { 5, 5, 5, 5 }, 3 }, { { 6, 7, 7, 7 }, 3 }, { { 7, 7, 7, 7 }, 3 }, { { 8, 5, -1, -1 }, 3 }, { { 9, 10, 10, 10 }, 3 },
    { { 11, 5, -1, -1 }, 3 }, { { 11, 5, -1, -1 }, 3 }, { { 13, 13, 13, 13 }, 2 }, { { 14, 5, -1, -1 }, 1 }, { { 18, -1, -1, -1 }, 1 },
    { { 15, 5, -1, -1 }, 3 }, { { 16, 5, -1, -1 }, 3 }, { { 17, 5, -1, -1 }, 3 }, { { 15, 5, -1, -1 }, 3 }, { { 17, 5, -1, -1 }, 3 },
    { { 12, 12, 12, 12 }, 3 }, { { 16, 17, 15, 16 }, 1.5f },
    { { 19, 19, 19, 19 }, 3 }, { { 20, 21, 21, 21 }, 3 }, { { 21, 21, 21, 21 }, 3 }, { { 22, 19, -1, -1 }, 3 }, { { 23, 24, 24, 24 }, 3 },
    { { 25, 19, -1, -1 }, 3 }, { { 25, 19, -1, -1 }, 3 }, { { 27, 27, 27, 27 }, 2 }, { { 28, 19, -1, -1 }, 1 }, { { 32, -1, -1, -1 }, 1 },
    { { 29, 19, -1, -1 }, 3 }, { { 30, 19, -1, -1 }, 3 }, { { 31, 19, -1, -1 }, 3 }, { { 29, 19, -1, -1 }, 3 }, { { 31, 19, -1, -1 }, 3 },
    { { 25, 26, 26, 26 }, 3 }, { { 30, 31, 29, 30 }, 1.5f },
};
/* AnimLen(n) = duration(sub[0]) / speed, always of the BOSS model (AnimCtrl+0x4c) */
static float anim_len(const Enemy *e, int n)
{
    const Model *m = e->inst->model; int s = g_br[n].sub[0];
    return (uint32_t)s < m->nanims ? m->anims[s].duration_s / g_br[n].speed : 0;
}
/* Request 0x436b70 + Tick 0x436a50 on one instance: a chain of up to four animations, the last one loops, -1 = hold */
static void rec_tick(Instance *in, int n, int *cur, int *sub)
{
    const Model *m = in->model; const BRec *r = &g_br[n];
    if (n != *cur) { *cur = n; *sub = 0; if ((uint32_t)r->sub[0] < m->nanims) { in->anim = r->sub[0]; in->anim_time = 0; } }
    if ((uint32_t)in->anim >= m->nanims) return;
    in->anim_speed = r->speed;
    int nx = *sub < 3 ? r->sub[*sub + 1] : 0; float L = m->anims[in->anim].duration_s;
    if (*sub < 3 && (nx < 0 || (uint32_t)nx >= m->nanims)) {       /* hold the last frame: the clock advances after this, so stop it just before the end */
        if (in->anim_time >= L - 0.15f) { in->anim_time = L * 0.999f; in->anim_speed = 0; }
    } else if (*sub < 3 && in->anim_time >= L) { in->anim_time -= L; in->anim = nx; (*sub)++; }
}

static float ang_to(Vec3 a, Vec3 b) { return atan2f(b.z - a.z, b.x - a.x); }   /* direction = (cos, 0, sin), like the port's other enemies */
static float ang_diff(float a, float b) { float d = a - b; while (d > PI_F) d -= 2 * PI_F; while (d < -PI_F) d += 2 * PI_F; return d; }
static float dist_xz(Vec3 a, Vec3 b) { float x = a.x - b.x, z = a.z - b.z; return sqrtf(x * x + z * z); }
static float dist3(Vec3 a, Vec3 b) { float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z; return sqrtf(x * x + y * y + z * z); }

/* ---- the base parts of Enemy (docs/ENEMY.md 3-5) with the P values of subtype 11 (BOSS14.md 2.3) ---------------- */
#define B_TURN_W    (PI_F / 4)   /* P+0x10 wander */
#define B_TURN_C    PI_F         /* P+0x14 chase, state 5 */
#define B_SEE       4600.0f      /* P+0x20 (3D) */
#define B_DY        3000.0f      /* P+0x24 */
#define B_HIGH_T    1.0f         /* P+0x38 */
#define B_BITE      1.0f         /* P+0x3c */
#define B_VSPEED    900.0f       /* P+0x78 */
#define B_CONE_H    250.0f       /* P+0x7c */
#define B_CONE_R    150.0f       /* P+0x80 */
#define B_LOW       290.0f       /* P+0x88 */
#define B_LAND_T    0.3f         /* P+0x9c */
#define B_SHAKE_T   0.25f        /* P+0xa4 */
#define B_SHAKE     20.0f        /* P+0xa8 */
#define B_ACC       1000.0f      /* H+0x28 */
#define B_PL_R      69.0f        /* the Perso's vtbl[32] / [33] */
#define B_PL_H      193.0f

static int m1(const Enemy *e) { return e->b.mode == 1; }

/* FindTarget vtbl[48](1, 0) = 0x41af80: the player within 4600 (3D) and |dy| < 3000 */
static int find_target(const Enemy *e, const Player *pl)
{
    if (pl->dead_kind) return 0;
    return dist3(e->pos, pl->pos) < B_SEE && fabsf(pl->pos.y - e->pos.y) < B_DY;
}

/* vtbl[53] 0x4105f0: how long an action / state lasts = AnimLen of the record vtbl[45] would pick for it */
static int wander_rec(const Enemy *e, int a)
{
    int k = m1(e) ? 0 : 17;
    if (a == 0 || a == 3) return k ? 27 : 10; if (a == 1 || a == 4) return k ? 28 : 11; if (a == 2) return k ? 29 : 12;
    if (a == 5 || a == 6 || a == 7 || a == 9 || a == 10) return k ? 19 : 2;
    return -1;
}
static float state_len(const Enemy *e)
{
    int k = m1(e) ? 0 : 17, n = -1;
    switch (e->b.st) {
    case 0: case 1: case 4: n = wander_rec(e, e->b.w_act); break;
    case 2: case 3: n = 2 + k; break;  case 5: n = 16 + k; break;  case 6: case 7: n = 4 + k; break;  case 8: n = 15 + k; break;
    case 9: n = 8 + k; break;          case 10: case 11: n = 7 + k; break;  case 12: n = 9 + k; break;  case 13: n = 6 + k; break;
    }
    return n < 0 ? 0 : anim_len(e, n);
}

/* ---- behaviours (ENEMY.md 5): 0 Dwalen (wander with a leash around home), 1 Achtervolgen (chase), 2 Stilstaan ---- */
static void wander_choose(Enemy *e)                                  /* 0x41c180(-1): walking and idling alternate */
{
    BossState *b = &e->b; int a = b->w_act > 5 ? crt_mod(5) : 6 + crt_mod(2);   /* 0x41c1dc: one draw, rand() % the summed weights */
    b->w_act = a; int n = wander_rec(e, a); b->w_t = n < 0 ? 0 : anim_len(e, n);
    if (a == 7) { float r = enemy_sensor_random_free(e); e->want_ang = r >= 0 ? r : (float)crt_mod(6283) * 0.001f; }   /* 0x41c249: a free sensor direction, else 0x41ba10 */
}
/* Every caller of these two (Reset 0x40eda7, states 0 0x40eff6, 2 0x40f0af and 3 0x40f3c6) first calls the behaviour's vtbl[6]
 * (Wander 0x41c090, Chase 0x41bc60), and both begin with 0x41b2b0, which zeroes the behaviour's knockback timer Behav+0x1c
 * (and +0x20..+0x28). Without that a peck's knock timer on the Chase (2.4 s in W2D, AnimLen(25)) survives state 9: the next
 * chase spends it with a zero direction and a step > 0, so the stuck hook 0x41bdf0 wriggles him +-16 every frame, and a
 * peck while it still runs costs no hp (Enemy_TakeDamage refuses it). */
static void wander_start(Enemy *e) { e->b.knock[0] = 0; e->b.behav = 0; e->b.turn = B_TURN_W; e->b.w_act = -1; wander_choose(e); }   /* vtbl[6] 0x41c090 + 0x41c160 */
static void chase_start(Enemy *e, const Player *pl)                  /* vtbl[6] 0x41bc60 + 0x41bc80: run speed at once, turn pi, first turn on the spot */
{
    BossState *b = &e->b; b->knock[1] = 0; b->behav = 1; b->turn = B_TURN_C; e->speed = e->want_speed = e->P.run;
    e->want_ang = ang_to(e->pos, pl->pos);                           /* Steer: the first target */
    float a = fabsf(ang_diff(ang_to(e->pos, pl->pos), e->ang));
    if (a > 1e-4f) { b->c_turn_t = a / b->turn; b->c_run = 0; } else b->c_run = 1;
}
static void h_turn(Enemy *e, float dt)                               /* H tick 0x41ba90 */
{
    float d = ang_diff(e->want_ang, e->ang), s = e->b.turn * dt;
    if (fabsf(d) <= s) e->ang = e->want_ang; else e->ang += d > 0 ? s : -s;
}
static void h_speed(Enemy *e, float dt)
{
    if (e->speed < e->want_speed) { e->speed += B_ACC * dt; if (e->speed > e->want_speed) e->speed = e->want_speed; }
    else if (e->speed > e->want_speed) { e->speed -= B_ACC * dt; if (e->speed < e->want_speed) e->speed = e->want_speed; }
}
/* The common move 0x41b2c0 with the sweep 0x437580 is enemy_common_move (enemy.c, docs/ENEMY.md 5.1): a SPHERE of radius P+4
 * (240) whose centre sits r + up + 1 above the feet (up = min(P+0x30, h/2) = 75, so 316 over pos: it spans pos.y + 76 .. pos.y
 * + 556), in substeps of at most 30, pushed out of the world polygons and the instance press nodes (0x407340, xz only) and lifted
 * so the feet never end below GetHeight. This is what keeps Buzz out of the four lanterns of the W1B arena (model 6, press nodes
 * up to y 1972) and away from the rock walls behind them: in the low phase (pos.y 1635) the sphere meets the lantern heads, in
 * the high phase (2430) it passes over them but meets the walls, so he never gets within the 150 (xz) he needs to stomp a
 * player who hides in the corner behind a lantern (docs/BOSS14.md 5.1). Subtype 11 keeps y (res.y = from.y) and has
 * P+0x2c/0x30 = 15000, so only the floor normal of the free test 0x41b514..0x41b54a can refuse a step: then he stays (plus the
 * platform delta) and the behaviour's OnBlocked vtbl[3] runs: Wander 0x41c420 turns to the widest free sensor direction
 * (action 5), Chase 0x41be90 to the free direction nearest its angle with the turn timer 0. Over the W1B arena floor the test
 * always passes; it refuses steps over steep slopes. The Chase hook 0x41bdf0 (stuck => wriggle -16..15) is in there too. */
static void behav_move(Enemy *e, Player *pl, float step, float dt)
{
    BossState *b = &e->b;
    /* the sweep runs EVERY frame, also with a zero step (Stilstaan, the shake, the stomp fall): 0x437580 then does one
     * substep in place, so the sphere keeps pushing him off a lantern head while he drops next to it. A peck knocks with
     * dir 0: the knock timer of the active behaviour only freezes the step */
    if (step < 0) step = 0;
    Vec3 d = { cosf(e->ang) * step, 0, sinf(e->ang) * step };
    if (enemy_common_move(e, pl, d, &b->knock[b->behav], (Vec3){ 0, 0, 0 }, b->behav, dt)) return;
    if (b->behav == 0) {                                             /* Wander OnBlocked 0x41c420 */
        float a = enemy_sensor_widest_free(e); if (a >= 0) e->want_ang = a;
        b->w_act = 5; int n = wander_rec(e, 5); b->w_t = n < 0 ? 0 : anim_len(e, n);
    } else if (b->behav == 1) { e->want_ang = enemy_sensor_nearest_free(e, e->ang); b->c_turn_t = 0; }   /* Chase OnBlocked 0x41be90 */
    if (wenv("WOODY_BOSSLOG")) printf("  boss blocked at %.0f %.0f %.0f (behav %d)", e->pos.x, e->pos.y, e->pos.z, b->behav), puts("");
}
static void behav_tick(Enemy *e, Player *pl, float dt)
{
    BossState *b = &e->b;
    if (b->behav == 0 || b->behav == 1) enemy_sensor_tick(e, pl);  /* Enemy::Update 0x41a3e0: Wander (kind 0) / Chase (1) */
    switch (b->behav) {
    case 0: {                                                        /* Dwalen 0x41c640 */
        if ((b->w_t -= dt) <= 0) { if (b->w_act == 10) { b->w_act = 9; b->w_t = anim_len(e, wander_rec(e, 9)); } else wander_choose(e); }
        if (dist3(e->pos, e->home) > e->P.leash) {                   /* 0x41c500: back home, turn first (action 10), then walk (9) */
            e->want_ang = ang_to(e->pos, e->home);
            if (b->w_act != 9 && b->w_act != 10) { b->w_act = 10; b->w_t = fabsf(ang_diff(e->want_ang, e->ang)) / b->turn; }
        }
        int mv = b->w_act == 6 || b->w_act == 7 || b->w_act == 9 || b->w_act == 10;
        e->want_speed = mv ? e->P.walk : 0; h_speed(e, dt);
        if (b->w_act > 4) h_turn(e, dt);
        behav_move(e, pl, mv ? dt * e->speed : 0, dt);
        break; }
    case 1:                                                          /* Achtervolgen 0x41bee0 */
        h_turn(e, dt); h_speed(e, dt);                               /* Steer 0x41bd00: H.Tick toward the last target, then the new one */
        { float a = ang_to(e->pos, pl->pos); e->want_ang = enemy_sensor_free(e, a) ? a : enemy_sensor_nearest_free(e, a); }
        if (b->c_turn_t <= 0 && !b->c_run) b->c_run = 1; else b->c_turn_t -= dt;
        behav_move(e, pl, b->c_run ? dt * e->speed : 0, dt);
        break;
    default: behav_move(e, pl, 0, dt); break;                         /* Stilstaan: only the knockback timer */
    }
}

/* vtbl[43] 0x410900: hover height, or the fall (flag 4) with the feet k below pos */
static void height_tick(Enemy *e, Player *pl, float dt)
{
    BossState *b = &e->b; float h2 = e->P.height * 0.5f, gy0; int f0;
    game_msgmask(e->inst, 0x200, enemy_probe(e, pl, (Vec3){ e->pos.x, e->pos.y + h2, e->pos.z }, h2, &gy0, &f0));   /* 0x410973: the probe at pos + h/2 (Press/In/UnPress), 0x410993 / 0x4109ab */
    if (b->grav) {
        float k = m1(e) ? 200.0f : 130.0f, feet = e->pos.y - k; int found;
        float gy; enemy_probe(e, pl, (Vec3){ e->pos.x, feet + h2, e->pos.z }, h2, &gy, &found);   /* 0x41a561 again, with the same probe at the feet */
        int on = found && gy >= feet - 0.5f;
        if (on) b->fall_v = 0; else { b->fall_v += dt * b->fall_g - b->fall_v * 0.2f; if (b->fall_v > 0) feet -= b->fall_v; }   /* 0x41a4e0 -> 0x440330 / 0x440340: v in units per FRAME, g = the Fall object's own +4 (200) */
        if (found && feet < gy) { feet = gy; on = 1; }
        b->on_ground = on; e->pos.y = feet + k; game_msgmask(e->inst, 0x200, on); return;   /* 0x41a642 / 0x41a65e */
    }
    b->on_ground = 0;
    if (!m1(e) && !b->high && b->st != 9) {                          /* mode 2 hops in the low phase */
        float s = dt * e->P.run;
        if (b->bob_down) { b->bob += s; if (b->bob >= b->bob_max) { game_smoke_ring((Vec3){ e->pos.x, e->pos.y - 50, e->pos.z }, (Vec3){ 0, 1, 0 }, 0, 1.5f, 6.0f); audio_fx(49, NULL, NULL); b->bob = b->bob_max; b->bob_down = 0; } }
        else { b->bob -= s; if (b->bob <= 0) { b->bob = 0; b->bob_down = 1; } }
    } else b->bob = 0;
    if (b->high) e->home.y = b->y_high;
    float d = e->home.y - e->pos.y - b->bob;
    if (fabsf(d) > 0.01f) { float s = dt * B_VSPEED; if (fabsf(d) > s) d = d < 0 ? -s : s; e->pos.y += d; }
}

/* vtbl[31] 0x410cf0: the player inside the cone that hangs point down under pos (250 high, 150 wide at the top) */
static int cone_touch(const Enemy *e, const Player *pl)
{
    Vec3 A = pl->pos, S = e->pos; float bot = S.y - B_CONE_H;
    float ph = player_body_height(pl);                                    /* vtbl[33]: 61 while ducked */
    if (A.y + ph < bot || S.y < A.y) return 0;
    float o = A.y - bot + ph, r = B_PL_R + (o < B_CONE_H ? o * B_CONE_R / B_CONE_H : B_CONE_R);
    return (A.x - S.x) * (A.x - S.x) + (A.z - S.z) * (A.z - S.z) <= r * r;
}
/* the player takes the bite; 1 = he died of it (the caller of the Perso's vtbl[39] kills him, like the other enemies) */
static int bite(Enemy *e, Player *pl)
{
    float dx = pl->pos.x - e->pos.x, dz = pl->pos.z - e->pos.z, l = sqrtf(dx * dx + dz * dz); Vec3 d = l > 1e-3f ? (Vec3){ dx / l, 0, dz / l } : (Vec3){ 0, 0, 0 };
    if (player_hit(pl, B_BITE, d)) { player_kill(pl, 3); return 1; }
    return 0;
}

/* vtbl[45] 0x410170: the record for the state of the PREVIOUS frame (it runs inside Enemy::Update, before the switch) */
static void anim_choose(Enemy *e, float dt)
{
    BossState *b = &e->b; int k = m1(e) ? 0 : 17, n = -1; (void)dt;
    switch (b->st) {
    case 0: case 1: case 4: n = (b->w_act == 8 || b->w_act < 0) ? -1 : wander_rec(e, b->w_act); break;
    case 2: case 3: n = 2 + k; break;  case 5: n = 16 + k; break;  case 6: case 7: n = 4 + k; break;  case 8: n = 15 + k; break;
    case 9: n = 8 + k; break;          case 10: case 11: n = 7 + k; break;  case 12: n = 9 + k; break;  case 13: n = 6 + k; break;
    }
    if (n < 0) n = b->rec >= 0 ? b->rec : 2 + k;                     /* no request: the controller keeps the running one */
    rec_tick(e->inst, n, &b->rec, &b->sub);
    if (b->link) rec_tick(b->link, n, &b->lrec, &b->lsub);          /* the same record number, on the link's own model */
    if (b->st != 10 && b->st != 11 && b->st != 12) {                 /* 0x468e40 / 0x468e50: the 2D loop, started on the first active frame */
        b->active = 1; if (!b->loop_on) { audio_fx(m1(e) ? 39 : 44, NULL, NULL); b->loop_on = 1; }
    }
}
static void loop_stop(Enemy *e) { if (e->b.loop_on) { audio_fx_stop(39, NULL, 0); audio_fx_stop(44, NULL, 0); e->b.loop_on = 0; } }   /* 0x468e20 */

/* vtbl[55] = 0x41b1b0 because vtbl[54] returns the link: it takes the boss's position and rotation */
static void sync_link(Enemy *e)
{
    Instance *l = e->b.link; if (!l) return;
    l->position = e->inst->position; l->quat = e->inst->quat; l->scale = e->inst->scale; l->world = e->inst->world;
}

/* Reset is reached from the factory, from every mode switch of the mailbox and from message 11 [inst, 4] (W2D object 745,
 * W3D 775: when Woody loses a life in the arena, docs/BOSS14.md 11.1; W1B and WWS never send it).Enemy::Reset 0x41a010 puts pos and the home point
 * back on the START position +0x128 (not on the instance position, which is pos itself in the original and the last placed
 * pos in the port), snaps the H angle to the start angle +0x1c (0x41b840), clears the Fall object (0x440310: v and its clock,
 * not its gravity), hp = P+0x34 and the hit timer +0x158. It touches no behaviour: only the Wander's knock timer is cleared,
 * by its vtbl[6] below; the Chase's is cleared by the next chase start, the Stand's runs out with the hit timer. */
void boss_reset(Enemy *e)                                            /* vtbl[17] 0x40ed90 (Enemy::Reset 0x41a010 first) */
{
    BossState *b = &e->b; Instance *in = e->inst;
    e->pos = e->home = e->start; e->ang = e->want_ang = e->start_ang;   /* 0x41a02f..0x41a067, 0x41b840 */
    if (in->position.x != e->pos.x || in->position.y != e->pos.y || in->position.z != e->pos.z) {   /* +0xc IS the instance position in the original */
        in->position = e->pos; mat4_from_trs(&in->world, in->position, in->quat, in->scale);
    }
    e->hp = wenv("WOODY_BOSSHP") ? (float)atof(wenv("WOODY_BOSSHP")) : e->P.hp; e->hit_t = 0;   /* WOODY_BOSSHP: testing */ in->visible = 1;   /* 0x407790: back in the world */
    enemy_reset_probe(e); game_msgmask(in, 0x10, 0);                /* Enemy::Reset 0x41a010: the probe 0x41a148, msgmask 0x10 cleared 0x41a167 */
    wander_start(e); b->grav = 0; b->st = 0; b->t1d8 = 0; b->high = 1; b->acc = 0;
    b->bob_down = 1; b->bob_max = 150.0f; b->bob = 0; b->fall_v = 0;
    b->rec = b->lrec = -1; b->sub = b->lsub = 0;
    game_boss_smoke(b->link, -1, 0);                                 /* bytes 0x5e857c..e */
    /* 0x40ee49 / 0x40ee7d also write P+0 = 400 / 800, but the Fall object took its gravity from P+0 (200) once, in PostLoad
     * (0x419eb2 -> ctor 0x4402f0), and nothing reads P+0 after that: the drop keeps g = 200 (b->fall_g) */
    if (b->mode == 1) { e->P.run = 900; e->P.walk = 400; e->P.fall_g = 400; }
    else if (b->mode == 2) { e->P.run = 400; e->P.walk = 390; e->P.fall_g = 800; }
    loop_stop(e);                                                    /* 0x468e10: a new source */
    if (wenv("WOODY_BOSSLOG")) printf("  BOSS %u reset: pos %.0f %.0f %.0f ang %.3f hp %.0f mode %d", in->index, e->pos.x, e->pos.y, e->pos.z, e->ang, e->hp, b->mode), puts("");
}

void boss_init(Enemy *e)
{
    BossState *b = &e->b; Instance *in = e->inst;
    e->P.radius = 240; e->P.height = 150; e->P.walk = 600; e->P.run = 900; e->P.fall_g = 200; e->P.leash = 1000; e->P.see = B_SEE; e->P.dy = B_DY;
    e->P.hp = 5; e->P.active_d = 3000; e->attackable = 1; e->P.drop = e->P.rise = 15000.0f;   /* P+0x2c / P+0x30: no edge or step test */
    /* PostLoad 0x419ec1: the start angle from the placement (vtbl[44] builds the same quaternion as enemy_place) */
    { Quat q = in->quat; float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w); if (l > 1e-6f) { q.x /= l; q.y /= l; } float yaw = 2.0f * atan2f(-q.y, q.x); e->ang = e->want_ang = e->start_ang = PI_F * 0.5f - yaw; }   /* also H+0x1c, the start angle Reset snaps back to */
    e->start = in->position;                                         /* +0x128 (0x419f74) */
    b->fall_g = e->P.fall_g;                                         /* the Fall object +0x110 (0x419eb2): g = P+0 = 200, kept for good */
    b->y_high = in->position.y; b->mode = 0; b->mail_var = 0; b->link = NULL;
    boss_reset(e);
    in->visible = 1;                                                 /* the factory hangs it in the world; the W1B script hides it 1 s later */
}

int boss_take_damage(Enemy *e, const Vec3 *pt, int kind)             /* vtbl[39] 0x40fe90 */
{
    BossState *b = &e->b;
    if (b->mode == 0 || b->high || e->hit_t > 0) return 0;          /* high phase or still "hit": invulnerable */
    b->st = 9; e->hit_t = anim_len(e, m1(e) ? 8 : 25);               /* vtbl[53]: AnimLen(8) = 2.1 s in W1B */
    audio_fx(m1(e) ? 42 : 47, NULL, NULL);
    if (m1(e)) switch ((int)e->hp) {                                 /* hp BEFORE the hit, table 0x410054 */
        case 1: game_boss_smoke(b->link, -1, 0); break;             /* the last hit: the smoke stops */
        case 2: game_boss_smoke(b->link, 0, 1); break;
        case 3: game_boss_smoke(b->link, 1, 1); break;
        case 4: game_boss_smoke(b->link, 2, 1); break;
        default: break;
    } else game_explosion((Vec3){ e->pos.x, e->pos.y + 400, e->pos.z });
    /* Enemy_TakeDamage 0x41adc0 with 1.0 whatever the attacker says: a knockback still running on the active behaviour refuses the hit */
    if (b->knock[b->behav] > 0) return 0;
    b->knock[b->behav] = state_len(e);
    if (pt && kind != 2) game_hit_star(*pt);                         /* 0x41adea: star 0x40c2d0 -> 0x4750e0 at pt; none for the special attack (2) or a blast (pt NULL, 0x41aeec) */
    e->hp -= 1.0f;
    printf("  BOSS %u hit, hp %.0f", e->inst->index, e->hp), puts("");
    return e->hp <= 0;
}

void boss_update(Enemy *e, Player *pl, Vec3 cam, float dt)
{
    BossState *b = &e->b; Instance *in = e->inst;
    if (!b->y_low_ok) {                                              /* PostLoad: the ground under the placement + 290 (0x41a2a0) */
        int found; float gy = player_ground_query(pl, in, (Vec3){ in->position.x, in->position.y + e->P.height * 0.5f, in->position.z }, &found);
        b->y_low = (found ? gy : in->position.y) + B_LOW; b->y_low_ok = 1;
    }
    if (b->mode == 0) { if (b->rec >= 0) { rec_tick(in, b->rec, &b->rec, &b->sub); if (b->link) rec_tick(b->link, b->lrec, &b->lrec, &b->lsub); } }   /* beaten: the last record plays out */
    /* Think 0x41a320: only for an instance of this frame's list world+0x64 (0x42b400), and only within 3000 of the camera (or dead) */
    if (!in->visible || !game_enemy_thinks(in) || (dist3(e->pos, cam) >= e->P.active_d && e->hp > 0)) return;
    /* mailbox 0x410be0, first thing in every update */
    { int v = game_var_get(b->mail_var);
      if ((v == 1 || v == 2) && b->mode != v) { b->mode = v; boss_reset(e); printf("  BOSS %u: mode %d", in->index, v), puts(""); }
      game_var_set(b->mail_var, -1); }
    if (b->mode == 0) return;
    /* Enemy::Update 0x41a3e0: behaviour, height, rotation, animation, the link */
    behav_tick(e, pl, dt);
    height_tick(e, pl, dt);
    enemy_place(e);
    anim_choose(e, dt);
    if (b->t1d4 >= 0) b->t1d4 -= dt; if (b->t1d8 >= 0) b->t1d8 -= dt; if (b->t1e4 >= 0) b->t1e4 -= dt;
    int t = find_target(e, pl), k = m1(e) ? 0 : 1;
    switch (b->st) {
    case 0:                                                          /* to the hover height, wander 0x40efb5 */
        e->home.y = b->high ? b->y_high : b->y_low; b->grav = 0;
        wander_start(e); b->t1e4 = b->high ? B_HIGH_T : 0; b->st = 1; break;
    case 1: if (b->t1e4 < 0) b->st = 2; break;
    case 2:                                                          /* notice 0x40f06b */
        if (!t) { b->st = 0; break; }
        b->grav = 0; chase_start(e, pl); b->pl_prev = (Vec3){ 0, 0, 0 }; b->st = 3; break;
    case 3: {                                                        /* chase 0x40f0e9 */
        if (b->high && e->home.y - e->pos.y >= 20.0f) break;         /* first (nearly) up at the high hover height */
        if (!t) { b->st = 0; break; }
        b->pl_prev = b->pl_cur; b->pl_cur = pl->pos;
        if (!b->high) {
            if (cone_touch(e, pl)) {
                if (bite(e, pl)) { b->st = 10; break; }
                audio_fx(k ? 46 : 41, NULL, NULL); b->high = 1; b->st = 0; break;
            }
            if (dist_xz(e->pos, b->pl_cur) < 150.0f) {               /* under / next to him without touching: back off 1300 from the player */
                Vec3 a = { e->home.x - pl->pos.x, e->home.y - pl->pos.y, e->home.z - pl->pos.z }; float L = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); if (L < 1e-3f) L = 1e-3f;
                b->home_save = e->home;
                e->home = (Vec3){ pl->pos.x + a.x / L * 1300.0f, b->y_low, pl->pos.z + a.z / L * 1300.0f };
                wander_start(e); b->leash_save = e->P.leash; e->P.leash = 1.0f; b->st = 4; break;
            }
        }
        if (dist_xz(e->pos, b->pl_cur) < 150.0f) { b->behav = 2; b->shake_i = 0; b->t1d4 = B_SHAKE_T; if (b->high) b->st = 6; }
        break; }
    case 4: case 5: {                                                /* back off 0x40f47d / hang there facing the player 0x40f558 */
        int still = 0;
        if (t) { b->pl_prev = b->pl_cur; b->pl_cur = pl->pos; still = dist3(b->pl_prev, b->pl_cur) < 1.0f; }   /* 0x410c60 */
        if (!t || !still) { e->P.leash = b->leash_save; e->home = b->home_save; b->st = 2; break; }   /* restore 0x40f66f */
        if (b->st == 4) { if (dist3(e->pos, e->home) < 150.0f) b->st = 5; }
        else { b->behav = 2; b->turn = B_TURN_C; e->want_ang = ang_to(e->pos, pl->pos); h_turn(e, dt); }
        break; }
    case 6: {                                                        /* shake over the player 0x40f6bb */
        if (!t) { b->st = 2; break; }
        if (b->t1d4 <= 0) { b->st = 7; b->grav = 1; b->fall_v = 0; break; }   /* gravity on: the drop */
        b->acc += dt; if (b->acc <= 1.0f / 60) break;                /* a fixed 60 Hz step, at most one per frame */
        Vec3 d = { cosf(e->ang), 0, sinf(e->ang) }; int n = b->shake_i++; float s = n == 0 ? -B_SHAKE : n == 3 ? -B_SHAKE : B_SHAKE;
        if (n == 3) b->shake_i = 0;
        Vec3 p = { e->pos.x + d.x * s, e->pos.y, e->pos.z + d.z * s };
        if (b->shake_i & 1) { p.x += (pl->pos.x - p.x) * 0.1f; p.z += (pl->pos.z - p.z) * 0.1f; }   /* 10 % toward the player */
        e->pos = p; while (b->acc > 1.0f / 60) b->acc -= 1.0f / 60;
        break; }
    case 7:                                                          /* the drop 0x40f909 */
        if (!t) { b->st = 0; break; }
        if (b->on_ground) {
            game_cam_shake(1.5f); b->high = 0; b->t1d8 = B_LAND_T; b->st = 8;
            if (!m1(e)) game_smoke_ring((Vec3){ e->pos.x, e->pos.y - 50, e->pos.z }, (Vec3){ 0, 1, 0 }, 0, 1.5f, 6.0f);
            audio_fx(k ? 45 : 40, NULL, NULL);
        }
        e->want_ang = ang_to(e->pos, pl->pos); b->turn = B_TURN_C; h_turn(e, dt);
        { float k = 1.0f - powf(1.0f - 0.01f, dt * 60.0f);           /* 0x4a94f8: 1 % per FRAME, normalised to 60 Hz so the pull does not grow with our frame rate */
          e->pos.x += (pl->pos.x - e->pos.x) * k; e->pos.z += (pl->pos.z - e->pos.z) * k; }
        if (cone_touch(e, pl)) {
            if (bite(e, pl)) { b->st = 10; break; }
            audio_fx(k ? 46 : 41, NULL, NULL); game_cam_shake(1.5f); b->high = 0; b->t1d8 = B_LAND_T; b->st = 8;
        }
        break;
    case 8: if (b->t1d8 < 0) b->st = 0; break;                       /* after the landing: the low phase begins */
    case 9:                                                          /* hit 0x40fbd1 */
        if (e->hp <= 0) { b->st = 12; break; }
        e->hit_t -= dt; b->behav = 2;
        if (e->hit_t < 0) { b->high = 1; b->st = 0; }
        break;
    case 10:                                                         /* the player is beaten 0x40fcb1 */
        audio_fx(k ? 48 : 43, NULL, NULL); loop_stop(e);
        b->t1e0 = 4.0f; b->st = 11; b->behav = 2; e->want_speed = e->P.walk;
        /* fallthrough */
    case 11: if ((b->t1e0 -= dt) <= 0) { b->high = 1; b->st = 0; } break;
    case 12:                                                         /* beaten 0x40fc27 */
        b->behav = 2; b->grav = 1; if (e->hit_t >= 0) e->hit_t -= dt;
        game_var_set(b->mail_var, 3);                                /* the script: "boss beaten" */
        loop_stop(e); b->mode = 0;
        printf("  BOSS %u beaten: var %u := 3", in->index, b->mail_var & 0xffffff), puts("");
        break;
    }
    if (wenv("WOODY_BOSSHIT") && b->mode && !b->high && b->st == 3 && e->hit_t <= 0 && dist_xz(e->pos, pl->pos) < 400.0f)   /* testing: a peck as soon as he is low and near */
        printf("  BOSSHIT test peck: %s", boss_take_damage(e, NULL, 0) ? "beaten" : "done"), puts("");
    sync_link(e);
    game_boss_bar(1, (int)e->hp, (int)e->P.hp);                      /* 0x40fd82 */
    if (wenv("WOODY_BOSSLOG")) printf("  boss st %d high %d mode %d pos %.0f %.0f %.0f hp %.0f rec %d anim %d behav %d act %d knock %.2f %.2f %.2f t %.3f", b->st, b->high, b->mode, e->pos.x, e->pos.y, e->pos.z, e->hp, b->rec, in->anim, b->behav, b->w_act, b->knock[0], b->knock[1], b->knock[2], game_time()), puts("");
}

/* every frame, updated or not: the render colour hook vtbl[26] 0x40fde0 (also the link's, through its owner) and the
 * sound source that stops the loop on the first frame without an update */
void boss_frame_end(Enemy *e)
{
    BossState *b = &e->b;
    if (!b->active) loop_stop(e);
    b->active = 0;
    int red = 0;
    if (b->st == 9 || b->st == 12) {                                 /* the counter goes up once per drawn model: 8 red, 8 normal */
        int nd = 1 + (b->link != NULL); b->blink = (b->blink + nd) % 16; red = b->blink >= 1 && b->blink <= 8;
    }
    e->inst->tint_red = red; if (b->link) b->link->tint_red = red;
}

void enemies_boss_msg(EnemySet *s, Instance *inst, int id, uint32_t arg, Instance *linked)
{
    Enemy *e = NULL; for (int i = 0; i < s->n; i++) if (s->e[i].inst == inst && s->e[i].type >= 14 && s->e[i].type <= 16) e = &s->e[i];
    if (!e) return;
    if (e->type != 14) { if (id == 60) e->bb.mail_var = arg & 0xffffff; return; }   /* class 15 0x40e7e3 (+0x24c), class 16 0x40d7aa (+0x294) */
    if (id == 60) { e->b.mail_var = arg & 0xffffff; return; }        /* 0x4100b0 */
    if (id == 59 && linked) {                                        /* 0x4100d2: one instance, its own AnimCtrl, owner = the boss */
        e->b.link = linked; linked->scripted = 0; e->b.lrec = -1; e->b.lsub = 0;
        player_set_carried(inst, linked);                            /* it moves with the boss: never the boss's floor */
    }
}

/* ==== classes 15 and 16: Buzz's second and third fights (docs/BOSS15_16.md) ==========================================
 * Both are the Buzz model (43 animations: class 14 mode 2 plays 19..32, class 15 33..37, class 16 38..42) and both
 * stand still (only the behaviour Stilstaan, flag 4 = ground following). The fight is in the instances the script links.
 *
 * Class 15 ("Boss2" in its debug string; W2D 762, W3D 790; ctor 0x40d850, vtable 0x4a9778, Update 0x40dd30): message 61
 * links four crushers and four launchers (type 42, bomb throwers). PostLoad puts the centre C 1300 in front of him;
 * Reset hangs the crushers 1000 above the corners (+-750, +-750) of a square around C and the launchers on a ring of 300
 * around C, facing out. Command 4 in his mailbox (message 60) starts a 3 s spin of the crushers, then a 14 s cycle over
 * and over: 2 s wait, then four crushers one after the other (pattern 0x4b1898) slam down in 0.3 s and rise in 1.0 s;
 * with the first slam of a cycle the launcher in line with it throws one bomb (after the first hit: all four). A crusher
 * costs the player a heart within 200 of its origin and sets off every bomb whose fuse is still in state 2 there. He
 * cannot be pecked (vtbl[39] is `return 0`); only a bomb blast (radius 400 against his cylinder, or a bomb flying into
 * him) takes 1 of his 6 hp. After the first hit the slams are 0.2 / 0.9 s, from 3 hp on 0.2 / 0.7 s. At 0 hp: mailbox
 * := 3 every frame, death animation, every bomb in play discarded.
 *
 * Class 16 (W3D 801; ctor 0x40c730, vtable 0x4a9658, Update 0x40cb80): message 62 links four groups of seven instances
 * (pads, pad tops, light columns, ambient volumes) that Reset puts on a circle of 1200 around his placement. Command 5
 * makes him appear on a pad in a light column (fading in over 1.5 s), taunt, throw three homing fireballs (straight at
 * the player and +-45 degrees, 2000 u/s, 4 damage), fade out (1.5 s), send a wave of light round the columns for
 * `interval` s (2.0) and come back three pads further on. Only while he taunts or throws does a hit count (with the
 * attacker's damage, 10 hp); every hit ends that round and shortens the wave by 0.2 s. At 0 hp: mailbox := 3 every
 * frame, death animation, the columns gone.
 *
 * The dynamic lights 0x498790 of the columns and the wave are registered (rnd_light_add); the original never draws
 * them (docs/LIGHTING.md 7), the port only with WOODY_DYNLIGHT=1. Not ported: actor list 1
 * (0x40c080, only the rocket explosion reads it) and class 90 mode 1, so class 16's group-3 volumes show nothing.
 * The turning sense of class 15's intro spin is verified (round 30): 0x46d220(0, a, 0) builds rows (c, 0, -s), (0, 1, 0),
 * (s, 0, c) from the cos table 0x5e823c (s = -cos[a + 0x80]), 0x40deaf takes T.x row0 + T.y row1 + T.z row2 = (T.x c + T.z s,
 * T.y, T.z c - T.x s) in R/U/W - what state 2 below does. */

typedef struct { int sub[4]; int prio; float speed; } PRec;
static const PRec g_r15[5] = {                                       /* AnimCtrl records 0x4b18a8 (getter 0x40eb30), all restart 1 */
    { { 36, 36, 36, 36 }, 1000, 1 }, { { 33, 36, 36, 36 }, 1001, 1 }, { { 34, 36, 36, 36 }, 1001, 1 }, { { 35, 36, 36, 36 }, 1002, 1 },
    { { 37, -1, -1, -1 }, 1003, 1 } };
static const PRec g_r16[5] = {                                       /* records 0x4b17a8 (getter 0x40d810), all restart 1 */
    { { 41, 41, 41, 41 }, 1000, 3 }, { { 40, 41, 41, 41 }, 1001, 3 }, { { 39, 41, 41, 41 }, 1002, 3 }, { { 38, 41, 41, 41 }, 1001, 3 },
    { { 42, -1, -1, -1 }, 1003, 3 } };
/* Request 0x436b70 queues; of a frame's requests the highest priority wins, the last one among equals */
static void ac_request(BossBState *b, const PRec *tab, int n) { if (b->req < 0 || tab[n].prio >= tab[b->req].prio) b->req = n; }
/* Tick 0x436a50: the winner replaces the running record if its priority is not lower, or once the instance animation has
 * ended (inst+0xc0 == 1: the end of one animation of the chain, or the held last frame); then the chain plays on, the
 * last entry looping, -1 = hold */
static void ac_tick(Instance *in, const PRec *tab, BossBState *b)
{
    const Model *m = in->model; int n = b->req; b->req = -1;
    if (n >= 0 && n != b->rec && (b->rec < 0 || tab[b->rec].prio <= tab[n].prio || b->ended)) {
        b->rec = n; b->sub = 0; b->held = 0; in->anim = tab[n].sub[0]; in->anim_time = 0;   /* restart byte: inst+0xa8 = now */
    }
    b->ended = b->held;
    if (b->rec < 0 || (uint32_t)in->anim >= m->nanims) return;
    const PRec *r = &tab[b->rec]; float L = m->anims[in->anim].duration_s;
    if (b->held) { in->anim_time = L * 0.999f; in->anim_speed = 0; return; }   /* the clock advances after this: stop just before the end */
    in->anim_speed = r->speed;
    if (in->anim_time < L) return;
    int nx = r->sub[b->sub < 3 ? b->sub + 1 : 3]; b->ended = 1;
    if (nx < 0 || (uint32_t)nx >= m->nanims) { b->held = 1; in->anim_time = L * 0.999f; in->anim_speed = 0; }
    else { in->anim_time -= L; in->anim = nx; if (b->sub < 3) b->sub++; }
}
static float ac_len(const Instance *in, const PRec *tab, int n)      /* AnimLen 0x436b90(n, 0) = duration(sub[0]) / speed */
{
    const Model *m = in->model; int s = tab[n].sub[0];
    return (uint32_t)s < m->nanims ? m->anims[s].duration_s / tab[n].speed : 0;
}

static float start_angle(const Instance *in)                         /* PostLoad 0x419ec1: the H angle from the placement, as in boss_init */
{
    Quat q = in->quat; float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w); if (l > 1e-6f) { q.x /= l; q.y /= l; }
    return PI_F * 0.5f - 2.0f * atan2f(-q.y, q.x);
}
static void link_place(Instance *l, Vec3 p)                          /* +0xc and the placement matrix; posed again for the marker queries */
{
    l->position = p; mat4_from_trs(&l->world, p, l->quat, l->scale); ins_pose(l, l->anim, l->anim_time);
}
/* rows (d x up, d, up) = the images of the model's x, y, z axes: local +y along d, local +z up. The .ins default quat
 * (rotx -90) sends local y to -z, so this is enemy_place's yaw quaternion for -d (enemy_place sends local y to -dir). */
static void link_face(Instance *l, Vec3 d)
{
    float yaw = atan2f(-d.x, -d.z), c = cosf(yaw * 0.5f), s = sinf(yaw * 0.5f);
    l->quat.x = 0.70710678f * c; l->quat.y = -0.70710678f * s; l->quat.z = -0.70710678f * s; l->quat.w = -0.70710678f * c;
}
static void ground_follow(Enemy *e, Player *pl, float dt)            /* Enemy::Update -> vtbl[43] 0x41a4e0 (flag 4): v += 200 dt - 0.2 v per frame */
{
    int found; float gy, h2 = e->P.height * 0.5f; int on = enemy_probe(e, pl, (Vec3){ e->pos.x, e->pos.y + h2, e->pos.z }, h2, &gy, &found);   /* 0x41a561 */
    e->vfall += e->P.fall_g * dt - 0.2f * e->vfall; e->pos.y -= e->vfall;
    if (found && e->pos.y <= gy) { e->pos.y = gy; e->vfall = 0; on = 1; }
    game_msgmask(e->inst, 0x200, on);                                /* 0x41a642 / 0x41a65e */
}
/* 0x40ea70(c, r, base, R, H): a sphere against an upright cylinder: the y gap to [base, base + H], then 3D against r + R */
static int sphere_cyl(Vec3 c, float r, Vec3 base, float R, float H)
{
    float dy = c.y > base.y + H ? c.y - (base.y + H) : c.y < base.y ? c.y - base.y : 0, dx = c.x - base.x, dz = c.z - base.z;
    return dx * dx + dy * dy + dz * dz - R * R - 2.0f * r * R - r * r < 0;
}

/* ---- class 15 ------------------------------------------------------------------------------------------------------ */
static const float g_T15[4][3] = { { 750, 1000, 750 }, { 750, 1000, -750 }, { -750, 1000, -750 }, { -750, 1000, 750 } };   /* 0x4b1838 */
static const unsigned char g_pat15[16] = { 0, 1, 2, 3, 1, 2, 3, 0, 2, 3, 0, 1, 3, 0, 1, 2 };                            /* 0x4b1898 */
static const Player *g_b15_pl;
static Vec3 frame_pt(const BossBState *b, float x, float y, float z)   /* C + x R + y U + z W */
{
    return (Vec3){ b->C.x + x * b->R.x + y * b->U.x + z * b->W.x, b->C.y + x * b->R.y + y * b->U.y + z * b->W.y, b->C.z + x * b->R.z + y * b->U.z + z * b->W.z };
}
void boss15_reset(Enemy *e)                                          /* vtbl[17] 0x40da50 (Enemy::Reset 0x41a010 first) */
{
    BossBState *b = &e->bb;
    e->pos = e->home; e->vfall = 0; e->inst->visible = 1;           /* 0x407790: back in the world */
    enemy_reset_probe(e); game_msgmask(e->inst, 0x10, 0);           /* Enemy::Reset 0x41a010: the probe 0x41a148, msgmask 0x10 cleared 0x41a167 */
    e->hp = wenv("WOODY_BOSSHP") ? (float)atof(wenv("WOODY_BOSSHP")) : e->P.hp;   /* WOODY_BOSSHP: testing */
    b->st = 1; e->hit_t = 1.0f; b->t_intro = 0; b->row = 0; b->phase = 0; b->t_taunt = 5.0f; b->phase_flag = 0;
    if (b->crush[0]) for (int i = 0; i < 4; i++) {
        const float *T = g_T15[i];
        if (b->crush[i]) link_place(b->crush[i], frame_pt(b, T[0], T[1], T[2]));
        float a = 2.0f * PI_F * (float)((0x40 - 0x80 * i) & 0x1ff) / 512.0f, s = sinf(a), c = cosf(a);   /* sine table [0x5e823c], 512 steps */
        Instance *l = b->launch[i]; if (!l) continue;
        link_face(l, (Vec3){ s * b->R.x - c * b->W.x, 0, s * b->R.z - c * b->W.z });   /* row 1 (+0x34) points away from C */
        link_place(l, frame_pt(b, 300.0f * s, 15.0f, -300.0f * c));
    }
    b->rec = -1; b->req = -1; b->ended = b->held = 0; ac_request(b, g_r15, 0);   /* AnimCtrl vtbl[4] and Request(0) */
}
void boss15_init(Enemy *e)
{
    BossBState *b = &e->bb; Instance *in = e->inst;
    e->P.radius = 50; e->P.height = 140; e->P.hp = 6; e->P.see = 1000; e->P.dy = 1000; e->P.active_d = 3500; e->P.fall_g = 200; e->P.drop = e->P.rise = 10.0f;   /* subtype 12, 0x41db5f (P+0x2c / 0x30 keep the default 10) */
    e->attackable = 1;
    e->ang = e->want_ang = start_angle(in);
    b->W = (Vec3){ cosf(e->ang), 0, sinf(e->ang) }; b->U = (Vec3){ 0, 1, 0 }; b->R = (Vec3){ b->W.z, 0, -b->W.x };   /* 0x41b860, then 0x46d320 */
    b->C = (Vec3){ in->position.x + b->W.x * 1300.0f, in->position.y, in->position.z + b->W.z * 1300.0f };           /* +0x20c (0x4a9860) */
    b->mail_var = 0;
    boss15_reset(e);
}
/* 0x40e930: crusher i against the player (sphere 200 around its origin against the Perso's cylinder: 1 heart, no push)
 * and against the bombs whose fuse is still in state 2 */
static void crush_hit(Enemy *e, Player *pl, int i)
{
    Vec3 c = e->bb.crush[i]->position;
    if (!pl->dead_kind && sphere_cyl(c, 200.0f, pl->pos, B_PL_R, player_body_height(pl)))
        if (player_hit(pl, 1.0f, (Vec3){ 0, 0, 0 })) player_kill(pl, 3);   /* direction [0x53a4c0]: never written, so no push */
    game_bombs_crush(c, 200.0f);                                     /* |bomb - crusher|^2 < 40000 (0x4a9890) */
}
void boss15_blast(Enemy *e, Vec3 c, float r)                         /* vtbl[40] 0x40e800, no state or immunity test */
{
    BossBState *b = &e->bb;
    if (!sphere_cyl(c, r, e->pos, e->P.radius, e->P.height)) return;
    e->hp -= 1.0f;
    if (e->hp == 0) {
        if (g_b15_pl && g_b15_pl->dead_kind) { e->hp = 1.0f; return; }   /* an Npc of category 1 whose vtbl[36] (dead) holds: no double knock-out */
        b->st = 5;
    } else if (e->hp <= 3.0f) { if (b->phase == 1) { b->phase_flag = 1; b->phase = 2; } }
    else if (e->hp <= 6.0f) { if (b->phase == 0) { b->phase_flag = 1; b->phase = 1; } }
    ac_request(b, g_r15, 3); audio_fx(47, NULL, NULL);
    printf("  BOSS2 %u blast: hp %.0f", e->inst->index, e->hp), puts("");
}
int boss15_protects(const EnemySet *s)                               /* the Perso's Kill 0x44c110: any category-2 / subtype-12 actor whose vtbl[36] 0x40e710 holds */
{
    for (int i = 0; s && i < s->n; i++) if (s->e[i].type == 15 && s->e[i].bb.st == 5 && s->e[i].inst->visible) return 1;
    return 0;
}
void boss15_update(Enemy *e, Player *pl, Vec3 cam, float dt)
{
    BossBState *b = &e->bb; Instance *in = e->inst;
    g_b15_pl = pl;
    if (!in->visible || !game_enemy_thinks(in)) return;              /* out of the world (0x407850) or not in this frame's list world+0x64: no Think (0x42b400) */
    if (dist3(e->pos, cam) >= e->P.active_d && e->hp > 0) return;    /* Think 0x41a320: 3500 */
    enemy_common_move(e, pl, (Vec3){ 0, 0, 0 }, &e->knock_t, e->knock_dir, 2, dt);   /* Enemy::Update 0x41a3e0: Stand's Tick = 0x41b2c0 (sweep r 50, up 10) */
    ground_follow(e, pl, dt); enemy_place(e);                        /* its vtbl[45] is empty */
    if (!b->crush[0]) return;
    game_boss_bar(1, (int)e->hp, (int)e->P.hp);                      /* 0x40dd80 */
    switch (b->st) {
    case 1:                                                          /* waiting for command 4, 0x40dda0 (no answer is written) */
        if (game_var_get(b->mail_var) == 4) { b->st = 2; ac_request(b, g_r15, 0); audio_fx(44, NULL, NULL); break; }
        /* fallthrough: the same taunts as state 4 */
    case 4:
        if ((b->t_taunt -= dt) < 0) { b->t_taunt = crt_rnd() * 10.0f + 5.0f; ac_request(b, g_r15, crt_rnd() * 2.0f < 1.0f ? 1 : 2); }   /* every 5..15 s */
        else ac_request(b, g_r15, 0);
        if (b->st == 1) break;
        {   /* the attack cycle 0x40e2a6 */
            int idx = g_pat15[b->nstep * b->row + b->step]; const float *T = g_T15[idx];
            if (b->new_step) {
                if (b->fire == 1) game_launcher_start(b->launch[(idx + 3) & 3]);   /* the launcher on the crusher's diagonal */
                else if (b->fire == 4) for (int k = 0; k < b->fire; k++) game_launcher_start(b->launch[k]);
                b->new_step = 0;
            }
            b->t_cycle += dt;
            if (b->t_cycle > 2.0f) { if (b->t_down < b->down) b->t_down += dt; else if (b->t_up < b->up) b->t_up += dt; }   /* 0x4a9870 */
            if (b->t_down < b->down) {                               /* slamming down to C's height */
                link_place(b->crush[idx], frame_pt(b, T[0], T[1] - T[1] * (b->t_down / b->down), T[2])); b->crush[idx]->visible = 1;   /* 0x4077f0 */
                crush_hit(e, pl, idx);
            } else if (b->t_up < b->up) {                            /* rising again */
                if (b->t_up == 0) { audio_fx(38, NULL, NULL); game_cam_shake(0.7f); }   /* the impact */
                link_place(b->crush[idx], frame_pt(b, T[0], b->t_up / b->up * T[1], T[2]));
                crush_hit(e, pl, idx);
            } else if (b->t_cycle > b->hold) {                       /* the cycle is over: the next row of the pattern */
                b->row = (b->row + 1) % b->nstep; b->t_down = b->t_up = 0; b->new_step = 1;
                if (b->phase_flag) { b->st = 3; b->phase_flag = 0; }
                audio_fx(48, NULL, NULL); b->t_cycle = 0; b->step = 0;
            } else {                                                 /* back on top: the next crusher of the row */
                link_place(b->crush[idx], frame_pt(b, T[0], T[1], T[2]));
                if (b->step < b->nstep - 1) { b->step++; b->t_down = b->t_up = 0; }
            }
            if (wenv("WOODY_BOSSLOG")) printf("  Boss2 -> Vie:%f   AttackPhase:%d  (t %.2f crusher %d down %.2f up %.2f cycle %.2f)", e->hp, b->phase, game_time(), idx, b->t_down, b->t_up, b->t_cycle), puts("");   /* 0x4b1934 */
        }
        break;
    case 2: {                                                        /* the crushers spin 0x40de59: a turn in 2.4 s, a quarter back in 0.6 s */
        b->t_intro += dt; float f = b->t_intro * (1.0f / 3.0f), a;
        if (f <= 0.8f) a = (float)(int)(f * 1.25f * 512.0f);
        else if (f < 1.0f) a = (float)(int)(512.0f - (f - 0.8f) * 1.25f * 512.0f);
        else a = 0;                                                  /* 90 degrees off, which the square does not show */
        float c = cosf(a * 2.0f * PI_F / 512.0f), s = sinf(a * 2.0f * PI_F / 512.0f);   /* 0x46d220(0, a, 0): about the frame's up axis (sense verified) */
        for (int i = 0; i < 4; i++) if (b->crush[i]) { const float *T = g_T15[i]; link_place(b->crush[i], frame_pt(b, T[0] * c + T[2] * s, T[1], T[2] * c - T[0] * s)); }
        if (f >= 1.0f) { b->t_intro = 0; b->st = 3; audio_fx_stop(44, NULL, 0); }
        break; }
    case 3:                                                          /* set up the phase 0x40e151 */
        b->nstep = 4; b->new_step = 1; b->hold = 14.0f;
        if (b->phase == 0) { b->step = 0; b->t_down = b->t_up = b->t_cycle = 0; b->fire = 1; b->up = 1.0f; b->down = 0.3f; }
        else if (b->phase == 1) { b->fire = 4; b->up = 0.9f; b->down = 0.2f; }
        else { b->fire = 4; b->up = 0.7f; b->down = 0.2f; }
        b->st = 4;
        break;
    case 5:                                                          /* beaten 0x40e6bf, every frame */
        game_var_set(b->mail_var, 3); ac_request(b, g_r15, 4); game_bombs_discard();
        break;
    }
    ac_tick(in, g_r15, b);
    if (wenv("WOODY_BOSSLOG") && b->st != 4) printf("  boss15 st %d hp %.0f pos %.0f %.0f %.0f rec %d anim %d", b->st, e->hp, e->pos.x, e->pos.y, e->pos.z, b->rec, in->anim), puts("");
}

/* ---- class 16 ------------------------------------------------------------------------------------------------------ */
static Vec3 pad_top(const BossBState *b, int i) { Vec3 p = b->grp[0][i]->position; p.y += 30.0f; return p; }   /* 0x4a9740 */
static void col_fade(Instance *c, float f) { if (c) c->fade = c->fade_target = f; }   /* +0x6c; a scripted instance: its fade target must follow */
static void col_light(const Instance *c, float r)                    /* 0x498790([0x4c4cac], 0, &col->pos, white, r) */
{
    static const float white[3] = { 255.0f, 255.0f, 255.0f };
    if (c) rnd_light_add(0, c->position, white, r);
}
static void face_player(Enemy *e, const Player *pl) { e->ang = e->want_ang = ang_to(e->pos, pl->pos); }   /* 0x41b230: H snapped to the target */
static void loop_anim0(Instance *g, float now)                       /* 0x436ca0(inst, 0.3, 0, 0, 0, 0): anim 0 in all slots, speed 0.3 * 3 */
{
    g->slot[0] = g->slot[1] = g->slot[2] = g->slot[3] = 0; g->a_speed = g->a_base_speed = 0.9f; g->a_start = now; g->a_ended = 0;
}
void boss16_reset(Enemy *e)                                          /* vtbl[17] 0x40c8f0 (Enemy::Reset 0x41a010 first) */
{
    BossBState *b = &e->bb; float now = game_time();
    e->pos = e->home; e->vfall = 0; e->inst->visible = 1;
    enemy_reset_probe(e); game_msgmask(e->inst, 0x10, 0);           /* Enemy::Reset 0x41a010: the probe 0x41a148, msgmask 0x10 cleared 0x41a167 */
    e->hp = wenv("WOODY_BOSSHP") ? (float)atof(wenv("WOODY_BOSSHP")) : e->P.hp;   /* WOODY_BOSSHP: testing */
    b->st = 1; b->cur = 0; b->interval = 2.0f;
    for (int i = 0; i < 7; i++) {
        float a = (float)i * 0.8975979f, x = b->C.x + cosf(a) * b->radius, z = b->C.z + sinf(a) * b->radius;   /* 2 pi / 7 (0x4a9744) */
        Instance *g;
        if ((g = b->grp[0][i])) link_place(g, (Vec3){ x, b->C.y, z });                 /* the pads */
        if ((g = b->grp[2][i])) { link_place(g, (Vec3){ x, b->C.y + 30.0f, z }); g->visible = 0; b->glow_n[i] = 0; loop_anim0(g, now); }   /* the columns: out of the world */
        if ((g = b->grp[1][i])) { link_place(g, (Vec3){ x, b->C.y + 30.0f, z }); loop_anim0(g, now); }
        if ((g = b->grp[3][i])) { link_place(g, (Vec3){ x, b->C.y - b->depth[i], z }); g->visible = 1; }   /* bottom on C.y; 0x40cb41 Recell 0x4077f0 = back in the world */
    }
    if (b->grp[0][0]) e->pos = pad_top(b, 0);
    for (int k = 0; k < 16; k++) b->wave[k].t = -1;
    b->rec = -1; b->req = -1; b->ended = b->held = 0; ac_request(b, g_r16, 0);
}
void boss16_init(Enemy *e)
{
    BossBState *b = &e->bb; Instance *in = e->inst;
    e->P.radius = 50; e->P.height = 140; e->P.hp = 10; e->P.see = 4600; e->P.dy = 3000; e->P.active_d = 3000; e->P.fall_g = 200; e->P.drop = e->P.rise = 5000.0f;   /* subtype 13, 0x41dba7 (P+0x2c / 0x30 = 5000) */
    e->attackable = 1;
    e->ang = e->want_ang = start_angle(in);
    b->C = in->position; b->radius = 1200.0f;                        /* +0x254, +0x268 */
    b->mail_var = 0;
    boss16_reset(e);
}
int boss16_take_damage(Enemy *e, float dmg)                          /* vtbl[39] 0x40d480 */
{
    BossBState *b = &e->bb;
    if (b->st != 3 && b->st != 4) return 0;                          /* only while he stands on his pad, visible */
    b->interval -= 0.2f; b->t298 = 0; b->st = 5;                     /* 0x4a9760 */
    e->hp -= dmg;                                                     /* Enemy_TakeDamage 0x41adc0: his vtbl[53] is 0, so no knockback refuses it */
    audio_fx_stop(66, NULL, 0); audio_fx(65, NULL, NULL);
    printf("  BOSS3 %u hit, hp %.0f", e->inst->index, e->hp), puts("");
    if (e->hp <= 0) { b->st = 7; return 1; }
    ac_request(b, g_r16, 2);
    return 1;                                                         /* "dead" whatever the hp (mov al, 1), literally */
}
static void wave_tick(Enemy *e, float dt)                            /* the records 0x40c610 (effect pool [0x5e823c]+0xdb8), 0.9 s each */
{
    BossBState *b = &e->bb;
    for (int k = 0; k < 16; k++) {
        if (b->wave[k].t < 0) continue;
        int i = b->wave[k].idx; Instance *c = b->grp[2][i]; float f = (b->wave[k].t += dt) / 0.9f;
        if (f < 1.0f) {
            float v = f * 0.4f + 0.6f;                               /* 0x4a9654, 0x4a9650: from 0.6 to faded out */
            if (b->glow_fr[i] == b->frame) { if (c && v < c->fade) col_fade(c, v); }   /* two records on one column: the brighter wins */
            else { col_fade(c, v); b->glow_fr[i] = b->frame; }
            col_light(c, (1.0f - f) * 400.0f);                       /* 0x40c6dc: radius 400 shrinking to 0 */
        } else {
            if (--b->glow_n[i] == 0 && b->st == 6 && c) { col_fade(c, 0.6f); c->visible = 0; }   /* 0x407850 */
            b->wave[k].t = -1;
        }
    }
}
void boss16_update(Enemy *e, Player *pl, Vec3 cam, float dt)
{
    BossBState *b = &e->bb; Instance *in = e->inst;
    if (!in->visible || !game_enemy_thinks(in)) return;              /* list world+0x64 (0x42b400) */
    if (dist3(e->pos, cam) >= e->P.active_d && e->hp > 0) return;    /* Think 0x41a320: 3000 */
    enemy_common_move(e, pl, (Vec3){ 0, 0, 0 }, &e->knock_t, e->knock_dir, 2, dt);   /* Stand's Tick = 0x41b2c0 (sweep r 50, up 70: P+0x30 = 5000) */
    ground_follow(e, pl, dt); enemy_place(e);
    if (!b->grp[0][0]) return;
    b->frame++;
    game_boss_bar(1, (int)e->hp, (int)e->P.hp);                      /* 0x40cbf1 */
    Instance *col = b->grp[2][b->cur];
    switch (b->st) {
    case 1:                                                          /* invisible, waiting for command 5, 0x40cc0d */
        in->fade = 1.0f;
        if (game_var_get(b->mail_var) == 5) { b->t298 = 0; b->st = 2; e->pos = pad_top(b, b->cur); e->vfall = 0; }
        break;
    case 2: {                                                        /* appear 0x40cc73: 1.5 s, in his column */
        b->t298 += dt; float f = b->t298 * 0.6666667f;               /* 0x4a975c */
        if (col) col->visible = 1;                                   /* 0x4077f0 */
        col_fade(col, 0.6f); col_light(col, 400.0f);                 /* 0x40cce7 */
        if (f < 1.0f) { face_player(e, pl); in->fade = 1.0f - f; }
        else { b->st = 3; b->t29c = 0; audio_fx(66, NULL, NULL); }
        break; }
    case 3:                                                          /* taunt 0x40cd3a for AnimLen(1) */
        b->t29c += dt; ac_request(b, g_r16, 1); col_fade(col, 0.6f); col_light(col, 400.0f); face_player(e, pl);   /* 0x40cd9d */
        if (ac_len(in, g_r16, 1) < b->t29c) { b->t2a0 = 0; b->st = 4; }
        break;
    case 4:                                                          /* throw 0x40cdde: at 62 % of AnimLen(3) (0x4a9758) */
        ac_request(b, g_r16, 3); col_fade(col, 0.6f); b->t2a0 += dt; col_light(col, 400.0f);   /* 0x40cea4 */
        if (ac_len(in, g_r16, 3) * 0.62f < b->t2a0) {
            audio_fx(67, NULL, NULL);
            Vec3 o = { e->pos.x, e->pos.y + 150.0f, e->pos.z }, d = { pl->pos.x - o.x, 0, pl->pos.z - o.z };   /* 0x4a9754; flat */
            float l = sqrtf(d.x * d.x + d.z * d.z); if (l > 1e-4f) { d.x /= l; d.z /= l; } else d = (Vec3){ 0, 0, 1 };
            Vec3 R = { d.z, 0, -d.x };                               /* 0x46d320(d): rows R, up, d */
            static const float D[3][2] = { { 0.70710677f, 0.70710677f }, { -0.70710677f, 0.70710677f }, { 0, 1 } };   /* (x, z) of the three local directions */
            for (int k = 0; k < 3; k++)                              /* template 1, speed P+0x60, damage P+0x40, steer P+0x68, vertical P+0x70, visual P+0x74 */
                game_enemy_shot_v(e, o, (Vec3){ D[k][0] * R.x + D[k][1] * d.x, 0, D[k][0] * R.z + D[k][1] * d.z }, 2000.0f, 4.0f, 0.1f, 30.0f, 30.0f, 3, 20);
            b->t298 = 0; b->st = 5;
        }
        break;
    case 5: {                                                        /* vanish 0x40d15e: 1.5 s */
        b->t298 += dt; float f = b->t298 * 0.6666667f;
        ac_request(b, g_r16, 0); col_fade(col, f * 0.4f + 0.6f); col_light(col, (1.0f - f) * 400.0f);   /* 0x40d1da */
        if (f < 1.0f) in->fade = f;
        else { if (col) col->visible = 0; b->st = 6; b->t2a4 = 0; b->t264 = 0; in->fade = 1.0f; }
        if (find_target(e, pl)) face_player(e, pl);                  /* vtbl[48](1, 0) */
        break; }
    case 6:                                                          /* the wave 0x40d24a: ten columns light up, one after the other, in `interval` s */
        b->t2a4 += dt; b->t264 += dt; in->fade = 1.0f;
        if (b->t2a4 < b->interval) {
            int n = (int)(b->t264 * 10.0f / b->interval); b->t264 -= b->interval * 0.1f * (float)n;
            int k = (int)(b->t2a4 * -10.0f / b->interval), first = (b->cur - k - n) % 7 + 1;
            for (int j = 0; j < n; j++) {
                int s = 0; while (s < 16 && b->wave[s].t >= 0) s++; if (s == 16) break;   /* the original's pool holds 2000 */
                int i = (first + j) % 7; b->wave[s].t = 0; b->wave[s].idx = i;
                if (b->grp[2][i]) b->grp[2][i]->visible = 1;          /* 0x4077f0 */
                b->glow_n[i]++; b->glow_fr[i] = b->frame;
            }
        } else { b->cur = (b->cur + 3) % 7; b->st = 2; b->t298 = 0; e->pos = pad_top(b, b->cur); e->vfall = 0; }   /* three pads on */
        break;
    case 7:                                                          /* beaten 0x40d3f6, every frame */
        game_var_set(b->mail_var, 3); ac_request(b, g_r16, 4);
        for (int i = 0; i < 7; i++) if (b->grp[2][i]) b->grp[2][i]->visible = 0;
        break;
    }
    wave_tick(e, dt);
    ac_tick(in, g_r16, b);
    if (wenv("WOODY_BOSSLOG")) printf("  boss16 t %.2f st %d cur %d hp %.0f fade %.2f pos %.0f %.0f %.0f rec %d anim %d", game_time(), b->st, b->cur, e->hp, in->fade, e->pos.x, e->pos.y, e->pos.z, b->rec, in->anim), puts("");
}

/* messages 61 (class 15, 0x40e749) and 62 (class 16, 0x40d55d): every linked instance gets +8 |= 0x20 (no re-cell on
 * animation: Instance.cell_fixed, the cell point stays the .ins position +0xc), class 15's four crushers also 0x40 (not
 * collidable), class 16's columns and volumes go out of the world (0x407850). Then Reset puts everything in place (class
 * 16: after every group), and its Recell 0x40cb41 puts the group-3 volumes back into the world. Without the 0x20 the volumes
 * would be celled on their animated root, which node 0's track puts 64 below the deck (no floor group: never listed, so
 * their class-90 think, the rising yellow motes of AMBIENT.md, would never run). */
void enemies_boss_links(EnemySet *s, Instance *inst, int id, int group, Instance **li, int n)
{
    Enemy *e = NULL; for (int i = 0; i < s->n; i++) if (s->e[i].inst == inst && (s->e[i].type == 15 || s->e[i].type == 16)) e = &s->e[i];
    if (!e) return;
    BossBState *b = &e->bb;
    if (id == 61 && e->type == 15) {
        for (int i = 0; i < 8 && i < n; i++) { if (li[i]) li[i]->cell_fixed = 1; if (i < 4) { b->crush[i] = li[i]; if (li[i]) li[i]->noncollide = 1; } else b->launch[i - 4] = li[i]; }
        boss15_reset(e);
    } else if (id == 62 && e->type == 16 && group >= 0 && group < 4) {
        for (int i = 0; i < 7 && i < n; i++) {
            Instance *g = li[i]; b->grp[group][i] = g; if (!g) continue;
            g->cell_fixed = 1;                                         /* +8 |= 0x20 */
            if (group == 2 || group == 3) g->visible = 0;
            if (group == 3) {                                          /* 0x40d6be: the lowest point of the model, rotated and scaled */
                Mat4 w; mat4_from_trs(&w, (Vec3){ 0, 0, 0 }, g->quat, g->scale); const Model *m = g->model; float lo = 0;
                for (uint32_t k = 0; k < m->npoints; k++) { Vec3 p = m->points[k].pos; float y = w.m[1] * p.x + w.m[5] * p.y + w.m[9] * p.z; if (k == 0 || y < lo) lo = y; }
                b->depth[i] = lo;
            }
        }
        boss16_reset(e);
    }
}
