/* enemy.c - enemy types 4/5/6 after docs/ENEMY.md (state machine 0x418cf0, behaviours 0x41b2c0..0x41cf00).
 * The obstacle sensor (16 directions, docs/OBSTACLE.md 3) steers Chase and Wander; Wander (0x41bf30) is the original's
 * weighted action machine with its animation chains. Every behaviour but the path runs the common move 0x41b2c0 each frame:
 * the swept sphere 0x437580 against the world and the instance press nodes, the platform carry of the ground probe and the
 * free test (floor normal, drop) that calls OnBlocked (enemy_common_move, shared with the bosses in boss.c). */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "enemy.h"
#include "player.h"
#include "audio.h"
#include "crtrand.h"

static void sac_init(Enemy *e);
static void sac_reset(Enemy *e);

/* constants that no message changes */
#define E_ACC      1000.0f
#define E_STEP     10.0f      /* P+0x30 / P+0x2c: max step up / drop */
#define E_KNOCK    600.0f     /* P+0x44 */

/* parameter block P per type (0x41d510 and the subtype cases; docs/ENEMY.md 2.3 and 8.1) */
static EnemyParams params_for(int type)
{
    EnemyParams p = { 30, 140, 200, 600, 800, 1500, 600, 1.5708f, 6.2832f, 800, 1, 1.5f, 1, 1, 2.0f, 400, 300, 0, 3000, 2, 19 };
    if (type == 5) p.cool = 1.0f;
    if (type == 6) { p.hp = 2; p.cool = 0.5f; }
    if (type >= 7) { p.dy = 800; p.turn_fast = 12.5664f; p.melee = 150; p.shot_visual = 0; p.shot_fx = 17; p.steer = 0.2f; }
    if (type == 8) { p.height = 130; p.hp = 2; p.cool = 1.0f; p.reload = 1.5f; p.steer = 0; p.shot_visual = 1; p.shot_fx = 18; }
    if (type == 13) { p.walk = 100; p.run = 300; p.dash = 500; p.dy = 10000; p.hp = 3; p.bite = 2; p.cool = 0.5f; p.shot_dmg = 2; p.reload = 1.0f; p.leash = 1000; p.melee = 300; p.shot_visual = 3; p.shot_fx = 20; }   /* docs/ENEMY2.md 2 */
    if (type == 9) { p.leash = 1000; p.hp = 3; p.bite = 3; p.shot_dmg = 2; p.melee = 10; p.steer = 0; p.shot_visual = 3; p.shot_fx = 20; }
    if (type == 12) { p.radius = 60; p.see = 2400; p.dy = 1000; p.height = 280; p.hp = 5; p.bite = 3; p.shot_dmg = 4; p.reload = 3.5f; p.melee = 300; }   /* subtype 8, docs/ENEMY2.md 2 */
    p.drop = p.rise = type == 13 ? 10000.0f : 10.0f;                        /* P+0x2c / P+0x30: the flyers (subtypes 9/10) have no edge test */
    return p;
}

/* logical animation -> main .ins animation and speed divisor (table 0x4b29b8) */
enum { EA_RUN, EA_WALK, EA_DASH, EA_BRAKE, EA_MISS, EA_WIN, EA_HIT, EA_DEAD, EA_IDLE, EA_TURN };
typedef struct { int anim; float speed; int hold; } EAnim;
static const EAnim g_ea[] = {
    { 2, 3, 0 }, { 4, 3, 0 }, { 13, 10, 0 }, { 15, 2, 1 }, { 14, 3, 1 }, { 18, 1.5f, 1 }, { 11, 4, 1 }, { 12, 3, 1 }, { 6, 3, 0 }, { 16, 3, 0 } };
/* ghost, type 13 (records 0x4b2190, docs/ENEMY2.md 3.6) */
static const EAnim g_ea13[] = {
    { 2, 3, 0 }, { 4, 3, 0 }, { 13, 3, 0 }, { 15, 3, 1 }, { 14, 2, 1 }, { 16, 3, 1 }, { 11, 4, 1 }, { 12, 3, 1 }, { 6, 2, 0 }, { 0, 3, 0 } };
#define EA(e, a) ((e)->type == 13 ? g_ea13[a] : g_ea[a])
static float ea_len(const Enemy *e, int a) { const Model *m = e->inst->model; int s = EA(e, a).anim; return (uint32_t)s < m->nanims ? m->anims[s].duration_s / EA(e, a).speed : 0.5f; }
static void ea_play(Enemy *e, int a)
{
    Instance *in = e->inst; const Model *m = in->model; int s = EA(e, a).anim; e->lanim = -1; if ((uint32_t)s >= m->nanims) return;
    if (in->anim != s) { in->anim = s; in->anim_time = 0; }
    in->anim_speed = EA(e, a).speed;
    if (EA(e, a).hold && in->anim_time > m->anims[s].duration_s - 0.15f) { in->anim_time = m->anims[s].duration_s * 0.999f; in->anim_speed = 0; }   /* held on the last frame: the clock advances after this, so it must stop */
}

float enemy_radius(const Enemy *e) { return e->P.radius; }
static void wander_init(Enemy *e);
static void wander_restart(Enemy *e, int a);
float enemy_height(const Enemy *e) { return e->P.height; }

static void ground_snap(Enemy *e, struct Player *pl);
void enemies_add(EnemySet *s, Instance *inst, int type, struct Player *pl)
{
    if (s->n >= MAX_ENEMIES) return;
    for (int i = 0; i < s->n; i++) if (s->e[i].inst == inst) return;
    Enemy *e = &s->e[s->n++]; Enemy z = { 0 }; *e = z;
    if (type >= 4 && type <= 13) s->total++;                                    /* [0x4c5330]++ in the class PostLoad (vtbl[1]); the bosses 14..16 do not count */
    e->col_cur = 0xffffffffu;                                                   /* the probes' ctor 0x436cf0 */
    e->inst = inst; e->type = type; e->pos = e->home = inst->position; e->P = params_for(type); e->hp = e->P.hp;
    enemy_sensor_init(e); e->need_snap = type < 13 || type == 14;              /* subtypes < 9: types 4..12 and Buzz */
    e->cool = -1; e->attackable = 1; e->speed = e->want_speed = e->P.walk; e->path_dir = 1; e->path_to = 1;
    { Vec3 f = mat4_apply(&inst->world, (Vec3){ 0, -1, 0 }); e->ang = e->want_ang = e->start_ang = atan2f(f.z - inst->position.z, f.x - inst->position.x); }   /* PostLoad 0x419ec1: angle0 = pos -> pos - row(+0x34) */
    e->start = inst->position; e->lanim = -1; wander_init(e);
    inst->anim = 0; inst->anim_time = 0; inst->anim_speed = 0;                  /* Reset 0x41a010 -> 0x42e250: frame 0 of anim 0, clock stopped. Only Update (vtbl[52])
                                                                                 * runs the AnimCtrl, so an enemy that never thinks keeps this pose: S1A's type 4 (message
                                                                                 * 11 [37, 10] = activation distance 10) stand frozen; the .ins default looped anim 0 */
    if (type >= 7 && type <= 9) sac_init(e);
    e->st = inst->traj.npoints > 1 ? 0 : (type >= 7 && type <= 9 ? 3 : 8);
    e->behav = 2;                                                                /* Stand: the thrower (PostLoad 0x410f80 makes only that one), bosses 15 / 16 */
    if (e->st == 0) { Vec3 a = inst->traj.points[0], b = inst->traj.points[1]; e->ang = atan2f(b.z - a.z, b.x - a.x); e->behav = 3; }
    else if (type >= 4 && type <= 13 && type != 12) { wander_restart(e, -1); e->behav = 0; }   /* Reset: behaviour Wander, vtbl[6]() */
    if (type == 13) e->hand = crt_rand() & 1;                                    /* ghost Reset 0x413a77, after its behaviour restart (0x413a1f) */
    if (type == 12) { e->st = 0; e->nlong = 4; e->idle_a = 9; e->idle_t = 0; game_msgmask(inst, 0x10, 0); if (wenv("WOODY_BOSSHP")) e->hp = (float)atof(wenv("WOODY_BOSSHP"));   /* testing */ Vec3 f = mat4_apply(&inst->world, (Vec3){ 0, -1, 0 }); e->ang = atan2f(f.z - inst->position.z, f.x - inst->position.x); }   /* Reset 0x411020: Stilstaan, timers 0, idle 9, state 0 (it clears msgmask 0x10); he keeps his .ins facing */
    inst->scripted = 0;
    if (type == 14) boss_init(e);
    if (type == 15) boss15_init(e);
    if (type == 16) boss16_init(e);
    /* the class factory 0x403502 runs PostLoad (vtbl[1]) and then Reset (vtbl[17], 0x403e74) right here, so the ground snap 0x41a1a0
     * happens during the script init, not at the first Think: an enemy that never thinks before a cinematic shows it (K3A 485/486,
     * placed 213 above the floor and seen in the door-306 cinematic, where Think 0x41a320 returns early) stands on the floor */
    if (pl && type < 13) { ground_snap(e, pl); enemy_place(e); }
}

static int bomber_peck(Enemy *e);
static void bomber_blast(Enemy *e, Vec3 c, float r);
int enemy_take_damage(Enemy *e, float dmg, Vec3 dir)
{
    if (e->type == 14) return boss_take_damage(e, NULL, 0);          /* vtbl[40] 0x41ae20: vtbl[39](0, hp, &dir, NULL, 0) */
    if (e->type == 15) return 0;                                       /* 0x40e720: xor al, al */
    if (e->type == 16) return boss16_take_damage(e, dmg);
    if (e->type == 12) return bomber_peck(e);                          /* the peck / charge run of the player (kinds 0 / 1) */
    int shooter = e->type >= 7 && e->type <= 9;
    if (e->removed || e->st == (shooter ? 10 : 12) || e->hit_t > 0 || e->knock_t > 0) return 0;
    if (shooter) sac_reset(e);                                         /* 0x417f60: AnimCtrl reset, so the hit record 12 takes over whatever runs */
    e->st = shooter ? 4 : 9; e->hit_t = e->knock_t = 0.25f; e->knock_dir = dir; e->hp -= dmg;
    return e->hp <= 0;
}

int enemy_hit(Enemy *e, float dmg, Vec3 dir, Vec3 pt, int kind)
{
    if (e->type == 14) return boss_take_damage(e, &pt, kind);       /* 0x40fe90 passes pt and the kind on: no star for kind 2 (the special attack) */
    if (e->type == 15) return 0;                                     /* 0x40e720: nothing hurts him but a blast */
    if (e->type == 16) { int r = boss16_take_damage(e, dmg); if (r && kind != 2) game_hit_star(pt); return r; }   /* 0x40d480 -> 0x41adc0 with the kind */
    if (e->type == 12) return kind == 0 || kind == 1 ? bomber_peck(e) : 0;   /* 0x411ab0 only reacts to kinds 0 / 1: the special attack does nothing */
    (void)kind;                                                      /* types 4..9, 13 call Enemy_TakeDamage with kind 0, so they always get the star */
    int shooter = e->type >= 7 && e->type <= 9;
    if (e->removed || e->st == (shooter ? 10 : 12) || e->hit_t > 0 || e->knock_t > 0) return 0;
    int died = enemy_take_damage(e, dmg, dir);
    game_hit_star(pt);
    return died;
}

/* bomb blast 0x44d650 -> vtbl[40] 0x41ae20 on every Npc of category 2: inside r (3D, to the instance origin) it dies at once,
 * through the hit state like a last peck (docs/BOMB.md 4.2); the boss takes its usual single point */
void enemies_blast(EnemySet *s, Vec3 c, float r)
{
    for (int i = 0; i < s->n; i++) {
        Enemy *e = &s->e[i]; if (e->removed || !e->inst->visible || e->hp <= 0) continue;
        if (e->type == 12) { bomber_blast(e, c, r); continue; }
        if (e->type == 15) { boss15_blast(e, c, r); continue; }       /* 0x40e800: his cylinder, 1 hp */
        float dx = e->pos.x - c.x, dy = e->pos.y - c.y, dz = e->pos.z - c.z; if (dx * dx + dy * dy + dz * dz >= r * r) continue;
        float l = sqrtf(dx * dx + dz * dz); Vec3 d = l > 1e-3f ? (Vec3){ dx / l, 0, dz / l } : (Vec3){ 0, 0, 1 };
        e->hit_t = e->knock_t = 0; enemy_take_damage(e, e->type == 14 ? 1.0f : e->hp, d);
    }
}

static float ang_diff(float a, float b) { float d = a - b; while (d > 3.14159265f) d -= 6.2831853f; while (d < -3.14159265f) d += 6.2831853f; return d; }
static void steer(Enemy *e, float want, float rate, float dt) { float d = ang_diff(want, e->ang), m = rate * dt; if (d > m) d = m; if (d < -m) d = -m; e->ang += d; e->ang = ang_diff(e->ang, 0); }   /* H.Tick 0x41ba90 wraps the angle */

/* ---- obstacle sensor Enemy+0x124 (docs/OBSTACLE.md 3) ------------------------------------------------------------
 * 16 directions i·pi/8 (+ a jitter that steps pi/40 per probe and wraps at pi/8); each frame ONE of them is probed
 * (0x41d010): the endless ray 0x435810 from R above the feet to the floor point R ahead, i.e. 45 degrees down, R swinging
 * 200 -> 100 -> 200 in steps of 10 per frame (0x41d4a0). On flat floor it hits at S = R·sqrt2; a hit more than P+0x2c·sqrt2
 * further is a drop (kind 3), more than P+0x30·sqrt2 nearer a wall or a step up (kind 2), anything between free (1); a
 * start point outside the world is kind 0. Mode 3 (vtbl[46] 0x45bd30, every class) blocks on 2 and 3 (0x41d400). */
