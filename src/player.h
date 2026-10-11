/* player.h - player controller for the WoodyRE engine, ported from the decompiled Perso class.
 *
 * Ported (docs/PERSO_FRAME.md, PERSO_MOVE.md, PERSO_JUMP.md, CAMERA.md, EVENTS.md): Mover walk ramp and turn blend,
 * Jumper state machine (time parabolas, short hop, coyote time, terminal speed), sub-stepped cylinder sweep with
 * ground clinging and the feet+43 ground probe, animations per state, follow camera (mode 1), and the engine->VM
 * events (trigger volumes, world_collision press nodes, msgmask 0x200).
 * Attacks (0x457a50): peck dash, rebounds, charge run and brake; logical animation chains (table 0x4b6180).
 * Ducking (action 5, 0x465b10, docs/PERSO_DUCK.md) is ported, and so is the follow camera's breadcrumb trail (0x423ab0).
 * Look-around (action 7, Perso state 3 + camera mode 0x200, docs/PERSO_LOOK.md) is ported. Not ported yet: cfg key mapping. The ground type of the floor (Perso+0x308) drives the footstep effect
 * and the slippery turn ramp of type 1 (0x45a850). */
#ifndef WOODY_PLAYER_H
#define WOODY_PLAYER_H
#include "level.h"
#include "render_gl.h"
#include "ekovm.h"

typedef struct {
    int forward, back, left, right, jump, action, duck, special;   /* current key state; duck = action 5 (docs/PERSO_DUCK.md), special = action 11 (docs/PERSO_SPECIAL.md) */
    float cam_turn;                                  /* -1..1 manual camera orbit */
    float ax, az;                                    /* the stick (actions 0/1 and 2/3 values, docs/INPUT.md 3): x right, z forward; 0, 0 = only the keys above */
    int look, mouse_dx, mouse_dy;                    /* look = action 7 (docs/PERSO_LOOK.md); mouse = the relative mouse of the look camera (0x459346) */
} PlayerInput;

struct EnemySet;

/* Jumper = Perso+0x334 (docs/PERSO_JUMP.md 1.1) */
typedef struct {
    int state;                      /* J+0x14: 0 start, 1 rising, 2 grounded, 3 early fall, 4 fall, 5 long fall, 6 landed, 7 apex */
    float D, t, h_prev, v_down;     /* J+0x18, +0x1c, +0x20, +0x24 */
    float fallen, coyote_t, dy;     /* J+0x28, +0x48, J+0xc (vertical displacement this frame) */
    int armed, fell_off, hard_fall, short_hop, coyote;
    int open_window;                /* set when the air attack window opens (0x457560 from the tick) */
} Jumper;

/* the race board's spray emitter Perso+0x4b8 (0x34 B, docs/RACE.md 2.2): +0 inst, +4 n type-9 markers, +8 mode (0 off, 1 envelope,
 * 2 normal, 3 boost), +0xc active this frame, +0x10..0x18 phases, +0x1c mode-1 timer, +0x20 emission accumulator, +0x24 prev[n],
 * +0x28 size index, +0x2c has_prev[n] */
typedef struct { Instance *inst; int n, mode, active, size_idx, has_prev[4]; float ph[3], t1c, acc; Vec3 prev[4]; } BoardFx;

