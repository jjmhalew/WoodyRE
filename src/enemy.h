/* enemy.h - enemy classes (docs/ENEMY.md): types 4/5/6 (patrol / wander, notice, chase, dash attack, brake, hit, death with
 * fade) and the shooters 7/8/9 (section 8: wait, aim, fire, short dash with a bite, type 9 dodges the air dive). */
#ifndef WOODY_ENEMY_H
#define WOODY_ENEMY_H
#include "level.h"

struct Player;

/* parameter block P (0x41d510); message 11 writes into it. shot_visual = P+0x74 (0/1 missile, 2 bolt, 3 fireball),
 * shot_fx = the SoundFx that 0x449130 plays for that visual */
typedef struct { float radius, height, walk, run, dash, see, dy, turn, turn_fast, leash, hp, cool, bite, shot_dmg, reload, melee, dodge, steer, active_d; int shot_visual, shot_fx;
                 float fall_g;                                                 /* P+0: gravity of the ground follower (200); the Fall object copies it once in PostLoad (0x419eb2), so Buzz's Reset writing 400 / 800 changes nothing */
                 float drop, rise; } EnemyParams;                              /* P+0x2c / P+0x30: max step down / up (10; subtype 10: 10000, the bosses: 15000) */

/* obstacle sensor Enemy+0x124 (ctor 0x41cfc0, tick 0x41d4a0(3), docs/OBSTACLE.md 3): 16 directions, one probed per frame */
typedef struct { float r, dr, s, jit; int i; int kind[17]; float ang[16]; unsigned char free[17]; } EnemySensor;   /* +4 +8 +0xc +0x14 +0x10, +0x1c/+0x20 + 8i, +0xa4 + i */

/* class 14, the Buzz boss (docs/BOSS14.md): fields beyond the Enemy base */
typedef struct BossState {
    int mode;                       /* +0x228: 0 off (only reads its mailbox), 1 W1B flying machine, 2 W2D/W3D/WWS hopping */
    uint32_t mail_var;              /* +0x230: script variable of message 60, the command mailbox */
    Instance *link;                 /* +0x234: the coupled instance of message 59 (W1B 404, type 17), carried along */
    int st, high, behav;            /* +0x1c0 state 0..12, +0x224 high phase (invulnerable), active behaviour 0 wander 1 chase 2 still */
    float t1d4, t1d8, t1e0, t1e4, acc;   /* shake / landing / cheer / high-wait timers, 60 Hz accumulator of the shake */
    int shake_i;                    /* +0x1e8 */
    float y_high, y_low; int y_low_ok;   /* +0x214 / +0x218 hover heights */
    Vec3 pl_prev, pl_cur, home_save; float leash_save;
    int bob_down; float bob, bob_max;    /* mode 2 hops */
    int grav, on_ground; float fall_v, fall_g;   /* flag 4 (falling) and flag 1 (on the ground), the base ground follower's per-frame speed and the Fall object's gravity (+4, P+0 at PostLoad = 200) */
    float turn;                     /* H turn rate (rad/s) */
    int w_act; float w_t;           /* Dwalen: current action (-1 none) and its remaining time */
    int c_run; float c_turn_t;      /* Achtervolgen: running, turn-in-place timer */
    float knock[3];                 /* each behaviour's own knockback timer (Behav+0x1c): a peck gives no direction, so it only freezes the step */
    int rec, sub, lrec, lsub;       /* AnimCtrl +0x1c4 and the link's +0x1c8: record and position in its chain */
    int blink, loop_on, active;     /* render-colour counter [0x4c5348], sound loop 39/44 playing, updated this frame */
} BossState;