#define SENS_STEP  0.39269909f                                             /* 0x4aa15c: pi/8 */
void enemy_sensor_init(Enemy *e)                                           /* 0x41cfc0 */
{
    EnemySensor *S = &e->sens; memset(S, 0, sizeof *S);
    S->r = 200.0f; S->dr = 10.0f;
    for (int i = 0; i < 16; i++) S->ang[i] = (float)i * SENS_STEP;
    for (int i = 0; i < 17; i++) S->free[i] = 1;                           /* +0xb4 = the 17th slot, always free (0x41d002) */
}
static int sens_pass(int kind) { return !(kind == 2 || kind == 3); }      /* 0x41d400(3, kind) */
static void sensor_probe(Enemy *e, struct Player *pl)                      /* 0x41d010 */
{
    EnemySensor *S = &e->sens; int kind;
    S->jit += 0.07853982f; if (S->jit >= SENS_STEP) S->jit = 0;            /* 0x4aa160 */
    float a = (float)S->i * SENS_STEP + S->jit; S->ang[S->i] = a;
    Vec3 st = { e->pos.x, e->pos.y + S->r, e->pos.z }, d = { cosf(a) * S->r, -S->r, sinf(a) * S->r };
    if (gel_cell(pl->gel, st) < 0) kind = 0;                                /* 0x428cc0: no cell */
    else {
        float t; int k = player_ray_endless(pl, st, d, &t);
        if (k == 1) kind = 3;                                               /* nothing below: the original reads a stale [0x53a558] here */
        else {
            float dist = t * sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
            if (S->s + sqrtf(2.0f * e->P.drop * e->P.drop) < dist) kind = 3;          /* 0x41d1c6: the floor drops away */
            else if (dist < S->s - sqrtf(2.0f * e->P.rise * e->P.rise)) kind = 2;    /* 0x41d1e2: wall or step up */
            else kind = 1;
        }
    }
    S->kind[S->i] = kind; S->kind[16] = 1;                                  /* +0x9c */
    if (++S->i >= 16) S->i = 0;
}
void enemy_sensor_tick(Enemy *e, struct Player *pl)                        /* 0x41d4a0(mode 3) */
{
    EnemySensor *S = &e->sens;
    S->r += S->dr;
    if (S->r > 200.0f) { S->r = 200.0f; S->dr = -S->dr; } else if (S->r < 100.0f) { S->r = 100.0f; S->dr = -S->dr; }
    S->s = sqrtf(S->r * S->r * 2.0f);
    sensor_probe(e, pl);
    for (int i = 0; i < 17; i++) S->free[i] = (unsigned char)sens_pass(S->kind[i]);   /* 0x41d430 */
    if (S->i == 0 && wenv("WOODY_SENSLOG")) {                            /* testing: the 16 kinds once per sweep, direction 0 = +x, counter-clockwise seen from above is +z */
        char m[17]; for (int i = 0; i < 16; i++) m[i] = (char)('0' + S->kind[i]); m[16] = 0;
        printf("  SENS %u (type %d, st %d) at %.0f %.0f %.0f ang %.0f: %s", e->inst->index, e->type, e->st, e->pos.x, e->pos.y, e->pos.z, e->ang * 57.2958f, m), puts("");
    }
}
static float sens_arc(float a, float b) { return fabsf(ang_diff(a, b)); }  /* 0x4401c0 */
int enemy_sensor_free(const Enemy *e, float ang)                            /* 0x41d2a0 -> 0x41d240: the nearest of the 16 */
{
    int bi = -1; float bd = 6.2831855f;
    for (int i = 0; i < 16; i++) { float d = sens_arc(e->sens.ang[i], ang); if (d < bd) { bd = d; bi = i; } }
    return bi < 0 || e->sens.free[bi];
}
float enemy_sensor_nearest_free(const Enemy *e, float ang)                  /* 0x41d310 */
{
    int bi = -1; float bd = 6.2831855f;
    for (int i = 0; i < 16; i++) if (e->sens.free[i]) { float d = sens_arc(e->sens.ang[i], ang); if (d < bd) { bd = d; bi = i; } }
    return bi < 0 ? ang : e->sens.ang[bi];
}
float enemy_sensor_random_free(const Enemy *e)                              /* 0x41d2c0 */
{
    int l[16], n = 0; for (int i = 0; i < 16; i++) if (e->sens.free[i]) l[n++] = i;
    return n ? e->sens.ang[l[crt_mod(n)]] : -1.0f;                             /* 0x41d2ea: no draw when nothing is free */
}
float enemy_sensor_widest_free(const Enemy *e)                              /* 0x41d390: walk both ways from each free one until either side is blocked */
{
    int best = -1, bi = -1;
    for (int i = 0; i < 16; i++) {
        if (!e->sens.free[i]) continue;
        int l = i, r = i, n = -1;
        do { if (--l < 0) l = 16; if (++r >= 16) r = 0; n++; } while (e->sens.free[l] && e->sens.free[r] && r != i);   /* left wraps to the sentinel 16 first */
        if (n > best) { best = n; bi = i; }
    }
    return bi < 0 ? -1.0f : e->sens.ang[bi];
}
/* Chase Steer 0x41bd00: the target angle is the player's direction, or the nearest free one when the sensor has that blocked */
static float chase_target(const Enemy *e, float ang) { return enemy_sensor_free(e, ang) ? ang : enemy_sensor_nearest_free(e, ang); }
/* ---- logical animations of the AnimCtrl +0x1c4 (records {sub[4], prio, speed, restart}; the prio is left out) ----------
 * 0x4b29b8 types 4/5/6, 0x4b26a8 types 7/8/9, 0x4b2190 the ghost. Only the wander actions play them through er_request
 * (the chain rules of 0x436b70 / 0x436a50, as anim_request in player.c); the other states keep their single .ins animation. */
typedef struct { signed char sub[4]; float speed; unsigned char restart; } ERec;
static const ERec g_r4[24] = {
    {{0,0,0,0},3,1}, {{16,0,-1,-1},3,1}, {{17,0,-1,-1},3,1}, {{1,2,2,2},3,1}, {{3,4,5,0},3,1}, {{3,4,5,0},3,1}, {{4,4,5,0},3,0}, {{4,4,4,4},3,0},
    {{13,-1,-1,-1},10,1}, {{15,0,0,0},2,1}, {{14,0,0,0},3,1}, {{18,0,0,0},1.5f,1}, {{11,0,0,0},4,1}, {{12,-1,-1,-1},3,1}, {{6,0,-1,-1},3,1}, {{7,0,-1,-1},3,1},
    {{8,0,-1,-1},3,1}, {{9,0,-1,-1},3,1}, {{10,0,-1,-1},3,1}, {{3,4,4,4},3,1}, {{4,4,4,4},3,1}, {{5,0,-1,-1},3,1}, {{16,0,-1,-1},3,1}, {{17,0,-1,-1},3,1} };
static const ERec g_r7[28] = {
    {{0,0,0,0},3,1}, {{16,16,16,16},3,1}, {{17,17,17,17},3,1}, {{1,2,2,2},3,1}, {{3,4,5,0},3,1}, {{3,4,5,0},3,1}, {{4,4,5,0},3,0}, {{4,4,4,4},3,0},
    {{13,-1,-1,-1},3,1}, {{15,0,0,0},3,1}, {{14,0,0,0},2,1}, {{18,0,0,0},3,1}, {{11,0,0,0},4,1}, {{12,-1,-1,-1},3,1}, {{6,0,-1,-1},2,1}, {{7,0,-1,-1},2,1},
    {{8,0,-1,-1},2,1}, {{9,0,-1,-1},2,1}, {{10,0,-1,-1},2,1}, {{21,-1,-1,-1},3,1}, {{22,0,-1,-1},3,1}, {{19,0,-1,-1},3,1}, {{20,0,-1,-1},3,1}, {{3,4,4,4},3,1},
    {{4,4,4,4},3,1}, {{5,0,-1,-1},3,1}, {{16,0,-1,-1},3,1}, {{17,0,-1,-1},3,1} };
static const ERec g_r13[26] = {
    {{0,0,0,0},3,1}, {{0,0,-1,-1},3,1}, {{0,0,-1,-1},3,1}, {{1,2,2,2},3,1}, {{3,4,5,0},3,1}, {{3,4,5,0},3,1}, {{4,4,5,0},3,0}, {{4,4,4,4},3,0},
    {{13,-1,-1,-1},3,1}, {{15,0,0,0},3,1}, {{14,0,0,0},2,1}, {{16,0,0,0},3,1}, {{11,0,0,0},4,1}, {{12,-1,-1,-1},3,1}, {{6,0,-1,-1},2,1}, {{7,0,-1,-1},2,1},
    {{8,0,-1,-1},2,1}, {{9,0,-1,-1},2,1}, {{10,0,-1,-1},2,1}, {{20,0,-1,-1},3,1}, {{21,0,-1,-1},3,1}, {{3,4,4,4},3,1}, {{4,4,4,4},3,1}, {{5,0,-1,-1},3,1},
    {{0,0,-1,-1},3,1}, {{0,0,-1,-1},3,1} };
static const ERec *er_rec(const Enemy *e, int n)
{
    if (e->type == 13) return n >= 0 && n < 26 ? &g_r13[n] : &g_r13[0];
    if (e->type >= 7 && e->type <= 9) return n >= 0 && n < 28 ? &g_r7[n] : &g_r7[0];
    return n >= 0 && n < 24 ? &g_r4[n] : &g_r4[0];
}
static float er_len(const Enemy *e, int n, int k)                             /* AnimLen 0x436b90 */
{
    const ERec *r = er_rec(e, n); const Model *m = e->inst->model; int s = r->sub[k];
    return s >= 0 && (uint32_t)s < m->nanims ? m->anims[s].duration_s / r->speed : 0.0f;
}
static void er_chain(Enemy *e)                                                 /* the slot step of the clock 0x43eee0 (0x43f0c9) */
{
    Instance *in = e->inst; const ERec *r = er_rec(e, e->lanim); const Model *m = in->model;
    int nx = e->lsub < 3 ? r->sub[e->lsub + 1] : 0;
    if ((uint32_t)in->anim >= m->nanims) return;
    if (e->lsub < 3 && (nx < 0 || (uint32_t)nx >= m->nanims)) {                 /* chain ends: hold the last frame (the clock runs after this) */
        if (in->anim_time >= m->anims[in->anim].duration_s - 0.15f) { in->anim_time = m->anims[in->anim].duration_s * 0.999f; in->anim_speed = 0; }
    } else if (e->lsub < 3 && in->anim_time >= m->anims[in->anim].duration_s) { in->anim_time -= m->anims[in->anim].duration_s; in->anim = nx; e->lsub++; }
}
static void er_request(Enemy *e, int n)                                        /* Request 0x436b70 + Tick 0x436a50 */
{
    Instance *in = e->inst; const ERec *r = er_rec(e, n); const Model *m = in->model;
    if (n != e->lanim) {
        e->lanim = n; e->lsub = 0;
        if (r->sub[0] >= 0 && (uint32_t)r->sub[0] < m->nanims && (r->restart || in->anim != r->sub[0])) { in->anim = r->sub[0]; in->anim_time = 0; }
    }
    in->anim_speed = r->speed;
    er_chain(e);
}
/* right after the clock of `in`, before its event scan and pose (main loop): the chain part the clock just ended moves on, as the
 * slot step inside 0x43eee0 does; only while `in` still shows the part er_request put there */
void enemies_anim_settle(EnemySet *s, const Instance *in)
{
    for (int i = 0; i < s->n; i++) {
        Enemy *e = &s->e[i];
        if (e->inst != in || e->lanim < 0 || e->lsub < 0 || e->lsub > 3) continue;
        if (er_rec(e, e->lanim)->sub[e->lsub] == in->anim) er_chain(e);
        return;
    }
}

/* ---- behaviour Wander (ctor 0x41bf30, vtable 0x4aa118; docs/ENEMY.md 5.4) ------------------------------------------
 * Eight actions with weights (message 11/19..27): 0..4 idle variations, 5 turning in place, 6 walking straight on, 7 walking off
 * in a new free direction; the choice alternates idle and walking. Two more are set by the leash: 10 turning toward home and 9
 * walking back. Every action lasts as long as its animation chain (vtbl[53] of the class). */
static EnemySet *g_eset;                                                        /* for Touch(0, 0) */
/* the Perso's position pointer vtbl[34] 0x44c030, which FindTarget 0x40c0d0 and Touch 0x40c1e0 / 0x411840 measure from: the feet +0x1f4,
 * or the root position +0x544 while +0x550 is set (the climb-over and the scripted actions with root motion, 0x44e290) */