typedef struct Player {
    Instance *inst;                 /* the Woody instance (model 0, instance 0) */
    const GelFile *gel;
    const InsFile *ins;
    const TexFile *tex;             /* the level's textures: the ground type byte of the floor polygon's group */
    Vec3 pos, vel;                  /* pos = feet (instance origin) */
    float yaw;                      /* facing, radians; forward = (sin yaw, 0, cos yaw) */
    float speed;                    /* horizontal speed along the facing direction (Mover RampA, 0x45b110) */
    int ramp_phase; float ramp_t, ramp_target, ramp_v0;   /* 0 idle, 1 accelerating, 2 at target, 3 decelerating */
    int on_ground;
    int steep_edge;                 /* Perso+0x234, the ledge sensor 0x44b2e0: the floor drops away ahead (docs/OBSTACLE.md 2) */
    Jumper jumper;
    /* damage / death / respawn */
    float health; int lives;        /* Perso+0x24c (hearts, max 5), +0x250 */
    int dead_kind; float death_delay, invuln_respawn, invuln_hit;   /* +0x26c, +0x288, +0x270, +0x280 */
    float soft_t;                   /* PORT EXTRA (Gameplay page, soft hazards): no second heart from a laser / blast within 1 s */
    Vec3 push_dir; float push_t, push_speed;                         /* knockback (Mover RampC) */
    int game_state; float game_t; int mask10_frames;                 /* Game sequence 0x4459c0 */
    float iris_from, iris_to, iris_dur, iris_t, iris; int iris_on;  /* its iris Game+4 (0x4776b0); iris_on = drawn this tick, iris = its value */
    Vec3 spawn_pos; float spawn_yaw; Vec3 start_pos;                  /* respawn point +0x318 / facing +0x324 (checkpoint), start +0x30c */
    int bonus_got, bonus_total, bonus_count, special_charges, unique_items, race_bonus, race_total;   /* [0x5e54e8], [0x5e54e4], Perso+0x25c, +0x254, +0x260, +0x264, [0x5e54f4] */
    Vec3 ground_n, slide_dir; float slide_speed; int sliding;   /* ground normal (Mover+0xd0), RampB (Mover+0x68: dir, v +0xc) and Mover+0xdc */
    int slide_phase; float slide_target, slide_t1, slide_t2;     /* RampB phase +0x2c, target +0x10, accel / decel timers +0x24 / +0x28 */
    int ground_kind;                /* Perso+0x308 (0x4628e0): 0 normal, 1 slippery, 2 dust/sand/snow (docs/PERSO_MOVE.md 6.4) */
    int wall_contact;               /* Perso+0x2e0: the last sweep touched a wall (0x437180); speeds up the Mover's braking (0x45ae50) */
    float crush;                    /* Perso+0x2e8: squash scale of the crush test 0x462a40 (1 = none), -> inst z scale +0x54 and every body height */
    Vec3 move_dir;                  /* Mover RampA.dir (M+0x34): the walking direction; the facing except on slippery ground (0x45a850) */
    float step_u;                   /* footsteps (docs/FOOTSTEPS.md): the fraction of the walk cycle at the previous frame, -1 = not walking */
    /* attack controller (Perso+0x5b4..): sub-state, timer, displacement, air window, charge; move lock = Perso+0x238 */
    /* peck climbing, Perso state 4 (0x4651d0, docs/OBJECTS.md 1.3): sub 1 grab, 2 climbing, 3 over the top, 4 let go */
    int use_root; Vec3 root_pos;   /* climb-over: pos is frozen, the root track carries the model; camera follows root_pos */
    int climb_sub, climb_act_prev; float grip, regrab, peck_t, over_t, over_len; Vec3 wall_n, over_from, over_to; const Instance *wall_inst;
    int atk, atk9_first; float atk_t; Vec3 atk_dir, atk_disp; int use_atk_disp; float air_win, charge, move_lock, vy_corr; int action_prev;
    struct EnemySet *enemies; void *target; Vec3 dash_start, aim; int has_target;   /* attack targets (Perso+0x5f0, +0x5e4, +0x5d0, +0x5dc) */
    int lanim, lanim_sub;           /* logical animation (table 0x4b6180) and position in its chain */
    int special_st, special_prev; float special_t;   /* special attack 0x458bf0: +0x750 (0 free, 1 charging up to the hit at 1.5 s, 2 after the hit), key state, +0x74c */
    int duck, duck_anim; float duck_t;   /* ducking 0x465b10: sub-state +0x694 (0 up, 1 going down, 2 down, 3 getting up), its logical anim, timer +0x698 */
    float floor_y;                  /* last floor height found under the player */
    float ring_ground_t, ring_air_t, ring_a;   /* landing ring 0x44af90: +0x580 time on the ground, +0x584 time in the air, +0x588 its alpha 0..255 */
    int floor_is_hull;              /* floor came from an instance press node (kind 1) */
    const Instance *att_inst; uint32_t att_node; Vec3 att_local, att_world;   /* platform attachment (Perso+0x298) */
    uint32_t cur_col;               /* world_collision id currently pressed, 0xffffffff = none (Probe+0x20) */
    int ground_22c;                 /* Perso+0x22c as msgmask 0x200 sees it (getter 0x44bcf0): on_ground, except that the rocket (state 8) keeps it */
    /* volume tracking: one flag per (instance, volume node) */
    uint32_t nvol; uint8_t *inside; Instance **vol_inst; uint32_t *vol_node; uint32_t *vol_id;
    /* follow camera state */
    float cam_yaw; Vec3 cam_pos; int cam_init;
    Vec3 cam_tprev; float cam_drop, cam_quick_t; int cam_behind_prev;   /* previous target, look-point drop while airborne, action 0xa */
    int cam_state, cam_n, cam_seg; float cam_u; Vec3 cam_pad[100];     /* C+0x2a4 (2 = target hidden, follow the breadcrumbs), C+0x7a0/+0x7a4/+0x7a8, trail C+0x2f0 */
    /* death / hit animations, scripted door actions (docs/PERSO_DEATH.md) */
    float dead_T, nograv_t, hit_anim_t; int dead_ground, hit_anim, dead_cam_req;
    int script_act, script_log; float script_t, script_total; int script_faded, fade_req, cam_end_req;   /* fade_req: 1 = fade out 0.5 s, 2 = fade in 0.5 s; cam_end_req: 0x44e5a0, back to the follow camera (both consumed by the app) */
    Instance *script_carry;                                             /* +0x554: the third argument of 0x44dda0 (message 1043), carried on the action's camera-track point (0x44db76) */
    /* idle 0x464500: +0x230 seconds standing still, +0x530 which idle variation, +0x52c the zzz bubble is up (its live flag);
     * idle_hold = Perso state != 0 (results, title, frozen by a cinematic camera): the timer neither runs nor resets (set by the app) */
    float idle_t; int idle_var, sleep_bubble, idle_hold;
    int gameover_req;                                                   /* 0x44a810 found no life left: menu page 0x1d GAME OVER (consumed by the app) */
    int respawn_req;                                                    /* respawn 0x445930 happened: side view off + camera reset 0x458f90 (consumed by the app) */
    /* Perso state 8: riding a class-20 rocket (docs/ROCKET.md 6). The app fills ride_state / ride_seat / ride_q before the update */
    /* race levels (script types 18/19, docs/RACE.md): message 1120 hangs the board under the Perso (+0x4b4) and gives the track polyline (+0x4b0) */
    Instance *board; const Trajectory *race_path;
    /* Perso state 1 = riding (subtype 4/5): sub-state +0x4a8 (0 wait for the path, 1 ride, 2 crash), ride direction M+0x1c,
     * start-anim timer +0x4e4, boost +0x4c4.., stuck counter +0x4ac, lean +0x4bc/+0x4c0, crouch +0x694, up filter +0x210 */
    int race_char, race_sub, race_stuck, race_lean, race_crouch, has_ckpt;
    float race_start_t, race_lean_t, race_crouch_t, race_crash_t, boost_t, boost_speed;
    Vec3 race_dir, boost_target, race_upf, race_floor_n;              /* race_floor_n = M+0xd0 as the up filter reads it: the floor normal under him, also in the air */
    int race_cam_req;                                                   /* sub-state 0: hard cut to the follow camera (consumed by the app) */
    int board_lanim, board_lanim_sub;                                   /* the board's own anim controller +0x498 (-1 = reset, nothing requested) */
    int race_bonus_ckpt;                                                /* +0x4e0: the race bonus count at the last checkpoint, restored by SurfEnter */
    int race_snd;                                                       /* +0x4a4: sound source of the ride loop SoundFx 60 (bit 0 marked this frame, bit 1 playing) */
    /* board spray emitter +0x4b8 (0x34 B, docs/RACE.md 2.2), created by 1120, ticked and drawn by the app (0x46d040) in the frames
     * the ride marked it active; mode 2 normal, 3 boost; has_prev cleared by 1120 and SurfEnter */
    BoardFx bfx;
    /* white blinking of the invulnerability bonus (vt[26] 0x44cf50): +0x704 time left, +0x708 accumulator, +0x70c frame counter */
    float bonus_inv, bonus_inv_acc; int bonus_inv_cnt;
    float cam_dist, cam_height, cam_zoom;                               /* follow camera C+0x7e0 (400), C+0x7d8 (180), zoom cam+0x678 (1.2) */
    float cam_orbit_x, cam_orbit_y, cam_lift;                           /* PORT EXTRA: the right stick this frame (-1..1, y down = +) and the height it added */
    /* Perso state 6, carrying a bomb (docs/BOMB_CARRY.md 1): +0x590 the bomb, +0x594 in his hands, +0x58c sub-state, +0x598 its
     * timer; carry_pressed = attack just pressed this frame (read by the sub-states), throw_hold = port: the throw animation
     * keeps playing after the release (the original does that with animation priorities) */
    struct Bomb *bomb; int carrying, bsub, carry_pressed; float bt, throw_hold;
    float charge_rumble_t;          /* +0x600: the charge run's rumble clock, 0.2 s per pulse (0x457c43) */
    int state6;                     /* Perso state 6 itself (+0x21c == 6): on with the pick-up, off with SetState; normally bomb != NULL, but
                                     * the look-around bug (docs/PERSO_LOOK.md 5) gives it back without a bomb */
    /* look-around = Perso state 3 (0x44b980, docs/PERSO_LOOK.md): look_prev6 = the state it goes back to (+0x220, 6 or 0), look_key =
     * action 7 last frame, cam_mode = the camera manager's active mode (CamMgr+0x134, 0x100 = the free camera), written by the app
     * before every update. Mode 0x200 block CamMgr+0x540: facing at the start (+0x54), yaw +0x78, pitch +0x7c, deltas +0x28/+0x2c */
    int look, look_prev6, look_key, look_show, cam_mode, look_dx, look_dy; float look_yaw0, look_yaw, look_pitch;   /* look_show = +0x268 */
    int app_menu;                   /* App+0 == 0 (the App's menu state, set by the app): the look-around refusal 0x44ba4c stays silent; the
                                     * results sequence (Perso state 9) runs in it from 1140 until the save pages close (docs/PERSO_STATE9.md) */
    /* second air action 0x465e50 / 0x465fe0 (docs/PERSO_JUMP.md 1.5): subtype (typeword bits 5..9: 1 Woody, 3 Knothead = air dash,
     * 2 Splinter = double jump, 4/5 the race riders), window +0x6f8, active +0x6e4, used since the ground +0x6fd, clock +0x6ec,
     * length +0x6e8, speed +0x6f0, the 1/60 s accumulator +0x6f4, direction / last displacement +0x6d8, action 4 last frame */
    int subtype, am_on, am_used, am_jprev; float am_win, am_t, am_dur, am_speed, am_acc; Vec3 am_dir;
    /* side view (camera mode 0x20, docs/CAMERA_SCRIPT.md 4.2): side_on = Perso+0x4ec (the app copies its plane lock in before every
     * update), side_l / side_r = +0x4ed / +0x4ee (facing the left / right key's way), side_flip = CamMgr+0x63c (p+0x20), written by 0x459c70;
     * side_walk = +0x500 (the walking vector: d at the start, negated on every turn, the facing snaps to it), side_n / side_pd = the plane
     * +0x4f0..+0x4fc (n, -n.A) that 0x459eb0 pulls the displacement onto; all set by player_side_start (0x459960) */
    int side_on, side_l, side_r, side_flip; Vec3 side_walk, side_n; float side_pd;
    Instance *ride; int ride_state; Vec3 ride_seat, ride_p0; Quat ride_q, ride_q0, ride_cur; float ride_t; int ride_jprev, ride_aprev;
    /* Perso state 7 (docs/PERSO_STATE7.md): +0x55c, the object whose first type-0 vector marker carries him (message 1044 -> 0x44e140,
     * every frame 0x44e1c0, message 1045 -> 0x44e1a0); non-NULL = state 7. No shipped level script sends 1044 */
    const Instance *follow; int follow_nomark;
    /* statistics */
    float play_time;                /* Perso+0x710 accumulator (0x453ca0): seconds played in this level, one of the five result stats */
    uint32_t events_sent;
} Player;