/* classes 15 and 16, Buzz's second and third fights (boss.c, docs/BOSS15_16.md): fields beyond the Enemy base */
typedef struct BossBState {
    int st;                         /* +0x1c0 */
    uint32_t mail_var;              /* 15: +0x24c, 16: +0x294 (message 60) */
    int rec, sub, ended, held, req; /* AnimCtrl +0x1c4: running record, place in its chain, "animation ended" (inst+0xc0), queued request */
    Vec3 C;                         /* 15: +0x20c = placement + 1300 along the facing; 16: +0x254 = the placement */
    /* class 15 ("Boss2", ctor 0x40d850) */
    Instance *crush[4], *launch[4]; /* +0x1c8 the four crushers, +0x1d8 the four launchers (message 61) */
    Vec3 R, U, W;                   /* +0x1e8 / +0x1f4 / +0x200: the frame 0x46d320 builds from the facing */
    float t_intro, t_taunt, t_down, t_up, t_cycle;   /* +0x230, +0x254, +0x234, +0x238, +0x23c */
    float down, up, hold;           /* +0x248 slam time, +0x244 rise time, +0x240 cycle length */
    int phase, nstep, row, step, new_step, fire, phase_flag;   /* +0x218, +0x21c, +0x220, +0x224, +0x22c, +0x228, +0x250 */
    /* class 16 (ctor 0x40c730) */
    Instance *grp[4][7];            /* +0x1c8 pads, +0x1e4 pad tops, +0x200 light columns, +0x21c ambient volumes (message 62) */
    float depth[7];                 /* +0x238: lowest point of each group-3 model */
    float radius, interval;         /* +0x268 (1200), +0x26c (2.0 s, 0.2 less per hit) */
    int cur;                        /* +0x260: the pad he appears on */
    float t298, t29c, t2a0, t2a4, t264;
    int glow_n[7], glow_fr[7];      /* +0x270 running wave records per column, +0x278 frame stamp of the last fade write */
    struct { float t; int idx; } wave[16];   /* the records of 0x40c610 in the effect pool */
    int frame;
} BossBState;

typedef struct Enemy {
    EnemyParams P; float reload; Vec3 warn, dodge_dir;                   /* shooters */
    float react_t;                                                       /* PORT EXTRA (Gameplay page): time left before a melee enemy that noticed Woody may attack */
    int ac_cur, ac_nq, ac_q[16], ac_slot[4], ac_c0; float ac_start, ac_speed, ac_base, ac_pos;   /* shooters: AnimCtrl +0x1c4 (+0x48 record, +0x44/+0x04 queue) and the instance clock (slots +0xb0, +0xc0, +0xa8, +0xa0, +0xa4, +0xac) */
    BossState b;                                                         /* type 14 */
    BossBState bb;                                                       /* types 15, 16 */
    int hand;                                                            /* ghost (type 13): fires from alternating hands */
    int nlong, big_touch, idle_a, done; float idle_t, melee_t, windup;          /* bomb thrower (type 12): +0x200, +0x1fc, +0x1f8, +0x1f4, +0x1d4, +0x1f0 */
    Instance *inst; int type;
    int st;                         /* state machine 0x418cf0: 0 patrol, 1 chase, 2 notice, 3 miss, 4 dash, 6 brake, 8 wander, 9 hit, 11 win, 12 dead */
    Vec3 pos, home; float ang;      /* ang: movement angle, direction = (cos, 0, sin) */
    float speed, want_speed, turn_t, cool, hit_t, knock_t, atk_t, t, dead_t, hp, vfall;
    Vec3 knock_dir;
    int attackable, removed, chasing;
    int list1;                                   /* registered in actor list 1 (RegisterActor 0x40c080) by this frame's Update: types 12 and 15 only */
    float want_ang;                              /* H target angle (+0x04) */
    /* behaviour Wander (0x41bf30, docs/ENEMY.md 5.4): +0x50 action, +0x54 its remaining time, +0x30.. the 8 weights (message 11/19..27),
     * +0x60 homing timer, +0x69 "turned toward home, walk next", +0x68 leash on (message 11/5), +0x58/+0x5c avoidance timer / last actor */
    int w_act, w_homing, w_leash; float w_dur, w_home_t, w_avoid_t; const void *w_avoid; unsigned short w_weight[8];
    int guard;                                   /* +0x174 flag 2 (message 11/6): FindTarget measures from the home point */
    int lanim, lsub;                             /* AnimCtrl +0x1c4 for the wander actions: running record and place in its chain (-1 = a plain state anim) */
    Vec3 start; float start_ang;                 /* +0x128 start position and H start angle (Reset, message 11/4) */
    float p154, p48, p50;                        /* message 11/18 (+0x154, x 0.01), 32 / 34 (P+0x48 / +0x50): stored, no reader in the classes 4..9 / 12 / 13 */
    int flag20;                                  /* +0x174 flag 0x20 (message 11/30): no reader */
    int need_snap;                               /* Reset's ground snap 0x41a1a0 still to do (done in enemies_add for 4..12, else on the next update) */
    uint32_t path_to; int path_dir;              /* patrol along the instance TRAJ */
    EnemySensor sens;                            /* +0x124 */
    uint32_t col_cur;                            /* +0x198 = probe +0x178 +0x20: the world_collision it presses, 0xffffffff none (ctor 0x436cf0) */
    int behav;                                   /* +0x11c, the active behaviour: 0 Wander, 1 Chase, 2 Stand, 3 Pad (path) - picks OnBlocked / the hook of the common move */
    const Instance *plat_inst; uint32_t plat_node; Vec3 plat_local, plat_world;   /* probe +0x178: +0 instance, +4 press node, +8 point in node space, +0x14 in world (0x436d80) */
} Enemy;