static Vec3 perso_pos(const struct Player *pl) { return pl->use_root ? pl->root_pos : pl->pos; }
static struct Player *g_epl;
static int wander_rec(const Enemy *e, int a)                                     /* the wander branch of vtbl[45] (0x4194f0 / 0x418000 / 0x414540) */
{
    if (a >= 0 && a <= 4) return 14 + a;
    if (a == 5) { int l = ang_diff(e->want_ang, e->ang) > 0; return e->type == 13 ? (l ? 24 : 25) : e->type >= 7 && e->type <= 9 ? (l ? 26 : 27) : (l ? 22 : 23); }
    return a == 6 ? 4 : a == 7 ? 5 : a == 9 ? 6 : a == 10 ? 7 : -1;
}
static float wander_len(const Enemy *e, int a)                                  /* vtbl[53] in the wander states (tables 0x419c4c / 0x4187f0 / 0x414c98) */
{
    if (a >= 0 && a <= 5) return er_len(e, wander_rec(e, a), 0);
    if (a == 6 || a == 7 || a == 9) {
        int n = wander_rec(e, a); float s = er_len(e, n, 0) + er_len(e, n, 1) + er_len(e, n, 2);
        if (a == 9 && e->inst->anim_speed > 0) s -= e->inst->anim_time / e->inst->anim_speed;   /* - inst+0xac / inst+0xa0 */
        return s;
    }
    return 0.0f;
}
static void wander_set(Enemy *e, int a)                                         /* 0x41c2a0: every action is kind 1 (the ctor sets 0x10000) */
{
    e->w_act = a;
    if (a == 10) e->w_dur = e->P.turn > 0 ? fabsf(ang_diff(e->want_ang, e->ang)) / e->P.turn : 0;
    else e->w_dur = wander_len(e, a);
    if (wenv("WOODY_WANDERLOG")) printf("  WANDER %u (type %d st %d) action %d for %.2f s at %.0f %.0f %.0f ang %.0f -> %.0f", e->inst->index, e->type, e->st, a, e->w_dur, e->pos.x, e->pos.y, e->pos.z, e->ang * 57.2958f, e->want_ang * 57.2958f), puts("");
}
static void wander_choose(Enemy *e, int a)                                      /* 0x41c180 */
{
    if (a == -1) {
        unsigned idle = 0, walk = 0, tot; int lo, hi;
        for (int i = 0; i < 8; i++) { if (i <= 5) idle += e->w_weight[i]; else walk += e->w_weight[i]; }
        if (e->w_act > 5) { tot = idle; lo = 0; hi = 4; } else { tot = walk; lo = 6; hi = 7; }   /* after walking an idle (action 5's weight falls to 4), else a walk */
        unsigned r = crt_rand(), acc = 0; r = tot ? r % tot : 0;                 /* 0x41c1dc, always drawn (the original divides by zero when all weights are 0) */
        a = lo; while (a < hi && r - acc >= e->w_weight[a]) { acc += e->w_weight[a]; a++; }
    }
    if (a == 5 || a == 7) { float f = enemy_sensor_random_free(e); e->want_ang = f >= 0 ? f : (float)crt_mod(6283) * 0.001f; }   /* 0x41d2c0, else 0x41ba10 */
    else e->want_ang = e->ang;                                                   /* 0x41b9f0(H.angle, snap): stop turning */
    wander_set(e, a);
}
static void wander_restart(Enemy *e, int a)                                     /* vtbl[6] 0x41c090 (a = -1) / 0x41c0c0(a) */
{
    e->knock_t = 0;                                                             /* both begin with 0x41b2b0: the behaviour's knock timer +0x1c = 0 (the port keeps one timer for all behaviours) */
    e->w_act = -1; e->w_dur = 0; e->w_home_t = 0; e->w_homing = 0; e->w_avoid = NULL; e->w_avoid_t = 0;
    wander_choose(e, a);
}
static void wander_init(Enemy *e)                                               /* ctor 0x41bf30: weights 10 for 0..5, 25 for 6/7, leash on */
{
    for (int i = 0; i < 8; i++) e->w_weight[i] = i < 6 ? 10 : 25;
    e->w_leash = 1; e->w_act = -1;
}
static void wander_blocked(Enemy *e)                                            /* OnBlocked 0x41c420: turn to the widest free gap */
{
    float a = enemy_sensor_widest_free(e); if (a >= 0) e->want_ang = a;
    wander_set(e, 5);
}
/* Tick 0x41c640 without the move: homecoming 0x41c500, the action timer 0x41c370, H.Tick outside the idles. Returns the step length */
static float wander_tick(Enemy *e, float dt)
{
    if (e->w_leash) {
        int go = 1;
        if (e->w_home_t > 0) { e->w_home_t -= dt; if (e->w_home_t > 0) go = 0; }
        if (go) {
            if (e->w_homing) { wander_set(e, 9); e->w_home_t = e->w_dur; e->w_dur += dt; e->w_homing = 0; }
            else {
                float hx = e->home.x - e->pos.x, hy = e->type == 13 ? e->home.y - e->pos.y : 0, hz = e->home.z - e->pos.z;   /* subtype >= 9: 3D */
                if (e->P.leash < sqrtf(hx * hx + hy * hy + hz * hz)) { e->want_ang = atan2f(hz, hx); wander_set(e, 10); e->w_homing = 1; e->w_home_t = e->w_dur; }
            }
        }
    }
    e->w_dur -= dt; if (e->w_dur <= 0 && !e->w_homing) wander_choose(e, -1);
    if (e->w_act < 0 || e->w_act > 4) steer(e, e->want_ang, e->P.turn, dt);
    if (e->w_act == 6 || e->w_act == 7 || e->w_act == 9 || e->w_act == 10) { e->want_speed = e->P.walk; return e->speed * dt; }   /* step 0x41c3a0, byte table 0x41c40c */
    return 0.0f;
}
/* avoidance 0x41c470 after the move: Touch(0, 0) = the first other actor within the sum of the radii (3D) turns it away (action 7) */
static void wander_avoid(Enemy *e, float dt)
{
    const void *o = NULL; Vec3 op = { 0, 0, 0 };
    if (g_epl && !g_epl->dead_kind) { Vec3 p = perso_pos(g_epl); float dx = p.x - e->pos.x, dy = p.y - e->pos.y, dz = p.z - e->pos.z, R = e->P.radius + 69.0f;
        if (dx * dx + dy * dy + dz * dz < R * R) { o = g_epl; op = p; } }
    for (int i = 0; !o && g_eset && i < g_eset->n; i++) {
        Enemy *f = &g_eset->e[i]; if (f == e || f->removed || !f->inst->visible || !f->attackable) continue;
        float dx = f->pos.x - e->pos.x, dy = f->pos.y - e->pos.y, dz = f->pos.z - e->pos.z, R = e->P.radius + f->P.radius;
        if (dx * dx + dy * dy + dz * dz < R * R) { o = f; op = f->pos; }
    }
    e->w_avoid_t -= dt;
    if (o && (o != e->w_avoid || e->w_avoid_t < 0)) {
        e->want_ang = atan2f(e->pos.z - op.z, e->pos.x - op.x); wander_set(e, 7); e->w_avoid = o; e->w_avoid_t = e->w_dur;
    }
}

/* OnBlocked, behaviour vtbl[3], when the common move 0x41b2c0 refuses the step: Wander 0x41c420 turns to the widest free
 * direction (action 5, turning); Chase 0x41be90 to the free direction nearest its current angle, the turn timer to 0; Stand's
 * slot is 0x445840 = ret. With nothing free Wander keeps its target and still turns (action 5). */
static void enemy_blocked(Enemy *e)
{
    if (e->behav == 0) wander_blocked(e);
    else if (e->behav == 1) { e->want_ang = enemy_sensor_nearest_free(e, e->ang); e->turn_t = 0; }
    if (wenv("WOODY_SENSLOG") && e->behav < 2) printf("  SENS %u blocked at %.0f %.0f %.0f ang %.0f -> %.0f (%s)", e->inst->index, e->pos.x, e->pos.y, e->pos.z, e->ang * 57.2958f, e->want_ang * 57.2958f, e->behav ? "chase" : "wander"), puts("");
}

/* platform delta 0x436d20 of the probe +0x178: where the attached point (node space) is now, minus where it was attached */
static Vec3 plat_delta(const Enemy *e)
{
    const Instance *in = e->plat_inst;
    if (!in || !in->node_world || e->plat_node >= in->model->nnodes) return (Vec3){ 0, 0, 0 };
    Vec3 w = mat4_apply(&in->node_world[e->plat_node], e->plat_local);
    return (Vec3){ w.x - e->plat_world.x, w.y - e->plat_world.y, w.z - e->plat_world.z };
}
/* the sweep 0x437580(res, from, to, up, 30.0) (docs/BOSS14.md 5.1): a SPHERE of radius P+4 ([0x4b3118], 0x41b489) whose centre sits
 * r + up + 1 above the feet moves in floor(|to - from| / 30) + 1 equal substeps; after each one the push-out 0x407340 (world
 * polygons + instance press nodes, vt[9] 0x433ff0) moves it in x and z only when it touched something ([0x4c4bd0]), and
 * GetHeight 0x435650(&c, -1, 1) under the centre lifts it so the feet never end below the floor (nothing found: y = c.y, normal
 * (0, 1, 0), which lifts the sphere by h). *gy / *ny = [0x53a568] / [0x4b310c] of the LAST substep. With up = min(P+0x30, h/2)
 * the ordinary enemies (30 / 10) sweep a sphere spanning feet + 11 .. feet + 71, the thrower (60 / 10) + 11 .. + 131. */
static Vec3 enemy_sweep(Enemy *e, struct Player *pl, Vec3 from, Vec3 to, float up, float *gy, float *ny)
{
    float r = e->P.radius, h = r + up + 1.0f;                                  /* 0x437583..0x437598 */
    Vec3 d = { to.x - from.x, to.y - from.y, to.z - from.z };
    float k = floorf(sqrtf(d.x * d.x + d.y * d.y + d.z * d.z) / 30.0f) + 1.0f; int n = (int)(k + 0.5f);   /* 0x4375fb floor, 0x437611 _ftol; d /= k (the float) */
    d.x /= k; d.y /= k; d.z /= k;
    Vec3 c = { from.x, from.y + h, from.z }, res = from;
    for (; n > 0; n--) {
        c.x += d.x; c.y += d.y; c.z += d.z;
        Vec3 push = player_sphere_push(pl, e->inst, c, r); c.x += push.x; c.z += push.z;   /* 0x437679, 0x43768a: x and z of the push */
        int found; Vec3 gn; float g = player_ground_query_n(pl, e->inst, c, &found, &gn);
        if (c.y - h < g) c.y = g + h;                                           /* 0x4376bf */
        res = (Vec3){ c.x, c.y - h, c.z }; *gy = g; *ny = gn.y;
    }
    return res;
}
/* the common move 0x41b2c0, the Tick of Stand (0x41bed0 = jmp) and the tail of the Wander / Chase Ticks, so it runs EVERY frame under
 * those three behaviours, with a zero step too (only the path follower 0x41cf00 slides without it):
 *   from = pos; up = min(P+0x30, h/2) (0x41b33b); knockT (Behav+0x1c) > 0 ? knockT = max(0, knockT - dt), to = from + knockDir
 *   * dt * P+0x44 * knockT : to = from + vtbl[1]() * vtbl[0]() (direction * step); to += the platform delta 0x436d20; the
 *   Probe2 push-out 0x437040 (dead: its sphere queries 0x435b60 are a stub, zero); Sweep; hook vtbl[2](res, from, step) (only
 *   Chase has one, 0x41bdf0: moved less than 0.01 (3D) with a step > 0 => from.xz += (rand() & 31) - 16 each on its local copy
 *   and the target angle from that point to res - only a new heading, the position is not moved); subtype >= 9 (types
 *   13..16) res.y = from.y; 0x436d10 clears the probe's platform; free when the
 *   floor normal y >= 0.8 (0x4a987c, NaN = blocked) and res.y - floor < P+0x2c (NaN = free): pos = res, collision centre, re-cell
 *   (with a drop < 1 on an instance floor it attaches the probe to it, 0x41b566 - overwritten by the ground follower's probe
 *   0x436dc0 of the same Update, which attaches or clears every frame; not ported); blocked: pos += platform delta, re-attach
 *   when the probe still has a platform (never: 0x436d10 just cleared it), re-cell, OnBlocked vtbl[3]. */
int enemy_common_move(Enemy *e, struct Player *pl, Vec3 delta, float *knock_t, Vec3 kd, int behav, float dt)
{
    Vec3 from = e->pos, to = from; float h2 = e->P.height * 0.5f, up = e->P.rise < h2 ? e->P.rise : h2, step;
    if (!(*knock_t > 0)) { step = sqrtf(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z); to.x += delta.x; to.y += delta.y; to.z += delta.z; }
    else { *knock_t -= dt; if (*knock_t < 0) *knock_t = 0; step = dt * E_KNOCK * *knock_t; to.x += kd.x * step; to.y += kd.y * step; to.z += kd.z * step; }   /* 0x41b3c0 */
    Vec3 pd = plat_delta(e); to.x += pd.x; to.y += pd.y; to.z += pd.z;          /* 0x41b453 */
    float gy = from.y, ny = 1.0f;
    Vec3 res = enemy_sweep(e, pl, from, to, up, &gy, &ny);                     /* 0x41b4d8 */
    if (behav == 1) {                                                           /* Chase hook 0x41bdf0 */
        float mx = res.x - from.x, my = res.y - from.y, mz = res.z - from.z;
        if (sqrtf(mx * mx + my * my + mz * mz) < 0.01f && step > 0) {          /* the ±16 goes onto the hook's copy of FROM (0x41be56 / 0x41be74), */
            float jx = (float)((int)(crt_rand() & 31) - 16), jz = (float)((int)(crt_rand() & 31) - 16);   /* which 0x41b2c0 never stores: pos = res (0x41b586) */
            e->want_ang = atan2f(res.z - (from.z + jz), res.x - (from.x + jx)); /* 0x41ba60(from', res, 0): the target angle from the jittered point to res */
        }
    }
    if (e->type >= 13) res.y = from.y;                                          /* 0x41b4fe: subtypes 9 (ghost), 11..13 (bosses) */
    e->plat_inst = NULL;                                                        /* 0x41b514 */
    float drop = res.y - gy;
    int ok = ny >= 0.8f && !(drop >= e->P.drop);                               /* 0x41b519..0x41b54a */
    if (wenv("WOODY_SWEEPLOG") && (!ok || fabsf(res.x - to.x) + fabsf(res.z - to.z) > 0.5f))
        printf("  SWEEP %u (type %d st %d) %s at %.0f %.0f %.0f: wanted %.1f %.1f got %.1f %.1f, floor %.1f below, normal y %.2f", e->inst->index, e->type, e->st, ok ? "pushed" : "blocked", from.x, from.y, from.z, to.x - from.x, to.z - from.z, res.x - from.x, res.z - from.z, drop, ny), puts("");
    if (ok) { e->pos = res; return 1; }
    e->pos.x += pd.x; e->pos.y += pd.y; e->pos.z += pd.z;                       /* 0x41b601 */
    return 0;
}
static void enemy_move(Enemy *e, struct Player *pl, Vec3 delta, float dt)
{
    if (e->behav == 3) return;                                                  /* the path follower 0x41cf00 slides on its own (state 0), without the sweep (docs/ENEMY.md 5.5) */
    if (!enemy_common_move(e, pl, delta, &e->knock_t, e->knock_dir, e->behav, dt)) enemy_blocked(e);
}