int  player_init(Player *p, InsFile *ins, const GelFile *gel, const TexFile *tex);
void player_bind(Player *p, Instance *inst);           /* SetTypeInstance 1/2/3/18/19: this instance is the player */
void player_update(Player *p, const PlayerInput *in, float dt, EkoVM *vm, float cam_yaw);
void player_anim_settle(Player *p, const Instance *in);   /* right after `in`'s clock, before its event scan and the pose: step an anim chain whose part just ended */
void player_game_tick(Player *p, EkoVM *vm, float dt); /* 0x4459c0: level-start iris, death -> iris closes -> respawn -> iris opens */
void player_restart(Player *p);                        /* pause menu "Start again" (0x40584d): 0x445930 + race restart 0x4560f0 */
void player_camera(Player *p, FreeCamera *cam, float dt, int behind_key);   /* behind_key = action 0xa */
void player_camera_reset(Player *p);                   /* SetMode(0, 0) / message 500: put the camera behind the player now */
void player_look_start(Player *p);                     /* 0x459050: the camera enters mode 0x200 (Perso state 3 began) */
void player_look_camera(Player *p, FreeCamera *cam, float dt);   /* 0x459346 + 0x425b80: mode 0x200, the view from his eyes */
void player_lock(Player *p, float t);                  /* message 30 (0x44cde9): LockMove(t) + idle record 1 */
int  volume_contains(const Instance *inst, uint32_t node, Vec3 p);   /* 0x4300c0: is the point inside this volume node of the instance? */
void player_free(Player *p);
void player_boost(Player *p, Vec3 p0, Vec3 dir, float speed, float dur);   /* message 1121 StartBoostSurf 0x456000 */
void player_sync_board(Player *p);
void player_race_start(Player *p);                     /* level start (Game ctor 0x445850): the race's SurfEnter after the init messages */                    /* 0x44bf10 tail + 0x463e60: the race board takes the Perso's placement and animation */
/* GetHeight for other actors: ground under pt, ignoring the instance `skip` */
float player_ground_query(const Player *p, const Instance *skip, Vec3 pt, int *found);
float player_ground_query_col(const Player *p, const Instance *skip, Vec3 pt, int *found, uint32_t *col);   /* + the world_collision id under pt (0x436dc0), 0xffffffff none */
void  player_leave_all(Player *p, EkoVM *vm);                  /* 0x443ff0 leave_all: PersoLeave on every volume he is in */
float player_ground_query_n(const Player *p, const Instance *skip, Vec3 pt, int *found, Vec3 *n);   /* with the floor normal (Buzz's free test) */
float player_ground_query_hit(const Player *p, const Instance *skip, Vec3 pt, int *found, uint32_t *col, const Instance **hi, uint32_t *node);   /* + the instance / press node under pt (hit type 2), else *hi = NULL */
int   player_node_local(const Instance *in, uint32_t node, Vec3 w, Vec3 *local);   /* 0x431700: world point -> node space of in's node */
int   player_ray_full(const Player *p, Vec3 a, Vec3 b, float *t);   /* the segment ray 0x4359b0 (world + press nodes): 0 nothing, 1 world, 2 instance, 3 a inside a press node */
float player_body_height(const Player *p);            /* 0x462490 -> P+0x08: 193 standing / 61 ducked (Woody), race 160 / 81 */
void player_set_carried(const Instance *owner, const Instance *follower);   /* follower moves with owner: a query that skips owner skips it too */
/* landing ring 0x44af90 (docs/PERSO_JUMP.md 5): runs the fade by dt and gives the floor point, its normal and the sprite
 * alpha; 0 = draw nothing this frame */