#define MAX_ENEMIES 256
typedef struct EnemySet { Enemy e[MAX_ENEMIES]; int n;
    int total, killed;                           /* [0x4c5330]: ++ in the PostLoad of every class 4..13 (0x418c09, 0x416eb9, 0x4153a9, 0x4122f9, 0x411002, 0x413999; not the bosses 14..16);
                                                  * [0x4c532c]: ++ in vtbl[29] 0x41aff0 when 0x40bf60 removes an enemy whose +0x10c bit 0 its death set
                                                  * (0x419391, 0x4179f2, 0x41171f, 0x414396, 0x415f55, 0x412c81); both zeroed per level by 0x40bf30 (from the load, 0x40439e) */
} EnemySet;

void enemies_add(EnemySet *s, Instance *inst, int type, struct Player *pl);  /* on SetTypeInstance 4..9, 12..16 (pl: the level geometry for the Reset ground snap) */
void enemies_update(EnemySet *s, struct Player *pl, Vec3 cam_pos, float dt);
void enemies_anim_settle(EnemySet *s, const Instance *inst);                  /* right after inst's clock: step a wander chain whose part just ended */
/* vtbl[39] 0x419480: returns 1 when the enemy died. dir = (0,0,0) for a peck (no knockback). */
int  enemy_take_damage(Enemy *e, float dmg, Vec3 dir);
int  enemy_hit(Enemy *e, float dmg, Vec3 dir, Vec3 pt, int kind);              /* vtbl[39] with the hit point: Enemy_TakeDamage 0x41adc0 puts the hit star 0x4750e0 on pt (not for Buzz on kind 2) */
void game_hit_star(Vec3 pt);                                                  /* 0x4750e0: flash + 8 sparks with stars (in main_engine.c) */
void enemies_blast(EnemySet *s, Vec3 c, float r);                          /* bomb blast 0x44d650: every enemy within r dies (vtbl[40] 0x41ae20) */
void enemies_msg11(EnemySet *s, Instance *inst, int n, int v);                /* Enemy::HandleMsg 0x41a740, id 11 */
void enemies_msg1201(EnemySet *s, Instance *inst, uint32_t ref, int on);     /* 0x403440: 1201 sets / 1202 clears type-word bit 0x400 (Enemy.attackable) */
void enemy_warn_dive(Enemy *e, Vec3 d);                                       /* vtbl[37] 0x417ee0: the player starts an air dive at this enemy */
void game_enemy_stars(Enemy *e);                                              /* vtbl[57] 0x41b000 -> 0x477610: five stars circle over the dying enemy (in main_engine.c) */
void enemy_player_killed(Enemy *e);                                           /* vtbl[41] 0x417fd0: its projectile killed the player */
/* implemented by the engine: projectile 0x4490a0 from an enemy (template 1 with the P overrides) */
void game_enemy_shot(Enemy *owner, Vec3 pos, Vec3 dir, float speed, float damage, float steer, int visual, int sound_fx);
float enemy_radius(const Enemy *e); float enemy_height(const Enemy *e);
void enemy_place(Enemy *e);                                                   /* vtbl[44] 0x41a680: pos and the H angle into the instance placement */
void  enemy_sensor_init(Enemy *e);                                            /* 0x41cfc0 */
void  enemy_sensor_tick(Enemy *e, struct Player *pl);                         /* 0x41d4a0(3): Enemy::Update, only under Wander / Chase */
int   enemy_sensor_free(const Enemy *e, float ang);                            /* 0x41d2a0: the direction nearest ang is free */
float enemy_sensor_nearest_free(const Enemy *e, float ang);                    /* 0x41d310: nearest free direction, else ang */
float enemy_sensor_random_free(const Enemy *e);                               /* 0x41d2c0: a random free direction, -1 = none */
float enemy_sensor_widest_free(const Enemy *e);                               /* 0x41d390: the free direction with the widest free gap, -1 = none */