void enemy_place(Enemy *e)
{
    Instance *in = e->inst;
    /* model faces along (cos, 0, sin) of ang; same construction as the player: yaw about y composed with rotx(-90) */
    float yaw = atan2f(cosf(e->ang), sinf(e->ang)), c = cosf(yaw * 0.5f), s = sinf(yaw * 0.5f);
    in->position = e->pos; in->cell_dy = e->P.height * 0.5f;              /* 0x4077f0(colCenter = pos + h/2): the cell point for the instance list */
    in->quat.x = 0.70710678f * c; in->quat.y = -0.70710678f * s; in->quat.z = -0.70710678f * s; in->quat.w = -0.70710678f * c;
    mat4_from_trs(&in->world, in->position, in->quat, in->scale);
}

/* Enemy::Reset 0x41a1a0 (subtypes < 9): feet onto the ground under pos + h/2, flag 1 (on the ground). Without it the first
 * step of an enemy placed a little above the floor is refused (more than P+0x2c = 10 to the ground). */
static void ground_snap(Enemy *e, struct Player *pl)
{
    if (!e->need_snap) return;
    e->need_snap = 0; int found; float gy = player_ground_query(pl, e->inst, (Vec3){ e->pos.x, e->pos.y + e->P.height * 0.5f, e->pos.z }, &found);
    if (found) { e->pos.y = gy; e->vfall = 0; }
    else printf("An ennemy (Id=%x) is outside of the world, please check this !!!\n", e->inst->id);   /* 0x41a213 */
}

/* Probe_Test 0x436dc0 on the probe +0x178 with the enemy's own id (docs/EVENTS.md 3.2): GetHeight under pt, "on the ground"
 * when pt.y - (ground + tol) < 1 (0x4a900c); standing on a world_collision press node sends Press / In, leaving it UnPress.
 * Callers: 0x41a4e0 (ground following), 0x414f10 (ghost height), 0x410900 (Buzz), Reset 0x41a010; all with pt = pos + h/2,
 * tol = h/2. 0x416a10 (type 10) and 0x41b030 (vtbl[56]: no call site through +0xe0) are not used by a shipped enemy. */
int enemy_probe(Enemy *e, struct Player *pl, Vec3 pt, float tol, float *gy, int *found)
{
    uint32_t col = 0xffffffffu, node = 0; const Instance *hi = NULL;
    *gy = player_ground_query_hit(pl, e->inst, pt, found, &col, &hi, &node);
    int on = *found && pt.y - (*gy + tol) < 1.0f;
    /* on an instance press node (hit type 2) 0x436d80 attaches the probe to it at pt (0x431700: pt in node space), anything else
     * clears it (0x436ea2 / 0x436ed0); the next common move adds that point's displacement (platform carry) */
    e->plat_inst = NULL;
    if (on && hi && player_node_local(hi, node, pt, &e->plat_local)) { e->plat_inst = hi; e->plat_node = node; e->plat_world = pt; }
    game_col_probe(&e->col_cur, on, col, e->inst);
    return on;
}
void enemy_reset_probe(Enemy *e)                                  /* 0x41a104..0x41a148: after the ground snap, at the start position */
{
    if (!g_epl) return;                                           /* no update has run yet (the level geometry comes with the player): the first ground follow presses */
    float h2 = e->P.height * 0.5f, gy; int found;
    enemy_probe(e, g_epl, (Vec3){ e->pos.x, e->pos.y + h2, e->pos.z }, h2, &gy, &found);
}
/* Enemy::HandleMsg 0x41abfd: message 6 with 0 on an enemy that is in the world (cell >= 0) -> UnPress of the probe's
 * collision (0x41ac11) and out of the world (0x407850, left to inst_msg) */
void enemies_msg6_off(EnemySet *s, Instance *inst)
{
    for (int i = 0; i < s->n; i++) {
        Enemy *e = &s->e[i]; if (e->inst != inst || !inst->visible) continue;
        game_col_probe(&e->col_cur, 0, 0xffffffffu, inst);
    }
}

/* FindTarget 0x41af80 -> 0x40c0d0: the player within P+0x20 (3D) and |dy| < P+0x24 of the centre -- the enemy itself, or its
 * home point +0x134 when flag 2 (message 11/6, "guard") is set; no view cone, no line of sight */
static int find_target(const Enemy *e, const struct Player *pl)
{
    Vec3 c = e->guard ? e->home : e->pos, tp = perso_pos(pl); float dx = tp.x - c.x, dy = tp.y - c.y, dz = tp.z - c.z;
    return !pl->dead_kind && dx * dx + dy * dy + dz * dz < e->P.see * e->P.see && fabsf(dy) < e->P.dy;
}
/* speed channel of H (Tick 0x41ba90): moves linearly toward the target speed at 1000 u/s^2, both ways */
static void speed_tick(Enemy *e, float dt)
{
    if (e->speed < e->want_speed) { e->speed += E_ACC * dt; if (e->speed > e->want_speed) e->speed = e->want_speed; }
    else if (e->speed > e->want_speed) { e->speed -= E_ACC * dt; if (e->speed < e->want_speed) e->speed = e->want_speed; }
}

static void enemy_update(Enemy *e, struct Player *pl, Vec3 cam, float dt)
{
    Instance *in = e->inst;
    if (e->removed || !in->visible || !game_enemy_thinks(in)) return;      /* Think 0x41a320 only runs from 0x42b400, for the instances of this frame's list world+0x64 */
    ground_snap(e, pl);
    { float dx = e->pos.x - cam.x, dy = e->pos.y - cam.y, dz = e->pos.z - cam.z; if (dx * dx + dy * dy + dz * dz >= e->P.active_d * e->P.active_d && e->st != 12) return; }   /* Think 0x41a320 */
    if (e->cool >= 0) e->cool -= dt;
    if (e->hit_t > 0) e->hit_t -= dt;
    if (e->reload >= 0) e->reload -= dt;
    if (e->type == 13 && e->st != 12) in->fade = 0.5f;            /* 0x413ab0: fade target 0.5 every frame = half transparent */
    Vec3 tp = perso_pos(pl); float dx = tp.x - e->pos.x, dy = tp.y - e->pos.y, dz = tp.z - e->pos.z;
    int see = find_target(e, pl);
    float dxz = sqrtf(dx * dx + dz * dz), to_player = atan2f(dz, dx);
    Vec3 step = { 0, 0, 0 }; int anim = EA_IDLE;

    speed_tick(e, dt);

    if (e->behav == 0 || e->behav == 1) enemy_sensor_tick(e, pl);   /* Enemy::Update 0x41a3e0: only under Wander / Chase (kind 0 / 1) */
    switch (e->st) {
    case 0: {                                                     /* patrol: slides exactly along the TRAJ segments, ping-pong on an open path */
        const Trajectory *T = &in->traj; Vec3 b = T->points[e->path_to];
        float bx = b.x - e->pos.x, bz = b.z - e->pos.z, l = sqrtf(bx * bx + bz * bz), mv = e->P.walk * dt;
        if (l <= mv) {
            e->pos.x = b.x; e->pos.z = b.z;
            int nx = (int)e->path_to + e->path_dir;
            if (nx < 0 || nx >= (int)T->npoints) { if (T->closed) nx = nx < 0 ? (int)T->npoints - 1 : 0; else { e->path_dir = -e->path_dir; nx = (int)e->path_to + e->path_dir; } }
            e->path_to = (uint32_t)nx;
        } else { e->ang = atan2f(bz, bx); e->pos.x += bx / l * mv; e->pos.z += bz / l * mv; }
        anim = EA_WALK;
        if (e->cool < 0 && see) { e->home = e->pos; e->st = 2; }
        break; }
    case 7:                                                       /* to wander 0x418d77 (ghost: state 4, 0x413b6b): speed P+0x08 direct, Wander restarts */
        e->speed = e->want_speed = e->P.walk; wander_restart(e, -1); e->behav = 0; e->st = 8; anim = -2;
        break;
    case 8: {                                                     /* wander 0x418dc6 (ghost 5): the behaviour's actions (wander_tick) */
        float l = wander_tick(e, dt); anim = -2;
        step = (Vec3){ cosf(e->ang) * l, 0, sinf(e->ang) * l };
        if (e->cool < 0 && see) e->st = 2;
        if (in->traj.npoints > 1) { float hx = e->home.x - e->pos.x, hz = e->home.z - e->pos.z; if (hx * hx + hz * hz < 10.0f * 10.0f) { e->behav = 3; e->st = 0; } }   /* back on the path (0x41cfb0); no enemy of the 28 levels has a TRAJ */
        break; }
    case 2:                                                       /* noticed: turn on the spot first, then run */
        if (!see) { e->st = 7; break; }
        e->want_ang = chase_target(e, to_player);                     /* 0x41bc80 -> Steer 0x41bd00 */
        e->turn_t = fabsf(ang_diff(to_player, e->ang)) / e->P.turn_fast; e->speed = e->want_speed = e->P.run; e->behav = 1; e->st = 1; if (e->type == 13) e->reload = e->P.reload;
        e->react_t = game_enemy_delay();                              /* PORT EXTRA: it chases at once but holds its attack (0 = the original) */
        /* fallthrough */
    case 1:
        if (!see) { e->st = 7; break; }
        steer(e, e->want_ang, e->P.turn_fast, dt); e->want_ang = chase_target(e, to_player);   /* Steer 0x41bd00: H.Tick toward the last target, then the new one */
        if ((e->turn_t -= dt) <= 0) { step = (Vec3){ cosf(e->ang) * e->speed * dt, 0, sinf(e->ang) * e->speed * dt }; anim = EA_RUN; } else anim = EA_TURN;
        e->atk_t = ea_len(e, EA_DASH) + 0.5f * ea_len(e, EA_MISS);
        if (e->react_t > 0) { e->react_t -= dt; break; }              /* PORT EXTRA (Gameplay page): no dash, dive or shot yet */
        if (e->type == 13) {                                      /* ghost 0x413d0d: dives within 300 (xz) when it moves at the player (3D dot), else fires while chasing */
            float d3 = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dxz <= e->P.melee && d3 > 1e-3f && (cosf(e->ang) * dx + sinf(e->ang) * dz) / d3 > 0.95f) { e->speed = e->want_speed = e->P.dash; e->st = 4; break; }
            if (e->reload > 0) break;
            const Model *mo = in->model; uint32_t want = e->hand == 1 ? 0 : 1, seen = 0; Vec3 m0 = { e->pos.x, e->pos.y + e->P.height * 0.6f, e->pos.z };
            for (uint32_t i = 0; i < mo->nnodes; i++) if (mo->nodes[i].kind == 0x20 && mo->nodes[i].type_code == 1 && mo->nodes[i].npoints >= 1) { if (seen++ == want) { m0 = ins_point_world(in, mo->nodes[i].point_base); break; } }
            Vec3 sd = { tp.x - m0.x, tp.y - m0.y, tp.z - m0.z }; float sl = sqrtf(sd.x * sd.x + sd.y * sd.y + sd.z * sd.z);
            float rt; if (sl > 1e-3f && !player_ray_full(pl, (Vec3){ e->pos.x, e->pos.y + e->P.height * 0.5f, e->pos.z }, m0, &rt))   /* 0x497ed0: world and press nodes */
                game_enemy_shot(e, m0, (Vec3){ sd.x / sl, sd.y / sl, sd.z / sl }, 1000.0f, e->P.shot_dmg, 0, e->P.shot_visual, e->P.shot_fx);   /* straight fireball at the feet (0x414d10) */
            e->reload += e->P.reload; e->hand ^= 1;
            break;
        }
        /* 0x41901b: xz distance <= P+0x5c * T, then 0x440160: H direction (cos, 0, sin) . the 3D-normalised (player - pos)
         * (0x440040 + 0x418fc8) > 0.95, so a player high above (after an air peck's recoil) is not charged */
        { float d3 = sqrtf(dx * dx + dy * dy + dz * dz);
          if (dxz <= e->P.dash * e->atk_t && d3 > 1e-3f && (cosf(e->ang) * dx + sinf(e->ang) * dz) / d3 > 0.95f) { e->speed = e->want_speed = e->P.dash; e->st = 4; } }
        break;
    case 4:                                                       /* dash: the only state that hurts the player */
        anim = EA_DASH;
        if (!see || e->atk_t <= 0) { e->t = ea_len(e, EA_BRAKE); e->behav = 2; e->st = 6; break; }   /* 5 -> 6 (ghost 13 -> 8): Stand */
        e->atk_t -= dt; steer(e, e->want_ang, e->P.turn_fast, dt); e->want_ang = chase_target(e, to_player);
        step = (Vec3){ cosf(e->ang) * e->speed * dt, 0, sinf(e->ang) * e->speed * dt };
        if (sqrtf(dx * dx + dy * dy + dz * dz) < e->P.radius + 69.0f) {
            Vec3 d = dxz > 1e-3f ? (Vec3){ dx / dxz, 0, dz / dxz } : (Vec3){ 1, 0, 0 };
            if (player_hit(pl, e->P.bite, d)) { player_kill(pl, 3); e->t = ea_len(e, EA_WIN); e->want_speed = e->P.walk; e->behav = 2; e->st = 11; }   /* 10 -> 11: Stand */
            else { e->t = ea_len(e, EA_MISS); e->behav = 2; e->st = 3; }   /* Stand */
        }
        break;
    case 3: anim = EA_MISS; if ((e->t -= dt) <= 0) { e->cool = e->P.cool; e->st = 7; } break;
    case 6: anim = EA_BRAKE; if ((e->t -= dt) <= 0) { if (e->type != 13) e->cool = e->P.cool; e->st = 7; } break;
    case 11: anim = EA_WIN; if ((e->t -= dt) <= 0) e->st = 7; break;
    case 9:
        anim = EA_HIT;
        if (e->hp <= 0) { game_enemy_stars(e); e->attackable = 0; e->dead_t = e->type == 13 ? (ea_len(e, EA_DEAD) + 1.0f) * 0.5f : 0; e->behav = 2; e->st = 12; }   /* the ghost starts fading at once; the behaviour stays through state 9 */
        else if (e->hit_t <= 0) { e->st = 7; if (e->type == 13) e->reload = 2.0f * e->P.reload; }
        break;
    case 12: {                                                    /* dead: animation 13, fades out during the second half, then removed */
        anim = EA_DEAD; e->dead_t += dt; float L = ea_len(e, EA_DEAD) + 1.0f;
        if (e->dead_t > L * 0.5f) { in->fade = (e->dead_t - L * 0.5f) / (L * 0.5f); if (in->fade > 1) in->fade = 1; }
        if (e->dead_t >= L) { e->removed = 1; in->visible = 0; g_eset->killed++; return; }   /* +0x10c |= 1 -> 0x40bf60 -> vtbl[29] 0x41aff0: [0x4c532c]++ (the defeated-enemies stat) */
        break; }
    }
    enemy_move(e, pl, step, dt);                                  /* the behaviour tick's common move 0x41b2c0, every frame (knockback: v = 600 * t_rest for 0.25 s) */
    if (e->st == 8) wander_avoid(e, dt);
    if (e->type == 13) {                                          /* height control 0x414f10: feet at the player's feet height (home without a target); frozen when hit / dead */
        if (e->st != 9 && e->st != 12) {
            int found; float gy, h2 = e->P.height * 0.5f; int on = enemy_probe(e, pl, (Vec3){ e->pos.x, e->pos.y + h2, e->pos.z }, h2, &gy, &found);   /* 0x414f8d */
            float want = see ? tp.y - e->pos.y : e->home.y - e->pos.y, stp = see ? e->P.run * dt : e->P.walk * dt;
            if (see && want > 0 && (pl->jumper.state == 0 || pl->jumper.state == 1 || pl->jumper.state == 7)) stp *= 0.2f;
            if (want < -0.01f) { e->pos.y += want < -stp ? -stp : want; if (found && e->pos.y < gy) { e->pos.y = gy; on = 1; } }
            else if (want > 0.01f) { float up = want > stp ? stp : want, t; if (!player_ray_full(pl, (Vec3){ e->pos.x, e->pos.y + e->P.height, e->pos.z }, (Vec3){ e->pos.x, e->pos.y + e->P.height + up, e->pos.z }, &t)) e->pos.y += up; }   /* 0x4359b0: world and press nodes */
            game_msgmask(in, 0x200, on);                              /* 0x41514d / 0x415169: +0x174 bit 0 (hit or clamped); frozen with it while hit / dead */
        }
    } else
    /* ground following 0x41a4e0: v += 200*dt - 0.2*v per frame, y -= v, never below the ground */
    { int found; float gy, h2 = e->P.height * 0.5f; int on = enemy_probe(e, pl, (Vec3){ e->pos.x, e->pos.y + h2, e->pos.z }, h2, &gy, &found);   /* 0x41a561 */
      e->vfall += 200.0f * dt - 0.2f * e->vfall; e->pos.y -= e->vfall;
      if (found && e->pos.y <= gy) { e->pos.y = gy; e->vfall = 0; on = 1; }
      game_msgmask(in, 0x200, on); }                                /* 0x41a642 / 0x41a65e */
    if (anim == -2) { int r = wander_rec(e, e->w_act); if (r >= 0) er_request(e, r); } else ea_play(e, anim);
    enemy_place(e);
}