int  player_landing_ring(Player *p, float dt, Vec3 *pos, Vec3 *normal, float *alpha);
int  player_collect(Player *p, int type, int arg);      /* bonus classes 30, 34..38: message 10; returns 1 when the instance must disappear */
void player_script_hold(Player *p, float t);       /* message 1040: scripted action, control taken away for t s */
void player_place(Player *p, Vec3 pos, float yaw);     /* Perso reset + SetPos + SetFacing (end of a cinematic, hub door) */
void player_cin_place(Player *p, Vec3 pos, float yaw); /* cinematic start 0x44eab0: player_place, instance upright (no race tilt) */
void player_cin_end(Player *p, int have, Vec3 pos, float yaw);   /* cinematic end 0x445af9: Perso Reset 0x44ab20, facing, 0x44a650 */
void player_volumes_actor(Player *p, EkoVM *vm, Vec3 pt, uint32_t actor);   /* a non-Perso volume actor at pt (the camera of message 800): plain enter/in/leave */
void player_ground_snap(Player *p);                    /* 0x462990: onto the floor under feet + 43, on the ground, Jumper reset */
void player_kill(Player *p, int kind);
Quat q_slerp(Quat a, Quat b, float u);
int  player_mount(Player *p, Instance *obj);           /* 0x465740: only in state 0 on the ground */
int  player_state_free(const Player *p);               /* Perso state +0x21c == 0: he has his own controls (0x44bcf0) */
void player_brake_charge(Player *p);                   /* 0x458e40: message 1042 stops the charge run the release just started */
void player_teleport(Player *p, Vec3 pos, int have_dir, Vec3 dir);   /* message 26 (0x44ce11); the caller leaves all volumes in the VM */
void player_side_start(Player *p, Vec3 a, Vec3 d, int v);   /* the Perso half of message 1088 (0x459960): facing d, placed at marker point A, plane through A */
/* message 1040 / 1140 (0x44dda0): the action number IS the raw .ins animation. 17 = into a door, 18 = out of it;
 * 10..16, 19 and 72..78 (the results animations) run with the root motion of 0x44e290. */