/* type 12, the bomb thrower (W2B end boss, docs/ENEMY2.md 4) */
Enemy *enemies_bomb_contact(EnemySet *s, const Enemy *owner, Vec3 a, Vec3 b, float r);   /* HitActors 0x44a0a0 for a bomb: the first thrower whose cylinder the swept sphere touches */
/* the actor hit tests of the engine (enemy.c) */
extern float g_hit_frac;                                                      /* the global hit fraction [0x53a558] */
int   sweep_sphere_cyl(Vec3 a, Vec3 b, float r, Vec3 foot, float R, float H);  /* 0x433920: sphere r swept a->b against the cylinder (foot, R, H); 2 = hit */
float seg_cyl(Vec3 a, Vec3 b, Vec3 centre, float R, float h);                 /* 0x433de0: segment against the cylinder centre +- (h - 0.1); 0.5 = hit, -1 = miss */
void enemies_actor_blast(EnemySet *s, Vec3 c, float r);                       /* rocket explosion 0x453560: vtbl[40](c, r) on the enemies of actor list 1 (thrower 12, Boss2 15) */
int  game_enemy_bomb(Enemy *e, Vec3 pos, Vec3 dir, float speed, float fuse);  /* Fire 0x411e80: 0 = no free bomb in the pool (in main_engine.c) */
void game_msgmask(Instance *in, uint32_t bits, int on);                      /* MsgMask_Set 0x443e50 / _Clear 0x443e90 on the instance's script object (in main_engine.c) */
void game_col_probe(uint32_t *cur, int on, uint32_t col, const Instance *actor);   /* 0x436dc0's Press/In/UnPress (cur = Probe+0x20; in main_engine.c) */
int  enemy_probe(Enemy *e, struct Player *pl, Vec3 pt, float tol, float *gy, int *found);   /* Probe_Test 0x436dc0 on the probe +0x178: 1 = on the ground */
void enemy_reset_probe(Enemy *e);                                             /* the probe of Enemy::Reset 0x41a010 at the start position */
/* the common move 0x41b2c0 (every behaviour tick but the path's): step or knockback, platform delta, the sphere sweep 0x437580, the Chase
 * hook 0x41bdf0 when behav == 1, then the free test; 1 = moved, 0 = blocked (the caller runs the behaviour's OnBlocked) */
int  enemy_common_move(Enemy *e, struct Player *pl, Vec3 delta, float *knock_t, Vec3 knock_dir, int behav, float dt);
void enemies_msg6_off(EnemySet *s, Instance *inst);                           /* Enemy::HandleMsg 0x41abfd: message 6 with 0 on an enemy in the world */