/* ---- shooters, types 7/8/9 (docs/ENEMY.md 8): state machine 0x416fb0, animation table 0x4b26a8 -------------------- */
enum { S_PATH = 0, S_BITE = 1, S_TOWANDER = 2, S_WANDER = 3, S_HIT = 4, S_DASH0 = 5, S_DASH = 6, S_BRAKE = 7, S_WIN = 9, S_DEAD = 10, S_WAIT = 11, S_AIM = 12, S_FIRE = 13, S_DODGE0 = 15, S_DODGE = 16 };
/* ---- the AnimCtrl +0x1c4 of types 7/8/9 (base 0x4369f0, vtable 0x4a9eac; records 0x4b26a8 = g_r7, docs/ENEMY.md 8.6) and the
 * instance clock 0x43eee0 it drives (docs/INSTANCE.md 1.2). Every frame the class asks for one record (vtbl[45] 0x418000, from the
 * state of the previous frame); Tick picks the queued record with the highest priority and only lets it replace a running record of
 * higher priority while the instance loops (+0xc0: slot0 == slot1). So the throw {22, 0} (1000) ends on the held last frame of anim 0
 * and the turn records 1 / 2 (900) of the wait state do not show until the next windup (1000). */
static const short g_r7_prio[28] = { 1000, 900, 900, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1500, 1000, 1000, 2000,
                                     1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000 };
static float g_r7_speed[28];                                                     /* the record speeds: 0x436bd0 rewrites them in the (global) table */
static double g_enow;                                                            /* the clock [0x509adc]+0x30 of the instance clocks */
static float sac_dur(const Instance *in, int s) { const Model *m = in->model; return s >= 0 && (uint32_t)s < m->nanims && m->anims[s].duration_s > 0 ? m->anims[s].duration_s : 1.0f; }
static float sac_speed(int n) { if (g_r7_speed[0] == 0) for (int i = 0; i < 28; i++) g_r7_speed[i] = g_r7[i].speed; return g_r7_speed[n]; }
static float sac_len(const Enemy *e, int n) { int s = g_r7[n].sub[0]; return (uint32_t)s < e->inst->model->nanims ? sac_dur(e->inst, s) / sac_speed(n) : 0; }   /* AnimLen 0x436b90(n, 0) */
static void sac_set_len(const Enemy *e, int n, float T)                            /* 0x436bd0(n, T, 1): the first animation of the chain lasts T */
{
    sac_speed(n); int s = g_r7[n].sub[0];
    if (T > 1e-4f && (uint32_t)s < e->inst->model->nanims) g_r7_speed[n] = sac_dur(e->inst, s) / T;   /* T = 0 (no turn left) would give an infinite speed: keep the old one */
}
static void sac_reset(Enemy *e) { e->ac_nq = 0; e->ac_cur = -1; }                /* 0x436a40 */
static void sac_init(Enemy *e)                                                   /* instance ctor 0x42e250: slot0 = 0, speed 0, +0xc0 = -1 */
{
    sac_reset(e); e->ac_slot[0] = 0; e->ac_slot[1] = e->ac_slot[2] = e->ac_slot[3] = -1; e->ac_c0 = -1; e->ac_speed = e->ac_base = e->ac_pos = 0; e->ac_start = (float)g_enow;
}
static void sac_request(Enemy *e, int n) { if (n >= 0 && n < 28 && e->ac_nq < 16) e->ac_q[e->ac_nq++] = n; }   /* 0x436b70 */
static void sac_tick(Enemy *e)                                                   /* 0x436a50 */
{
    int best = -1, bp = 0;
    while (e->ac_nq) { int r = e->ac_q[--e->ac_nq]; if (g_r7_prio[r] > bp) { bp = g_r7_prio[r]; best = r; } }   /* from the back: the last of equals wins */
    if (best != -1 && best != e->ac_cur && (e->ac_cur == -1 || g_r7_prio[e->ac_cur] <= bp || e->ac_c0 == 1)) {
        const ERec *r = &g_r7[best]; int na = (int)e->inst->model->nanims;
        e->ac_cur = best;
        if (r->restart) e->ac_start = (float)g_enow;                             /* inst+0xa8 = now */
        for (int k = 0; k < 4; k++) e->ac_slot[k] = r->sub[k] < na ? r->sub[k] : 0;   /* -1 stays -1 (signed compare) */
    }
    if (e->ac_cur >= 0) e->ac_speed = e->ac_base = sac_speed(e->ac_cur);        /* 0x42e290 every tick */
}
static void sac_clock(Enemy *e)                                                  /* 0x43eee0, then the pose the renderer reads */
{
    Instance *in = e->inst; if ((uint32_t)e->ac_slot[0] >= in->model->nanims) return;
    float L = sac_dur(in, e->ac_slot[0]), ph, now = (float)g_enow;
    if (e->ac_speed != 0) {
        ph = e->ac_speed > 0 ? (now - e->ac_start) / (L / e->ac_speed) : 1.0f - (now - e->ac_start) / -(L / e->ac_speed);
        if (!(ph >= 0 && ph < 1)) {
            if (e->ac_slot[1] != -1) {                                           /* on to the next animation of the chain */
                float n = 0, ratio = L / sac_dur(in, e->ac_slot[1]);
                if (ph < 0) { n = -floorf(ph); ph += n; }
                if (ph >= 1) { n = floorf(ph); ph -= n; }
                ph *= ratio;
                if (n != 0) e->ac_start += n * (L / fabsf(e->ac_speed));
                e->ac_speed = e->ac_base;
                e->ac_slot[0] = e->ac_slot[1]; e->ac_slot[1] = e->ac_slot[2]; e->ac_slot[2] = e->ac_slot[3];
            } else if (e->ac_speed < 0) { if (ph < 0) { e->ac_speed = e->ac_base = 0; ph = 0; } }
            else if (ph > 1) { e->ac_speed = e->ac_base = 0; ph = 1; }           /* one shot: the last frame stays */
        }
        e->ac_pos = sac_dur(in, e->ac_slot[0]) * ph;
    } else ph = e->ac_pos / L;
    e->ac_c0 = e->ac_slot[0] == e->ac_slot[1];
    L = sac_dur(in, e->ac_slot[0]);
    in->anim = e->ac_slot[0]; in->anim_speed = 0; in->anim_time = (ph < 0.9999f ? (ph > 0 ? ph : 0) : 0.9999f) * L;   /* short of L: anim_sounds would read a wrap */
}
static int sac_state_rec(const Enemy *e)                                         /* vtbl[45] 0x418000 (jump table 0x418184) */
{
    switch (e->st) {
    case 0: return 24;                                                           /* path 0x4182e0: the walk loop (no shooter has a TRAJ) */
    case 1: return 10;  case 2: case 3: return wander_rec(e, e->w_act);        /* wander 0x4181e0; action 8: nothing */
    case 4: return 12;  case 5: case 6: return 8;  case 7: case 14: return 9;  case 8: case 9: return 11;  case 10: return 13;
    case 11: return ang_diff(e->want_ang, e->ang) > 0 ? 1 : 2;                  /* H+0x0c > 0 (0x41b900) */
    case 12: return 0x13;  case 13: return 0x14;  case 15: return 0;  case 16: return 0x15;
    default: return -1;
    }
}
static int shooter_vector(const Instance *in, uint32_t tc, Vec3 *p0)      /* 0x42f6b0: start of the first marker with this typecode */
{
    const Model *mo = in->model;
    for (uint32_t i = 0; i < mo->nnodes; i++) if (mo->nodes[i].kind == 0x20 && mo->nodes[i].type_code == tc && mo->nodes[i].npoints >= 1) { *p0 = ins_point_world(in, mo->nodes[i].point_base); return 1; }
    return 0;
}
/* PORT EXTRA (Gameplay page): a shooter that just noticed Woody reloads for the chosen delay first; the original fires at once
 * (Reset zeroes the reload timer, docs/ENEMY.md) */