void player_script_action(Player *p, int act, int have, Vec3 p0, Vec3 dir);
void player_face_action(Player *p, int act, Vec3 at);   /* message 1041 (0x44e040): face `at`, ground snap, action `act` with length 0 (ends the next frame) */
void player_follow(Player *p, const Instance *obj);   /* message 1044 (0x44e140): Perso state 7, carried by obj's type-0 vector marker */
void player_follow_end(Player *p);                     /* message 1045 (0x44e1a0): +0x55c = 0, SetState(0) */
int  player_segment_blocked(const Player *p, Vec3 a, Vec3 b);   /* world polygons only */
Vec3 player_sphere_push(const Player *p, const Instance *skip, Vec3 c, float r);   /* 0x407340: world + instance press nodes */
float gel_ray_frac(const GelFile *g, Vec3 a, Vec3 b);   /* first world polygon hit on a->b as a fraction 0..1, or 2 when nothing is hit */                 /* Perso vt[38] */
int  player_ray_instances(const Player *p, const Instance *skip, Vec3 a, Vec3 b, float *frac, Vec3 *n_out, const Instance **inst_out);   /* ray 0x4359b0, instance part (hit kind 2): press-node polygons */
float gel_ray_hit(const GelFile *g, Vec3 a, Vec3 b, Vec3 *n_out);   /* the same, and the normal of that polygon, turned towards a */
int  player_ray_endless(const Player *p, Vec3 a, Vec3 dir, float *t);   /* 0x497a30: 1 nothing, 3 world polygon, 4 instance press node; *t in units of dir */
int  player_hit(Player *p, float damage, Vec3 dir);    /* Perso vt[39]; returns 1 when the caller should Kill(3) */