/* class 14, the Buzz boss (boss.c, docs/BOSS14.md) */
void boss_init(Enemy *e);                                                     /* ctor 0x40eb50 + PostLoad 0x40ec50 + factory Reset */
void boss_update(Enemy *e, struct Player *pl, Vec3 cam, float dt);            /* Think 0x41a320 -> Update 0x40eec0 */
int  boss_take_damage(Enemy *e, const Vec3 *pt, int kind);                    /* vtbl[39] 0x40fe90: always 1 hp, only in the low phase; the star at pt (kind != 2) */
void boss_reset(Enemy *e);                                                    /* vtbl[17] 0x40ed90 */
void boss_frame_end(Enemy *e);                                                /* sound source 0x468e50: the loop stops on the first frame without an update */
void enemies_boss_msg(EnemySet *s, Instance *inst, int id, uint32_t arg, Instance *linked);   /* vtbl[22] 0x410070: 59 couple, 60 mailbox */
/* implemented by the engine */
int  game_var_get(uint32_t var);
void game_var_set(uint32_t var, int v);                                       /* SetVar 0x443ca0: wakes the watchers */
float game_enemy_delay(void);                                                 /* PORT EXTRA (Gameplay page): s before a noticing enemy attacks, 0 = the original */
void game_cam_shake(float t);                                                 /* 0x41fbb0 */
void game_boss_bar(int on, int cur, int max);                                 /* 0x4484d0 */
void game_explosion(Vec3 p);                                                  /* 0x477060 kind 1 (two flash records) */
void game_boss_smoke(Instance *link, int n, int on);                          /* on: explosion 0x477060 + smoke plume 0x475f30 on marker typecode 0 nr n; n = -1, on = 0: all plumes off */

/* classes 15 and 16 (boss.c, docs/BOSS15_16.md) */
void boss15_init(Enemy *e); void boss16_init(Enemy *e);                        /* ctor + PostLoad 0x40d930 / 0x40c820 + factory Reset */
void boss15_reset(Enemy *e); void boss16_reset(Enemy *e);                      /* vtbl[17] 0x40da50 / 0x40c8f0 */
void boss15_update(Enemy *e, struct Player *pl, Vec3 cam, float dt);           /* Update 0x40dd30 */
void boss16_update(Enemy *e, struct Player *pl, Vec3 cam, float dt);           /* Update 0x40cb80 */
void boss15_blast(Enemy *e, Vec3 c, float r);                                  /* vtbl[40] 0x40e800: the only way to hurt it */
int  boss16_take_damage(Enemy *e, float dmg);                                  /* vtbl[39] 0x40d480: only while he stands visible on his pad */
void enemies_boss_links(EnemySet *s, Instance *inst, int id, int group, Instance **li, int n);   /* 61 (class 15, 8 instances) / 62 (class 16, group + 7) */
int  boss15_protects(const EnemySet *s);                                      /* vtbl[36] 0x40e710 = state 5: the Perso's Kill 0x44c110 is refused */
/* implemented by the engine */
void game_launcher_start(Instance *in);                                        /* 0x4522b0(1, 1.0, 0): one shot on the next think step */
void game_bombs_crush(Vec3 c, float r);                                        /* 0x40ea47: every bomb in fuse state 2 within r goes off (Bomb_Explode 0x44d6e0) */
void game_bombs_discard(void);                                                 /* 0x44db10 */
float game_time(void);                                                         /* World+0x30, for the instance animation clock */
/* projectile 0x4490a0 with vertical homing (T+0x44 per 1/60 s toward the target's feet + aim_h) on top of game_enemy_shot */
void game_enemy_shot_v(Enemy *owner, Vec3 pos, Vec3 dir, float speed, float damage, float steer, float vsteer, float aim_h, int visual, int sound_fx);

#endif