static void shooter_react(Enemy *e) { float d = game_enemy_delay(); if (e->reload < d) e->reload = d; }
static void shooter_update(Enemy *e, struct Player *pl, Vec3 cam, float dt)
{
    Instance *in = e->inst;
    if (e->removed || !in->visible || !game_enemy_thinks(in)) return;      /* Think 0x41a320 only runs from 0x42b400, for the instances of this frame's list world+0x64 */
    ground_snap(e, pl);
    { float dx = e->pos.x - cam.x, dy = e->pos.y - cam.y, dz = e->pos.z - cam.z; if (dx * dx + dy * dy + dz * dz >= e->P.active_d * e->P.active_d && e->st != S_DEAD) { sac_clock(e); return; } }
    sac_request(e, sac_state_rec(e)); sac_tick(e);                /* Enemy::Update: vtbl[45] + Tick, on the state of the previous frame */
    if (e->cool >= 0) e->cool -= dt;
    if (e->hit_t > 0) e->hit_t -= dt;
    if (e->reload >= 0) e->reload -= dt;
    Vec3 tp = perso_pos(pl); float dx = tp.x - e->pos.x, dy = tp.y - e->pos.y, dz = tp.z - e->pos.z;
    int see = find_target(e, pl);
    float dxz = sqrtf(dx * dx + dz * dz), to_player = atan2f(dz, dx);
    Vec3 step = { 0, 0, 0 };

    speed_tick(e, dt);

    if (e->behav == 0 || e->behav == 1) enemy_sensor_tick(e, pl);   /* Enemy::Update: under Wander (2, 3, 16 -> 3) / Chase (5, 6) */
    switch (e->st) {
    case S_PATH: {
        const Trajectory *T = &in->traj; Vec3 b = T->points[e->path_to];
        float bx = b.x - e->pos.x, bz = b.z - e->pos.z, l = sqrtf(bx * bx + bz * bz), mv = e->P.walk * dt;
        if (l <= mv) {
            e->pos.x = b.x; e->pos.z = b.z;
            int nx = (int)e->path_to + e->path_dir;
            if (nx < 0 || nx >= (int)T->npoints) { if (T->closed) nx = nx < 0 ? (int)T->npoints - 1 : 0; else { e->path_dir = -e->path_dir; nx = (int)e->path_to + e->path_dir; } }
            e->path_to = (uint32_t)nx;
        } else { e->ang = atan2f(bz, bx); e->pos.x += bx / l * mv; e->pos.z += bz / l * mv; }
        if (e->cool < 0 && see) { e->home = e->pos; e->behav = 2; e->st = S_WAIT; shooter_react(e); }
        break; }
    case S_TOWANDER:                                              /* 0x417057: speed P+0x08 direct, turn speed P+0x10, Wander restarts */
        e->speed = e->want_speed = e->P.walk; wander_restart(e, -1); e->behav = 0; e->st = S_WANDER;
        break;
    case S_WANDER: {                                              /* 0x4170a3 */
        float l = wander_tick(e, dt);
        step = (Vec3){ cosf(e->ang) * l, 0, sinf(e->ang) * l };
        if (e->cool < 0 && see) { e->st = S_WAIT; shooter_react(e); }
        if (in->traj.npoints > 1) { float hx = e->home.x - e->pos.x, hz = e->home.z - e->pos.z; if (hx * hx + hz * hz < 10.0f * 10.0f) { e->behav = 3; e->st = S_PATH; } }   /* even when 11 was just set */
        break; }
    case S_WAIT: {                                                /* reloading: stands still and does NOT turn (H.Tick is not called in 0x4171bd) */
        if (!see) { e->st = S_TOWANDER; break; }
        e->behav = 2;                                             /* 0x4171bd: Stand with Stand+0x38 = 0 */
        e->want_ang = to_player;                                  /* 0x41ba60(own, tgt, 0): the target angle, no snap */
        { float T = 4.0f * fabsf(ang_diff(to_player, e->ang)) / e->P.turn_fast; sac_set_len(e, 2, T); sac_set_len(e, 1, T); }   /* +0x1e0: the turn records last as long as the turn would */
        if (e->reload <= 0) { e->t = sac_len(e, 0x13); e->st = S_AIM; }
        break; }
    case S_AIM:                                                   /* windup: the only place it turns, at 4 * P+0x14 */
        if (!see) { e->st = S_TOWANDER; break; }
        if (dxz <= e->P.melee) { e->st = S_DASH0; break; }
        if (e->t > 0) e->t -= dt; else { if (e->type == 9) e->t = 0.1f; e->st = S_FIRE; }
        steer(e, to_player, 4.0f * e->P.turn_fast, dt);
        break;
    case S_FIRE: {
        if (!see) { e->st = S_TOWANDER; break; }
        Vec3 m0; float rt;
        if (!shooter_vector(in, 1, &m0)) { m0 = (Vec3){ e->pos.x + cosf(e->ang) * e->P.radius, e->pos.y + e->P.height * 0.7f, e->pos.z + sinf(e->ang) * e->P.radius }; }
        if (e->type == 9 && (e->t -= dt) > 0) break;
        /* 0x497ed0: from the own centre to the own muzzle; blocked = the shot is skipped but the reload still counts */
        if (!player_ray_full(pl, (Vec3){ e->pos.x, e->pos.y + e->P.height * 0.5f, e->pos.z }, m0, &rt))   /* world and press nodes */
            game_enemy_shot(e, m0, (Vec3){ cosf(e->ang), 0, sinf(e->ang) }, 1000.0f, e->P.shot_dmg, e->P.steer, e->P.shot_visual, e->P.shot_fx);
        e->reload += e->P.reload; e->st = S_WAIT;
        break; }
    case S_DASH0:
        if (!see) { e->st = S_TOWANDER; break; }
        if (dxz > e->P.melee) break;
        e->want_ang = chase_target(e, to_player);
        e->turn_t = fabsf(ang_diff(to_player, e->ang)) / e->P.turn_fast; e->speed = e->want_speed = e->P.run; e->atk_t = dxz / e->P.run; e->behav = 1; e->st = S_DASH;
        sac_set_len(e, 8, e->atk_t);                              /* 0x436bd0(8, +0x1d4, 1): the charge run animation lasts the run */
        break;
    case S_DASH:
        if (!see || e->atk_t < 0) { e->t = sac_len(e, 9); e->want_speed = e->P.walk; e->behav = 2; e->st = S_BRAKE; break; }   /* 14: Stand */
        e->atk_t -= dt; steer(e, e->want_ang, e->P.turn_fast, dt); e->want_ang = chase_target(e, to_player);
        if ((e->turn_t -= dt) <= 0) step = (Vec3){ cosf(e->ang) * e->speed * dt, 0, sinf(e->ang) * e->speed * dt };
        if (sqrtf(dx * dx + dy * dy + dz * dz) < e->P.radius + 69.0f) {
            Vec3 d = dxz > 1e-3f ? (Vec3){ dx / dxz, 0, dz / dxz } : (Vec3){ 1, 0, 0 };
            if (player_hit(pl, e->P.bite, d)) { player_kill(pl, 3); e->t = sac_len(e, 11); e->behav = 2; e->st = S_WIN; }   /* 8: Stand */
            else { e->t = sac_len(e, 10); e->behav = 2; e->st = S_BITE; }   /* 1: Stand */
        }
        break;
    case S_BITE: if (e->t > 0) e->t -= dt; else { e->cool = e->P.cool; e->st = S_TOWANDER; } break;
    case S_BRAKE: if (e->t < 0) { e->cool = e->P.cool; e->st = S_TOWANDER; } else e->t -= dt; break;
    case S_WIN: if ((e->t -= dt) <= 0) e->st = S_TOWANDER; break;
    case S_HIT:
        if (e->hp <= 0) { game_enemy_stars(e); e->attackable = 0; e->dead_t = 0; e->behav = 2; e->st = S_DEAD; }
        else if (e->hit_t <= 0) e->st = S_TOWANDER;
        break;
    case S_DEAD: {
        e->dead_t += dt; float L = sac_len(e, 13) + 1.0f;
        if (e->dead_t > L * 0.5f) { in->fade = (e->dead_t - L * 0.5f) / (L * 0.5f); if (in->fade > 1) in->fade = 1; }
        if (e->dead_t >= L) { e->removed = 1; in->visible = 0; g_eset->killed++; return; }   /* +0x10c |= 1 -> 0x40bf60 -> vtbl[29] 0x41aff0: [0x4c532c]++ (the defeated-enemies stat) */
        break; }
    case S_DODGE0: {                                              /* 0x417a63: 300 away along the dive, else sideways, else back */
        float l = sqrtf(e->warn.x * e->warn.x + e->warn.z * e->warn.z); Vec3 d = l > 1e-3f ? (Vec3){ e->warn.x / l * e->P.dodge, 0, e->warn.z / l * e->P.dodge } : (Vec3){ e->P.dodge, 0, 0 };
        Vec3 cand[4] = { { d.x, 0, d.z }, { -d.z, 0, d.x }, { d.z, 0, -d.x }, { -d.x, 0, -d.z } }, pick = cand[0];
        for (int k = 0; k < 4; k++) {
            Vec3 c = { e->pos.x + cand[k].x, e->pos.y, e->pos.z + cand[k].z }; int found;
            float rt; if (player_ray_full(pl, (Vec3){ e->pos.x, e->pos.y + e->P.height * 0.5f, e->pos.z }, (Vec3){ c.x, c.y + e->P.height * 0.5f, c.z }, &rt)) continue;   /* 0x4359b0: world and press nodes */
            float gy = player_ground_query(pl, in, (Vec3){ c.x, c.y + e->P.height * 0.5f, c.z }, &found), drop = e->pos.y - gy;   /* GetHeight 0x435650(c, -1, 1): nothing found = c.y, so -drop = h/2 */
            if (drop <= e->P.drop && -drop <= e->P.rise) { pick = cand[k]; break; }
        }
        e->dodge_dir = (Vec3){ pick.x / e->P.dodge, 0, pick.z / e->P.dodge }; e->ang = atan2f(-pick.z, -pick.x);   /* faces against the move */
        e->t = e->P.dodge / (3.0f * e->P.run); e->behav = 2; e->st = S_DODGE;
        break; }
    case S_DODGE:
        if (e->t > 0) { e->t -= dt; float v = 3.0f * e->P.run * dt; step = (Vec3){ e->dodge_dir.x * v, 0, e->dodge_dir.z * v }; }
        else { e->reload = e->P.reload; e->cool = e->P.cool; e->speed = e->want_speed = e->P.walk; e->behav = 0; e->st = S_WANDER; wander_restart(e, 0); }   /* 0x41c0c0(0): Wander restarts with action 0 */
        break;
    }
    enemy_move(e, pl, step, dt);                                  /* the common move 0x41b2c0 of the behaviour tick, every frame */
    if (e->st == S_WANDER) wander_avoid(e, dt);
    { int found; float gy, h2 = e->P.height * 0.5f; int on = enemy_probe(e, pl, (Vec3){ e->pos.x, e->pos.y + h2, e->pos.z }, h2, &gy, &found);   /* ground following 0x41a4e0 */
      e->vfall += 200.0f * dt - 0.2f * e->vfall; e->pos.y -= e->vfall;
      if (found && e->pos.y <= gy) { e->pos.y = gy; e->vfall = 0; on = 1; }
      game_msgmask(in, 0x200, on); }
    sac_clock(e);                                                 /* the instance clock of this frame's draw */
    enemy_place(e);
}

/* ---- type 12, the bomb thrower = the W2B end boss (docs/ENEMY2.md 4): Update 0x4110c0, animation table 0x411c40 ----------
 * He never walks (only the behaviour Stilstaan). He turns to the player at 2 pi rad/s, throws a bomb from his marker 1 as soon
 * as he faces him (template 0 at 800 u/s, fuse 7.5 s four times, then 1.3 s once), swipes within 300, cannot be pecked (a peck
 * only breaks his rhythm for 3.6 s) and loses 1 of his 5 hp per bomb blast, with 3.6 s of immunity after each. When he is dead
 * msgmask 0x10 tells the script to end the level (W2B object 533: MSGTEST 16 -> 1083). Touch 0x411840 is the base 0x40c1e0 (3D
 * distance of the position pointers) with P+0x8c added once after the flag +0x1fc; like every class he has no boss bar. */
enum { BA_AIM, BA_THROW, BA_WIND, BA_AFTER, BA_BACK, BA_WIN, BA_STUN, BA_DEAD, BA_HIT };
static const struct { int anim; float speed; int hold; } g_ba[] = {    /* logical 0, 14, 3, 5, 4, 6, 15, 8, 7 (records 0x4b1d10) */
    { 0, 3, 0 }, { 6, 3, 1 }, { 3, 3, 1 }, { 4, 2, 1 }, { 5, 3, 1 }, { 7, 3, 1 }, { 8, 3, 1 }, { 10, 1, 1 }, { 9, 3, 1 } };
static float ba_len(const Enemy *e, int a) { const Model *m = e->inst->model; int s = g_ba[a].anim; return (uint32_t)s < m->nanims ? m->anims[s].duration_s / g_ba[a].speed : 0.5f; }
static void ba_play(Enemy *e, int s, float speed, int hold)
{
    Instance *in = e->inst; const Model *m = in->model; if ((uint32_t)s >= m->nanims) return;
    if (in->anim != s) { in->anim = s; in->anim_time = 0; }
    in->anim_speed = speed;
    if (hold && in->anim_time > m->anims[s].duration_s - 0.15f) { in->anim_time = m->anims[s].duration_s * 0.999f; in->anim_speed = 0; }
}
static int bomber_peck(Enemy *e)                                        /* vtbl[39] 0x411ab0: never damage, always "not dead" */
{
    if (e->removed || e->st == 13 || e->hit_t > 0) return 0;
    e->hit_t = ba_len(e, BA_STUN); audio_fx(54, NULL, NULL); e->st = 11;
    return 0;
}
static void bomber_blast(Enemy *e, Vec3 c, float r)                    /* vtbl[40] 0x4119b0: 1 hp per explosion */
{
    float dx = e->pos.x - c.x, dy = e->pos.y - c.y, dz = e->pos.z - c.z;
    if (e->st == 13 || e->hit_t > 0 || dx * dx + dy * dy + dz * dz >= r * r) return;
    audio_fx(52, NULL, NULL); e->st = 14; e->hit_t = ba_len(e, BA_STUN);
    /* Enemy_TakeDamage 0x41adc0(0, 1.0, &(0,0,0), pos, 0): Behav_Knock 0x41b6b0 refuses while Stand's knock timer runs, else it starts it
     * with a zero direction for vtbl[53](-1) = AnimLen(7) in state 14 (0.4 s with the W2B model, always shorter than the 3.6 s hitT that
     * gates this function, so the refusal never happens); then the star on the blast point and 1 hp */
    if (e->knock_t > 0) return;
    e->knock_dir = (Vec3){ 0, 0, 0 }; e->knock_t = ba_len(e, BA_HIT);
    e->hp -= 1.0f; game_hit_star(c);
    if (wenv("WOODY_BOSSLOG")) printf("  THROWER %u blast: hp %.0f", e->inst->index, e->hp), puts("");
}
/* actor list 1 (0x4c52d8, max 8, double-buffered by 0x40bf60): besides the Perso only two enemy Updates call RegisterActor 0x40c080 --
 * the bomb thrower 0x4110e6 (every Update) and Boss2 0x40dd58 (once message 61 has linked his crushers). An Update only runs after
 * Think 0x41a320 (listed in world+0x64, within active_d of the camera or dead), so the membership is the one of this frame's Update */