/* footstep effects, drawn by the app (main_engine.c) in the effect pool (docs/FOOTSTEPS.md, docs/PARTICLES.md 2/3)
 * 0x47cba0(pos, ground normal, direction, foot 0 = left / 1 = right, kind 2 or 3) twice per walk cycle, and the landing
 * dust 0x476140(pos + (0,30,0), &ground normal, 3, 0.25, 1.5) on ground type 2. game_smoke_ring is 0x476140 itself:
 * kind 0 = the big dark ring (bombs, launcher muzzles, the Buzz boss: 0, 1.5, 6.0), 3 = the white landing dust. */
void game_footstep(Vec3 pos, Vec3 normal, Vec3 dir, int foot, int kind);
void game_land_dust(Vec3 pos, Vec3 normal);
void game_smoke_ring(Vec3 pos, Vec3 normal, int kind, float t0, float life);
void game_splash(Vec3 c, float speed, float radius);   /* 0x478660, docs/SPLASH.md */
void game_special_fx(void);                            /* 0x47ab90: the streaks and fire rings of the special attack (docs/PERSO_SPECIAL.md 3) */
int  game_soft_hazards(void);                          /* PORT EXTRA (Gameplay page): Kill kinds 2 / 6 / 9 cost a heart instead of a life */
int  game_enemy_thinks(const Instance *inst);           /* is the instance in this frame's list world+0x64 (rnd_instance_list), i.e. does its Think vtbl[3] run (0x42b400)? */
uint32_t game_instance_list(Instance *const **list);   /* that list in its order (world+0x64, count +0x60); 0 and NULL without a renderer */
/* the comic speech bubble 0x478980(inst, kind, duration, offY, offX, live) (docs/PERSO_DEATH.md 4.1): kind 0 "?!" (Kill 1),
 * 1 curse (hard landing), 2 "$", 3 "...", 4 "zzz"; with `live` it lasts while *live != 0 instead of `duration` */