static int actor_list1(const Enemy *e, Vec3 cam)
{
    if ((e->type != 12 && e->type != 15) || e->removed || !e->inst->visible || !game_enemy_thinks(e->inst)) return 0;
    if (e->type == 15 && !e->bb.crush[0]) return 0;
    float dx = e->pos.x - cam.x, dy = e->pos.y - cam.y, dz = e->pos.z - cam.z;
    return dx * dx + dy * dy + dz * dz < e->P.active_d * e->P.active_d || (e->type == 12 ? e->st == 13 : e->hp <= 0);
}
/* the rocket explosion (state 7, 0x453560..0x4535a9): every list-1 actor of category 1 or 2 gets vtbl[40](&rocket.pos, 600);
 * for the enemies that is the thrower's 0x4119b0 (1 hp) or Boss2's 0x40e800 (sphere against his cylinder, 1 hp) */
void enemies_actor_blast(EnemySet *s, Vec3 c, float r)
{
    for (int i = 0; i < s->n; i++) {
        Enemy *e = &s->e[i]; if (!e->list1) continue;
        if (e->type == 12) bomber_blast(e, c, r); else if (e->type == 15) boss15_blast(e, c, r);
    }
}
/* ---- the engine's actor hit tests (0x433920 / 0x433bc0 / 0x433de0), literally. The comparisons keep the x87 jumps: a NaN
 * (a zero-length sweep divides 0.5 by 0) goes the way `fcom; test ah, 1/0x41` sends it. g_hit_frac mirrors the one global
 * hit fraction [0x53a558]: 0x433920/0x433bc0 write it on a hit, the charge loop writes 0.5 (0x4589e9), the laser rays their
 * answer; the laser of type 50 reads it back when its ray finds nothing. In the original every ray and collision sweep of the
 * frame also writes it (19 writers, 0x424a69..0x4589e9); the port's other rays do not (port simplification). */
float g_hit_frac;
/* 0x433bc0(a, b, r, s, R): a sphere r swept from a to b against the sphere (s, R) in 3D. The entry fraction when it lies in
 * [0, 1], else the exit fraction when that does (a start inside the sphere); a sweep that starts and ends inside misses */
static int sweep_sphere_sphere(Vec3 a, Vec3 b, float r, Vec3 s, float R)
{
    float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z, ex = a.x - s.x, ey = a.y - s.y, ez = a.z - s.z;
    float A = dz * dz + dy * dy + dx * dx, B = (ez * dz + ey * dy + ex * dx) * 2.0f, Rs = r + R;
    float disc = B * B - (ez * ez + ey * ey + ex * ex - Rs * Rs) * A * 4.0f;               /* 0x4a94c0 = 4 */
    if (!(disc >= 0)) return 0;                                                             /* 0x433c7c */
    float sq = sqrtf(disc), inv = 0.5f / A, t0 = (-B - sq) * inv, t1 = (sq - B) * inv;     /* 0x4a9014 = 0.5 */
    if (t0 > t1) { float q = t0; t0 = t1; t1 = q; }                                         /* 0x433cbd */
    if (!(t0 > 1.0f) && t0 >= 0) { g_hit_frac = t0; return 2; }                             /* 0x433ccf..0x433cf7 */
    if (t1 > 1.0f || !(t1 >= 0)) return 0;                                                  /* 0x433cf8..0x433d1a */
    g_hit_frac = t1; return 2;
}
/* 0x433920(a, b, r, c, R, H): a sphere r swept from a to b against the upright cylinder with its FOOT at c, radius R, height H.
 * xz quadratic with R + r (a start inside the circle with no xz motion = the whole segment), the y span of the part inside the
 * circle against [c.y - r, c.y + H + r]; the straight wall only counts when that span crosses the whole of [c.y + R, c.y + H - R],
 * otherwise the two end spheres (c.y + R and c.y + H - R, radius R) decide: a capsule. Returns 2 (hit, g_hit_frac = entry) or 0 */
int sweep_sphere_cyl(Vec3 a, Vec3 b, float r, Vec3 c, float R, float H)
{
    float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z, ex = a.x - c.x, ez = a.z - c.z;
    float A = dz * dz + dx * dx, B = (ez * dz + ex * dx) * 2.0f, Rs = r + R, C = ez * ez + ex * ex - Rs * Rs;
    float disc = B * B - C * A * 4.0f, t0, t1;
    if (!(disc >= 0)) return 0;                                                             /* 0x4339c6 */
    if (!(A > 0.001f)) {                                                                    /* 0x4339db, 0x4a94c4: no xz motion */
        if (C >= 0) return 0;                                                               /* 0x433b2a: the start is outside the circle */
        t0 = 0; t1 = 1.0f;
    } else {
        float sq = sqrtf(disc), inv = 0.5f / A;
        t0 = (-B - sq) * inv; t1 = (sq - B) * inv;
        if (t0 > t1) { float q = t0; t0 = t1; t1 = q; }                                     /* 0x433a18 */
        if (t0 > 1.0f || !(t1 >= 0)) return 0;                                              /* 0x433a39, 0x433a4a */
        if (!(t0 >= 0)) t0 = 0;                                                             /* 0x433a61 */
        if (t1 > 1.0f) t1 = 1.0f;                                                           /* 0x433a78 */
    }
    float y0 = dy * t0 + a.y, y1 = dy * t1 + a.y;
    if (y0 > y1) { float q = y0; y0 = y1; y1 = q; }                                         /* 0x433aa9 */
    if (c.y - r > y1 || !(r + H + c.y >= y0)) return 0;                                     /* 0x433acb, 0x433ae5 */
    if (H + c.y - R > y0 && !(R + c.y >= y1)) { g_hit_frac = t0; return 2; }                /* 0x433aff, 0x433b11: through the straight part */
    if (sweep_sphere_sphere(a, b, r, (Vec3){ c.x, R + c.y, c.z }, R)) return 2;             /* 0x433b6d: lower cap */
    return sweep_sphere_sphere(a, b, r, (Vec3){ c.x, H + c.y - R, c.z }, R);                /* 0x433ba2: upper cap */
}
/* 0x433de0(a, b, c, R, h): the segment a..b against the upright cylinder around the CENTRE c, radius R, y range
 * [c.y - h + 0.1, c.y + h - 0.1] (0x4a9008 = 0.1): the xz quadratic clipped to [0, 1], then the y span of that part.
 * 0.5 (0x4a9014) on a hit, -1 (0x4a9500) otherwise - never the fraction. The laser 0x450f80 passes h = half the height,
 * the charge run 0x4589d0 the whole height around feet + h/2 (so there the range is feet - h/2 + 0.1 .. feet + 1.5 h - 0.1) */
float seg_cyl(Vec3 a, Vec3 b, Vec3 c, float R, float h)
{
    float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z, ex = a.x - c.x, ez = a.z - c.z;
    float A = dz * dz + dx * dx, B = (ez * dz + ex * dx) * 2.0f, C = ez * ez + ex * ex - R * R;
    float disc = B * B - C * A * 4.0f, t0, t1;
    if (!(disc >= 0)) return -1.0f;                                                         /* 0x433e7c */
    if (!(A > 0.001f)) {                                                                    /* 0x433e91 */
        if (C >= 0) return -1.0f;                                                           /* 0x433fbe */
        t0 = 0; t1 = 1.0f;
    } else {
        float sq = sqrtf(disc), inv = 0.5f / A;
        t0 = (-B - sq) * inv; t1 = (sq - B) * inv;
        if (t0 > t1) { float q = t0; t0 = t1; t1 = q; }                                     /* 0x433ed8 */
        if (!(t1 > 0) || t0 >= 1.0f) return -1.0f;                                         /* 0x433ef9, 0x433f0a */
        if (!(t0 >= 0)) t0 = 0;                                                             /* 0x433f1d */
        if (t0 > 1.0f) t0 = 1.0f;                                                           /* 0x433f32 */
        if (t1 > 1.0f) t1 = 1.0f;                                                           /* 0x433f4b */
    }
    float y0 = t0 * dy + a.y, y1 = dy * t1 + a.y;
    if (y0 > y1) { float q = y0; y0 = y1; y1 = q; }                                         /* 0x433f72 */
    if (y0 > h + c.y - 0.1f || c.y - h + 0.1f > y1) return -1.0f;                           /* 0x433f9a, 0x433fb2 */
    return 0.5f;
}
Enemy *enemies_bomb_contact(EnemySet *s, const Enemy *owner, Vec3 a, Vec3 b, float r)
{
    for (int i = 0; i < s->n; i++) {
        Enemy *e = &s->e[i];
        if ((e->type != 12 && e->type != 15) || e == owner || e->removed || !e->inst->visible) continue;   /* subtype 8 (type 12) and 12 (class 15, actor list 1 via 0x40c080) */
        if (e->type == 12 && (e->st == 1 || e->st == 13)) continue;   /* vtbl[47] 0x411970: no actor in states 1 / 13 */
        /* vtbl[24] 0x41ad80 = {pos + (0, h/2, 0), radius, h/2}; 0x44a15f passes the foot (c.y - h/2) and the whole height */
        if (sweep_sphere_cyl(a, b, r, e->pos, e->P.radius, e->P.height)) return e;
    }
    return NULL;
}
static void bomber_update(Enemy *e, struct Player *pl, Vec3 cam, float dt)
{
    Instance *in = e->inst;
    if (e->removed || !in->visible || !game_enemy_thinks(in)) return;      /* Think 0x41a320 only runs from 0x42b400, for the instances of this frame's list world+0x64 */
    ground_snap(e, pl);
    { float dx = e->pos.x - cam.x, dy = e->pos.y - cam.y, dz = e->pos.z - cam.z; if (dx * dx + dy * dy + dz * dz >= e->P.active_d * e->P.active_d && e->st != 13) return; }   /* Think 0x41a320 */
    Vec3 tp = perso_pos(pl); float dx = tp.x - e->pos.x, dy = tp.y - e->pos.y, dz = tp.z - e->pos.z, d3 = sqrtf(dx * dx + dy * dy + dz * dz);
    int see = !pl->dead_kind && d3 < e->P.see && fabsf(dy) < e->P.dy;   /* FindTarget(1, 0) */
    if (e->reload > 0) e->reload -= dt;
    if (e->hit_t > 0) e->hit_t -= dt;
    if (see && d3 < e->P.melee && e->st != 4 && e->st != 5 && e->st != 13 && e->st != 14) e->st = 4;   /* byte table 0x4117f0 */
    int anim = -1; float speed = 3; int hold = 1;
    switch (e->st) {
    case 0:                                                             /* idle 0x4111db: variations 9..13 (.ins 11..15, speed 2) */
        if (see) { e->st = 2; break; }
        if ((e->idle_t -= dt) <= 0) {
            int n = 9 + crt_mod(5); if (n == e->idle_a) n = 9 + (n - 8) % 5;
            e->idle_a = n; const Model *m = in->model; int s = n + 2; e->idle_t = (uint32_t)s < m->nanims ? m->anims[s].duration_s / 2.0f : 1.0f;
            in->anim = -1;                                              /* restart even when the same one comes again */
        }
        anim = e->idle_a + 2; speed = 2; hold = 1;
        break;
    case 2: {                                                           /* aim 0x4111ff: turn at P+0x14 = 2 pi, throw once he faces the player */
        if (!see) { e->st = 0; break; }
        float want = atan2f(dz, dx); steer(e, want, 6.2831853f, dt);
        anim = g_ba[BA_AIM].anim; hold = 0;
        if (e->reload > 0 || fabsf(ang_diff(want, e->ang)) > 1e-4f) break;
        Vec3 m0; if (!shooter_vector(in, 1, &m0)) break;               /* GetVector(typecode 1) = his throwing hand */
        Vec3 d = { tp.x - m0.x, tp.y - m0.y, tp.z - m0.z }; float l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z); if (l < 1e-3f) break;
        float fuse; if (e->nlong) { fuse = 7.5f; e->nlong--; } else { fuse = 1.3f; e->nlong = 4; }   /* P+0x94 / P+0x90 / P+0x98 */
        if (!game_enemy_bomb(e, m0, (Vec3){ d.x / l, d.y / l, d.z / l }, 800.0f, fuse)) break;   /* no free bomb: the fuse count is spent anyway */
        e->reload = ba_len(e, BA_THROW); e->st = 3; audio_fx(50, NULL, NULL);
        if (wenv("WOODY_BOSSLOG")) printf("  THROWER %u throws a bomb (fuse %.1f)", in->index, fuse), puts("");
        break; }
    case 3:                                                             /* throw 0x411341 */
        anim = g_ba[BA_THROW].anim;
        if (e->reload > 0) break;
        e->reload += e->P.reload - 0.5f * floorf((e->P.hp - e->hp) * 0.333333f); e->st = 2;   /* 0.5 s faster from 3 damage on */
        break;
    case 4:                                                             /* swipe start 0x4113b2 */
        if (!see) { e->st = 0; break; }
        e->windup = ba_len(e, BA_WIND); e->melee_t = ba_len(e, BA_WIND) + ba_len(e, BA_AFTER); e->st = 5; audio_fx(51, NULL, NULL);
        in->anim = -1; anim = g_ba[BA_WIND].anim;
        break;
    case 5:                                                             /* swipe 0x411412 */
        anim = g_ba[BA_WIND].anim;
        if (!see || e->melee_t < 0) { e->st = 7; break; }
        e->melee_t -= dt; e->windup -= dt;
        if (e->windup > 0) break;
        e->big_touch = 1;
        if (d3 >= e->P.radius + 69.0f + 300.0f) { e->big_touch = 0; e->st = 0; break; }   /* Touch 0x411840 with the reach P+0x8c */
        e->big_touch = 0;
        { float l = sqrtf(dx * dx + dz * dz); Vec3 d = l > 1e-3f ? (Vec3){ dx / l, 0, dz / l } : (Vec3){ 1, 0, 0 };
          if (player_hit(pl, e->P.bite, d)) { player_kill(pl, 3); e->st = 9; } else { e->t = ba_len(e, BA_AFTER); e->st = 6; } }
        break;
    case 6: anim = g_ba[BA_AFTER].anim; speed = 2; if (e->t > 0) e->t -= dt; else e->st = 11; break;
    case 7: e->t = ba_len(e, BA_BACK); e->st = 8; in->anim = -1; /* fallthrough */
    case 8: anim = g_ba[BA_BACK].anim; if ((e->t -= dt) <= 0) e->st = 0; break;
    case 9: e->t = ba_len(e, BA_WIN); e->st = 10; in->anim = -1; /* fallthrough */
    case 10: anim = g_ba[BA_WIN].anim; if ((e->t -= dt) <= 0) e->st = 0; break;
    case 11: e->t = ba_len(e, BA_STUN); e->st = 12; in->anim = -1; /* fallthrough */
    case 12: anim = g_ba[BA_STUN].anim; if ((e->t -= dt) <= 0) e->st = 0; break;
    case 13:                                                            /* dead 0x4116ec */
        anim = g_ba[BA_DEAD].anim; speed = 1; e->dead_t += dt;
        { float L = ba_len(e, BA_DEAD) + 1.0f;                            /* vtbl[51] 0x411d30 = AnimLen(8) + 1.0 */
          if (e->dead_t > L * 0.5f) { in->fade = (e->dead_t - L * 0.5f) / (L * 0.5f); if (in->fade > 1) in->fade = 1; }   /* Enemy::Update 0x41a3e0: fades out in the second half */
          if (e->dead_t >= L && !e->done) {                              /* flags10c |= 1 -> 0x40bf60 -> vtbl[29] 0x41aff0: [0x4c532c]++, 0x407850 = out of the world */
              e->done = 1; game_msgmask(in, 0x10, 1); e->removed = 1; in->visible = 0; g_eset->killed++;
              if (wenv("WOODY_BOSSLOG")) printf("  THROWER %u dead: msgmask 0x10, removed", in->index), puts(""); } }
        break;
    case 14:                                                            /* hit by a blast 0x411759 */
        anim = g_ba[BA_HIT].anim;
        if (e->hp <= 0) { e->dead_t = 0; game_enemy_stars(e); e->attackable = 0; e->st = 13; audio_fx(53, NULL, NULL); in->anim = -1; }
        else if (e->hit_t <= 0) e->st = 0;
        break;
    }
    if (e->st == 4 || e->st == 7 || e->st == 9 || e->st == 11) anim = -1;   /* the set-up states have no animation of their own */
    enemy_move(e, pl, (Vec3){ 0, 0, 0 }, dt);                         /* Enemy::Update: Stand's Tick = the common move 0x41b2c0 with a zero step (sweep, platform, knock timer) */
    { int found; float gy, h2 = e->P.height * 0.5f; int on = enemy_probe(e, pl, (Vec3){ e->pos.x, e->pos.y + h2, e->pos.z }, h2, &gy, &found);   /* ground following 0x41a4e0 */
      e->vfall += 200.0f * dt - 0.2f * e->vfall; e->pos.y -= e->vfall;
      if (found && e->pos.y <= gy) { e->pos.y = gy; e->vfall = 0; on = 1; }
      game_msgmask(in, 0x200, on); }
    if (anim >= 0) ba_play(e, anim, speed, hold);
    enemy_place(e);
}

void enemy_warn_dive(Enemy *e, Vec3 d) { if (e->type == 9 && !e->removed && e->st != S_DEAD && e->st != S_HIT) { e->warn = d; e->st = S_DODGE0; } }
void enemy_player_killed(Enemy *e)
{
    if (!e || e->removed) return;
    if (e->type == 13) { if (e->st != 12) { e->t = ea_len(e, EA_WIN); e->want_speed = e->P.walk; e->st = 11; } }
    else if (e->type >= 7 && e->type <= 9 && e->st != S_DEAD) { e->t = sac_len(e, 11); e->st = S_WIN; }
}

/* vtbl[17] Reset: Enemy::Reset 0x41a010 (back to the start position and angle, re-entered into the world = visible again, full hp,
 * timers, hit/death state and the remove flag cleared, attackable again, msgmask 0x10 cleared, animation reset 0x42e250) and the
 * class part: 4/5/6 0x418c20 and 13 0x4139b0 -> wander (8), 7/8/9 0x416ed0 -> wander (3), all with a restarted Wander (patrol 0 with a
 * TRAJ, which no enemy has); the bomb thrower 0x411020 -> state 0, idle 9, four long fuses. The ground snap 0x41a1a0 (subtypes < 9)
 * is left to the ground follower. */
static void enemy_reset(Enemy *e)
{
    Instance *in = e->inst;
    e->pos = e->home = e->start; e->ang = e->want_ang = e->start_ang;
    e->hp = e->P.hp; e->hit_t = e->dead_t = e->knock_t = e->vfall = 0; e->removed = 0; e->attackable = 1;
    in->visible = 1; in->fade = 0; in->anim = 0; in->anim_time = 0; in->anim_speed = 0; e->lanim = -1;   /* 0x42e250 */
    e->plat_inst = NULL;                                          /* both probes reset (0x436d10) */
    e->need_snap = e->type < 13; if (g_epl) ground_snap(e, g_epl); enemy_reset_probe(e);   /* 0x41a0f8..0x41a148: ground snap 0x41a1a0, then the probe (Press/In/UnPress) */
    game_msgmask(in, 0x10, 0);
    e->cool = e->t = e->atk_t = e->reload = e->turn_t = 0; e->path_to = 1;
    if (e->type >= 7 && e->type <= 9) sac_init(e);                /* the animation reset 0x42e250 and the AnimCtrl reset of 0x416ed0 */ e->path_dir = 1;
    if (e->type == 12) { e->st = 0; e->idle_t = 0; e->nlong = 4; e->idle_a = 9; e->done = 0; e->melee_t = e->windup = 0; e->big_touch = 0; e->behav = 2; }
    else {
        e->speed = e->want_speed = e->P.walk;
        if (in->traj.npoints > 1) { e->st = 0; e->behav = 3; } else { e->st = e->type >= 7 && e->type <= 9 ? S_WANDER : 8; wander_restart(e, -1); e->behav = 0; }
    }
    if (e->type == 13) e->hand = crt_rand() & 1;                  /* 0x413a77: the ghost's first hand, drawn after the behaviour restart */
    enemy_place(e);
}

/* game messages 1201 / 1202 [inst] (0x4034ba / 0x403473, docs/OBJECTS.md 3.1): type word (inst+0x104, vtbl[4] 0x403fe0) bit
 * 0x400 on / off, the "attackable" bit the target finder 0x4632e0 reads. The handler tests bit 0 of the instance REF, not of
 * anything in the instance (0x403496 / 0x4034de: test byte [msg+8], 1), so only odd slots are touched; it calls vtbl[4]
 * before the NULL test and never checks its result (the classes whose vtbl[4] is 0x4078b0 = NULL, e.g. 41/60/70/90/100/110,
 * would crash the original). No shipped level sends either message; of the classes with a type word only the enemies
 * (4..16) are targets in the port, so the others are ignored here. */
void enemies_msg1201(EnemySet *s, Instance *inst, uint32_t ref, int on)
{
    if (!inst || !(ref & 1)) return;
    for (int i = 0; i < s->n; i++) if (s->e[i].inst == inst) s->e[i].attackable = on;
}

/* Enemy::HandleMsg 0x41a740, id 11 [inst, n, v] (jump table 0x41ac40; docs/ENEMY.md 7). The scripts of the 28 levels send
 * n = 0, 1, 4, 5, 6, 7, 8, 13, 32, 33, 37 (scan: docs/MESSAGES.md 11); the rest is ported from the handler all the same. */
void enemies_msg11(EnemySet *s, Instance *inst, int n, int v)
{
    Enemy *e = NULL; for (int i = 0; i < s->n; i++) if (s->e[i].inst == inst) e = &s->e[i];
    if (!e) return;
    if (e->type == 14) { if (n == 4) boss_reset(e); return; }        /* Enemy::HandleMsg 11/4 = vtbl[17]; the rest writes P fields the boss sets itself */
    if (e->type == 15 || e->type == 16) { if (n == 4) { if (e->type == 15) boss15_reset(e); else boss16_reset(e); } return; }   /* the same for classes 15 / 16 */
    int has_wander = e->type != 12;                                   /* +0x160 / +0x164: the bomb thrower has only the Stand behaviour */
    switch (n) {
    case 0: e->P.leash = (float)v; break;                             /* P+0x1c */
    case 1: e->P.see = (float)v; break;                               /* P+0x20 */
    case 2: e->P.drop = v < 1 ? 1.0f : (float)v; break;               /* P+0x2c, at least 1 */
    case 3: e->P.rise = v < 1 ? 1.0f : (float)v; break;               /* P+0x30, at least 1 */
    case 4: enemy_reset(e); break;                                    /* vtbl[17] */
    case 5: if (!has_wander) break;                                   /* "trajet aleatoire": 0 = leash off and the guard flag off, 1 = leash round the home point */
            if (v == 0) { e->w_leash = 0; e->guard = 0; } else e->w_leash = 1; break;
    case 6: if (has_wander) e->guard = v == 1; break;                 /* "trajet de suivi": flag 2, FindTarget from the home point */
    case 7: e->P.walk = (float)v; e->speed = e->want_speed = (float)v; break;   /* P+0x08 and H speed direct (0x41b9d0(v, 1)) */
    case 8: e->P.run = (float)v; e->speed = e->want_speed = (float)v; break;    /* P+0x0c and H speed direct */
    case 9: case 10: e->P.turn = (float)v * 3.14159265f / 180.0f; break;       /* both write P+0x10 */
    case 12: e->P.bite = (float)v; break;                             /* P+0x3c */
    case 13: e->P.hp = e->hp = (float)v; break;                       /* P+0x34 and +0x150 */
    case 14: e->P.dy = (float)v; break;                               /* P+0x24 */
    case 15: e->P.cool = (float)v; break;                             /* P+0x38 */
    case 18: e->p154 = (float)v * 0.01f; break;                       /* +0x154: no reader */
    case 19: if (has_wander) for (int i = 0; i < 8; i++) e->w_weight[i] = (unsigned short)v; break;   /* 0x41c0f0 keeps the kind bits */
    case 20: case 21: case 22: case 23: case 24: if (has_wander) e->w_weight[n - 20] = (unsigned short)v; break;
    case 25: case 26: case 27: if (has_wander) e->w_weight[n - 20] = (unsigned short)v; break;          /* actions 5, 6, 7 */
    case 30: e->flag20 = v != 0; break;                               /* +0x174 flag 0x20: no reader */
    case 31: e->P.shot_dmg = (float)v; break;                         /* P+0x40 */
    case 32: e->p48 = (float)v; break;                                /* P+0x48: no reader in these classes */
    case 33: e->P.reload = (float)v; break;                           /* P+0x4c */
    case 34: e->p50 = (float)v; break;                                /* P+0x50: only type 10 reads it */
    case 35: e->P.melee = (float)v; break;                            /* P+0x54 */
    case 36: e->P.dodge = (float)v; break;                            /* P+0x58 */
    case 37: e->P.active_d = (float)v; break;                         /* P+0xc0 */
    default: break;                                                   /* 11, 16, 17, 28, 29 and > 37: ignored (0x41ac2a) */
    }
    if (wenv("WOODY_MSG11LOG")) printf("  MSG11 inst %u (type %d) n %d v %d", inst->index, e->type, n, v), puts("");
}

void enemies_update(EnemySet *s, struct Player *pl, Vec3 cam_pos, float dt)
{
    g_eset = s; g_epl = pl; g_enow += dt;
    for (int i = 0; i < s->n; i++) {
        Enemy *e = &s->e[i];
        if (e->type == 14) { boss_update(e, pl, cam_pos, dt); boss_frame_end(e); }
        else if (e->type == 15) boss15_update(e, pl, cam_pos, dt);
        else if (e->type == 16) boss16_update(e, pl, cam_pos, dt);
        else if (e->type == 12) bomber_update(e, pl, cam_pos, dt);
        else if (e->type >= 7 && e->type <= 9) shooter_update(e, pl, cam_pos, dt); else enemy_update(e, pl, cam_pos, dt);
        e->list1 = actor_list1(e, cam_pos);                           /* RegisterActor 0x40c080 in the Update */
    }
}