void game_bubble(Instance *inst, int kind, float dur, float offy, float offx, const int *live);
/* 0x42f6b0(inst, typecode, out, 0): the first vector marker with that typecode in world space, posed; P0 and dir = P1 - P0 */
int  game_inst_vector(const Instance *inst, uint32_t tc, Vec3 *p0, Vec3 *dir);
/* and the beak impact 0x479c80(kind, point, normal) (docs/PARTICLES.md 4, docs/OBJECTS.md 1.6):
 * kind 1 = a hit of the attack probe (a flash; no normal needed), 0 = the wall he is climbing (splinters and a hole) */
void game_peck_fx(int kind, Vec3 pos, const Vec3 *n);
/* the skeleton flash 0x477e40 of Kill 2 and 9 (docs/PERSO_DEATH.md 4.2, docs/PARTICLES.md 6): 1.5 s of the model and a
 * sprite skeleton taking turns, on the current player */
void game_skeleton(void);
/* 0x44f8a0 (SurfEnter): every race bonus (type 37) comes back, Respawn 0x44f8f0 = 0x407790 re-cells it */
void game_race_bonus_reset(void);
/* bombs (main_engine.c, docs/BOMB.md): pick one up (0x463430: in use, not ridden, within r of pos in 3D; it is held from now on),
 * hold it in the hand (0x463530 part A), and start its projectile again from where it is (0x44d3a0: the throw and the drop) */
struct Bomb;
struct Bomb *game_bomb_pick(Vec3 pos, float r);
void game_bomb_hold(struct Bomb *b, Vec3 pos, Quat q);
void game_bomb_launch(struct Bomb *b, Vec3 dir, float speed);

/* world queries (brute force over the .gel polygons) */
float gel_floor_below(const GelFile *g, Vec3 p, float step_up, float max_drop, int *found);

#endif
