/* main_engine.c - WoodyRE native engine prototype: loads a level (.gel/.tex/.ins + code), runs the
 * EKO CODE script VM every frame and renders world + instances with OpenGL.
 *
 * usage: woody.exe <Data dir> <LVL>            e.g. woody.exe extract/Data W1A
 * keys: the bindings of Woody.cfg when there is one (docs/INPUT.md), else arrows/WASD = walk Woody (camera-relative),
 *       Space = jump, X = duck, LCtrl/Shift = attack, RCtrl/E = special, C = camera behind, Esc = pause menu; a joystick
 *       works too. F5 = toggle free-fly camera (in fly mode WASD + right mouse = fly, Shift = fast), F1 world, F2 instances,
 *       F3 wireframe, [ ] = previous/next animation of the selected instance (default: Woody), Tab = next instance,
 *       P = pause VM. Script messages are printed to the console.
 */
#define WIN32_LEAN_AND_MEAN
#include "plat.h"
#ifdef _WIN32
#include <mmsystem.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "level.h"
#include "render_gl.h"
#include "ekovm.h"
#include "player.h"
#include "instance.h"
#include "enemy.h"
#include "audio.h"
#include "hud.h"
#include "water.h"
#include "storm.h"
#include "hnm.h"
#include "ambient.h"
#include "blackbox.h"
#include "datasetup.h"
#include "pad.h"
#include "texpack.h"
#include "gtao.h"
#include "postfx.h"
#ifdef WOODY_GUI
#define WOODY_DEBUG_TITLE 0                     /* the release build keeps the plain window title */
#define WOODY_DEBUG_KEYS (wenv("WOODY_DEBUGKEYS") != NULL)   /* and the developer keys only on request */
#else
#define WOODY_DEBUG_TITLE 1                     /* level, fps, VM clock and player state in the title */
#define WOODY_DEBUG_KEYS 1                      /* F1-F5, Tab, [ ], P, PgUp / PgDn (level skip), End (finish the level) */
#endif
#define DBGKEY(k) (WOODY_DEBUG_KEYS && win.keys[k])

static InsFile g_ins;
static int g_log_msgs = 1;
static Player *g_player;
static const Renderer *g_rnd; static const GelFile *g_gel;              /* the current level's renderer and world, for game_enemy_thinks() */
static EnemySet g_enemies;
static float g_now; static double g_clock;           /* game time in seconds (VM time base): World+0x20 / +0x30 (0x401880), the sum of the CLAMPED frame times */
static float g_dt;                                     /* this frame's dt, World+0x38; 0 while a level loads (its init messages) */
/* messages 12/13 wait for the running animation to end: offered again every frame (max 32 in the original, 0x4012f0 clears) */
static EkoMsg g_retry[32]; static int g_nretry;
static float inst_yaw(const Instance *in) { Vec3 f = mat4_apply(&in->world, (Vec3){ 0, -1, 0 }); return atan2f(f.x - in->position.x, f.z - in->position.z); }   /* as player_bind */
static Instance *slot_instance(uint32_t ref) { if ((ref >> 24) > 1) return NULL;   /* scripts also use bare slot numbers; the dispatcher 0x401370 only masks & 0xffffff */
    uint32_t i = ref & 0xffffff; return i < g_ins.nslots + 16 ? g_ins.slots[i] : NULL; }

/* level table 0x4b12a0 (docs/GAMEFLOW.md 1): 0 House (menu backdrop), 1 WWS hub, 2..10 Woody, 11 KWS hub, 12..17 Knothead,
 * 18 SWS hub, 19..24 Splinter, 25 BlackBox, 26 Credits, 27 dev slot, 28 Lang */
static const char *k_levels[29] = { "House", "WWS", "W1A", "W1B", "W2A", "W2B", "W2D", "W3A", "W3B", "W3C", "W3D", "KWS", "K1A", "K1R", "K2A", "K2R", "K3A", "K3R",
                                    "SWS", "S1A", "S1R", "S2A", "S2R", "S3A", "S3R", "BlackBox", "Credits", "BlackBox", "Lang" };
static int g_level = 27, g_prev_level = 27;           /* app+0x68 / +0x6c (init 0x1b) */
/* RequestLevel 0x404b60(fade_s, level, state, page): fade out, then the main loop swaps the level */
static int g_next_level = -1; static float g_fade_len = 0.5f, g_switch_fade = 0.0f;   /* every level load fades in over 1 s (0x404332), the first one too */
static void request_level(int index, float fade_s) { if (g_next_level < 0 && index >= 0 && index < 29) { g_next_level = index; g_fade_len = fade_s > 0.01f ? fade_s : 0.01f; audio_music_stop(g_fade_len * 0.9f); audio_rtc(-1); } }   /* 0x404b95 */
static int level_index(const char *name) { for (int i = 0; i < 29; i++) if (!_stricmp(k_levels[i], name)) return i; return -1; }

/* real time cinematic state (docs/CINEMATIC.md, object game+0x64); the update is further down */
static struct { int state, anim, nactors; Instance *main_inst, *vec; struct { Instance *inst; int anim; } actor[32]; uint32_t var; float remain, timer; int rtc; } g_cin;
static int cin_running(void) { return g_cin.state == 2 || g_cin.state == 3; }               /* 0x44f2e0: Perso update skipped */

static uint32_t msvc_rand(void *user);
/* ---- camera manager: follow camera (mode 1, player.c) + the fixed script cameras (docs/CAMERA_SCRIPT.md, CAMERA.md 4 and 6.1)
 * mode 2 (message 510) / mode 4 (520, letterbox, player frozen): camera at the .ins camera position looking at
 * target origin + (0, f, 0). A mode change blends linearly from the frozen old camera unless the script asked for a cut. */
typedef struct { float t, T, diff, start, tgt; } SvRamp;       /* one ramp of the side view: elapsed, duration, target - start, start, target */
static struct {
    int mode; Vec3 fix_pos; Instance *fix_target; float fix_f;
    float dur, speed; int dur_from_speed, cut;                  /* 570 / 560 / 580 */
    int active; Vec3 from_pos, look_from, look_cur, look_off; float elapsed, t;
    Vec3 pos;                                                    /* camera position of the last frame */
    const Trajectory *rail; float rail_d, rail_y; Vec3 rail_pt; int rail_first;   /* mode 8 (540): camera on a TRAJ at distance d from the player */
    /* mode 0x20 (game messages 1088 + 1110, CAMERA_SCRIPT.md 4.2): side view of a section where the player is kept on a vertical plane */
    int plane_on, side; Vec3 plane_a, plane_d; float sv_par[8];   /* sv_par: lat 1000, ahead 300, h 340, h_up 500, h_down 0, rate_h 400, rate_a 700, rate_lat 200 */
    float sv_a, sv_h, sv_lat, sv_htarget;                        /* S+0x28c A, +0x290 H, +0x294 Lat; p+0x24 the height target */
    SvRamp sv_ra, sv_rh, sv_rl;                                  /* the linear ramps of A (S+0x368..0x378), H (+0x354..0x364), Lat (+0x37c..0x38c) */
    float sv_blend, sv_bt, sv_bstart, sv_bT; int sv_bsign;        /* 0x4250b0: S+0x340 blend, +0x348 t, +0x34c start, +0x350 duration, +0x344 sign byte */
    int death_cam;                                               /* engine use of mode 2 (0x41fb50 from 0x459030): the camera stops and watches the player fall */
    /* mode 0x80 (docs/CAMERA_SCRIPT.md 4.3): the camera comes from a camera track in the animation an instance plays */
    Instance *anim_inst; int anim_letterbox; Vec3 anim_eye, anim_tgt;   /* CamMgr+0x5d4, +0x618 & 2, +0x1d0, +0x5d8 */
    float shake;                                                 /* +0x67c: remaining shake time (0x41fbb0), docs/CAMERA.md 6.3 */
    int look_prev;                                               /* 0x459090 ctl+8, as far as state 3 goes: was the Perso looking around last frame */
    int fsaved; float fsave_d, fsave_h;                          /* CamMgr+0 / +4 / +8: follow-camera distance and height saved once by message 650 (ctor 0x41dd67: 0, 120, 50) */
    int autozoom;                                                /* CamMgr+0x66c bit 2 (message 710, 0x41f650): zoom by distance in 0x41f690; cleared by the camera reset 0x41df70 */
    Camera *actor;                                               /* CamMgr+0x664 (message 800): a .ins camera object that takes the camera position every frame and is a volume actor */
} g_cam = { 1 };
static const float k_sv_defaults[8] = { 1000, 300, 340, 500, 0, 400, 700, 200 };
static Camera *slot_camera(uint32_t ref) { uint32_t i = ref & 0xffffff; return i < g_ins.nslots + 16 ? g_ins.cam_slots[i] : NULL; }
/* 0x424b30, the init of mode 0x20 that SetMode (0x41f56b -> 0x41e4e0) runs on EVERY SetMode(5), also the re-entry of 0x459960:
 * flip byte p+0x20 = 0, A = p+0x2c and H = Htarget = p+0x30 and Lat = p+0x28 at once (no ramp at the start), blend 1.0 with the
 * sign byte 1 (look ahead along +p->dir), the flip blend and the three ramps zeroed */
static void cam_side_init(void)
{
    const float *q = g_cam.sv_par;
    if (g_player) g_player->side_flip = 0;
    g_cam.sv_a = q[1]; g_cam.sv_htarget = q[2]; g_cam.sv_h = g_cam.sv_htarget; g_cam.sv_lat = q[0];
    g_cam.sv_blend = 1.0f; g_cam.sv_bsign = 1; g_cam.sv_bt = g_cam.sv_bstart = g_cam.sv_bT = 0;
    memset(&g_cam.sv_ra, 0, sizeof g_cam.sv_ra); memset(&g_cam.sv_rh, 0, sizeof g_cam.sv_rh); memset(&g_cam.sv_rl, 0, sizeof g_cam.sv_rl);
}
static void cam_set_mode(int mode)                               /* SetMode 0x41f410 + 0x41eaa0 */
{
    if (wenv("WOODY_CAMLOG") && mode != g_cam.mode) printf("  CAM mode %d -> %d (%s)\n", g_cam.mode, mode, g_cam.cut ? "cut" : "travelling");
    if (!g_cam.cut) {
        g_cam.look_from = g_cam.active ? g_cam.look_cur : g_cam.look_off; g_cam.from_pos = g_cam.pos;
        g_cam.active = 1; if (g_cam.dur <= 0) g_cam.dur = 2.0f; g_cam.elapsed = 0; g_cam.t = 0;
    } else g_cam.active = 0;
    g_cam.mode = mode; if (mode != 0x80) g_cam.anim_inst = NULL;
    if (mode == 1 && g_player) g_player->cam_init = 0;
    if (mode == 0x20) cam_side_init();                           /* 0x41f56b: 0x41e4e0 -> 0x424b30 */
}
/* 0x41df70, the camera reset of 0x458f90 (teleport 26, respawn) and of the level start 0x402b0d: shake off (+0x67c), zoom 1.2
 * (0x41f680), auto-zoom off (+0x66c &= ~4), follow camera state CENTER (0x422350, the cam_init of the mode switch) */
static void cam_reset(void) { g_cam.shake = 0; g_cam.autozoom = 0; if (g_player) g_player->cam_zoom = 1.2f; }
static void cam_hard_reset(void) { cam_reset(); g_cam.cut = 1; cam_set_mode(1); }   /* 0x458f90: the reset, then the hard cut to the follow camera */
/* 0x44de44: a Perso state change (scripted action, teleport, cinematic, death) ends the side view's plane lock */
static void plane_release(void) { if (!g_cam.plane_on) return; g_cam.plane_on = 0; if (g_cam.mode == 0x20) { g_cam.cut = 1; cam_set_mode(1); } puts("  side view: plane lock released"); }
static void cam_side_start(Instance *in, int v)                   /* Perso::0x459960 */
{
    if (g_cam.plane_on) { if (g_cam.mode != 0x20) { g_cam.cut = 1; cam_set_mode(0x20); } return; }
    const Model *mo = in->model; int node = -1;                    /* marker node (kind 0x20) with typecode 0: two points A, B (0x42f6b0) */
    for (uint32_t i = 0; i < mo->nnodes && node < 0; i++) if (mo->nodes[i].kind == 0x20 && mo->nodes[i].type_code == 0 && mo->nodes[i].npoints >= 2) node = (int)i;
    if (node < 0) { printf("1088: instance %u has no marker\n", in->index); return; }
    ins_pose(in, in->anim, in->anim_time);
    Vec3 A = ins_point_world(in, mo->nodes[node].point_base), B = ins_point_world(in, mo->nodes[node].point_base + 1), d = { B.x - A.x, 0, B.z - A.z };
    float l = sqrtf(d.x * d.x + d.z * d.z); if (l < 0.01f) d = (Vec3){ 1, 0, 0 }; else { d.x /= l; d.z /= l; }
    g_cam.plane_on = 1; g_cam.plane_a = A; g_cam.plane_d = d; g_cam.side = v == 1 ? 0 : 1;
    if (g_player) player_side_start(g_player, A, d, v);           /* facing d, feet onto A (not in state 5), +0x4ed/+0x4ee, plane, walk vector */
    memcpy(g_cam.sv_par, k_sv_defaults, sizeof k_sv_defaults);
    g_cam.cut = 1; cam_set_mode(0x20);                             /* 0x459baf / 0x459bba; the mode init 0x424b30 sets A, H, Lat and the blend */
}
/* WOODY_MSGUNK=1: every message id the port does not handle, once per id (with the args of that first send) */
static void msg_unknown(const EkoMsg *m, const char *what)
{
    static uint8_t seen[2048 / 8]; if (!wenv("WOODY_MSGUNK") || m->id >= 2048 || (seen[m->id >> 3] >> (m->id & 7) & 1)) return;
    seen[m->id >> 3] |= (uint8_t)(1 << (m->id & 7));
    printf("  MSGUNK %u (%s) [", m->id, what); for (uint32_t i = 0; i < m->nargs; i++) printf("%s0x%x", i ? ", " : "", m->args[i]); printf("]\n");
}
static void cam_msg(const EkoMsg *m, const Camera *c)
{
    int a1 = m->nargs > 1 ? (int)m->args[1] : 0;
    switch (m->id) {
    case 500: case 501: cam_set_mode(1); g_cam.plane_on = 0; break;   /* the plane lock ends with a Perso state change (0x44de44); here: with the camera */                  /* 501 (camera in front of the player) starts behind as well */
    case 510: case 520: {
        Instance *t = m->nargs > 2 ? slot_instance(0x1000000 | (m->args[2] & 0xffffff)) : NULL; if (!t) break;
        g_cam.fix_pos = c->position; g_cam.fix_f = (float)a1; g_cam.fix_target = t; cam_set_mode(m->id == 510 ? 2 : 4); break; }
    case 540: if (wenv("WOODY_CAMLOG")) for (uint32_t i = 0; i < c->traj.npoints; i++) printf("rail %u: %.0f %.0f %.0f closed %d", i, c->traj.points[i].x, c->traj.points[i].y, c->traj.points[i].z, c->traj.closed), puts("");
        if (c->traj.npoints >= 2) { g_cam.rail = &c->traj; g_cam.rail_d = (float)a1; g_cam.rail_first = 1; cam_set_mode(8); } break;
    case 560: g_cam.speed = (float)a1; g_cam.dur_from_speed = 1; break;
    case 570: g_cam.dur = a1 * 0.01f; g_cam.dur_from_speed = 0; break;
    case 580: g_cam.cut = a1 == 2; break;
    /* the follow-camera parameters (vtable[22] 0x498bd0, byte table 0x498f00; docs/CAMERA.md 4). They live in the camera
     * manager, not in the camera object the script names: every script passes some camera of its own as arg 0. */
    case 650: if (g_player) { if (!g_cam.fsaved) { g_cam.fsaved = 1; g_cam.fsave_d = g_player->cam_dist; g_cam.fsave_h = g_player->cam_height; }   /* 0x41fab0: saved ONCE per level */
                              g_player->cam_init = 0; } break;                                                  /* 0x422350 state CENTER + 0x4247f0(0): re-seated behind the player */
    case 660: if (g_player && g_cam.fsaved) { g_player->cam_dist = g_cam.fsave_d; g_player->cam_height = g_cam.fsave_h; } break;   /* 0x41fb00: restore (nothing before a 650) */
    case 670: if (g_player) g_player->cam_height = (float)a1; break;                                        /* 0x41fa60: C+0x7d8, height above the target point (180) */
    case 680: if (g_player) g_player->cam_dist = (float)a1; break;                                          /* 0x41fa80: C+0x7e0 = +0x7e4 = +0x280, the distance band (400) */
    case 690: if (g_player && a1 > 0) g_player->cam_zoom = a1 * 0.01f; break;                               /* 0x41f660 (no level sends it) */
    case 700: if (g_player) g_player->cam_zoom = 1.2f; break;                                               /* 0x41f680 (no level sends it) */
    case 710: g_cam.autozoom = 1; break;                                                                    /* 0x41f650 */
    case 800: g_cam.actor = (Camera *)c; break;                                                             /* 0x498d93: CamMgr+0x664 = the object in arg 0 itself */
    default: msg_unknown(m, "camera"); break;                                                               /* 530, 550, 590, 600: no level sends them */
    }
}
/* ---- rail camera, mode 8 (message 540 [cam, d]; docs/CAMERA.md 6.2): update 0x421570, fallback 0x420e40 ----------------- */
static float v3d2(Vec3 a, Vec3 b) { float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z; return x * x + y * y + z * z; }
/* 0x421c00 (xz = 0) / 0x421e80 (xz = 1, the y of both vectors zeroed: a vertical cylinder): where segment A-B meets the sphere of
 * radius r around c. s1 <= s2 are the roots; one outside [0,1] is replaced by the other, none inside = no hit. The points are
 * A + (B - A) s in 3D either way. */
static int rail_seg_sphere(Vec3 c, float r, Vec3 A, Vec3 B, int xz, Vec3 *o1, Vec3 *o2)
{
    Vec3 v = { B.x - A.x, xz ? 0 : B.y - A.y, B.z - A.z }, w = { A.x - c.x, xz ? 0 : A.y - c.y, A.z - c.z };
    float a = v.x * v.x + v.y * v.y + v.z * v.z, b = 2 * (w.x * v.x + w.y * v.y + w.z * v.z), C = w.x * w.x + w.y * w.y + w.z * w.z - r * r, D = b * b - 4 * a * C;
    if (a < 1e-9f || D < 0) return 0;                                  /* a == 0 (a doubled rail point) divides by zero in the original */
    float sq = sqrtf(D), s1 = (-b - sq) / (2 * a), s2 = (sq - b) / (2 * a);
    if (s1 < 0) { if (s2 < 0) return 0; s1 = s2; }
    if (s2 < 0 && s1 >= 0) s2 = s1;
    if (s1 > 1) { if (s2 > 1) return 0; s1 = s2; }
    if (s2 > 1 && s1 <= 1) s2 = s1;
    *o1 = (Vec3){ A.x + (B.x - A.x) * s1, A.y + (B.y - A.y) * s1, A.z + (B.z - A.z) * s1 };
    *o2 = (Vec3){ A.x + (B.x - A.x) * s2, A.y + (B.y - A.y) * s2, A.z + (B.z - A.z) * s2 };
    return 1;
}
/* The point the rail camera wants this frame. Only the count - 1 segments of the polyline are walked (the closed flag of the
 * TRAJ is not read). A segment that meets the sphere of radius d around the player c gives its entry point o1 as long as no
 * earlier segment did (and always on the first frame, so there the LAST such segment wins); after that both o1 and o2 compete
 * by their distance to Q, the camera of the previous frame. No segment meets it (d is often smaller than the rail's distance
 * to the player: 540 [cam, 50] is "the point of the rail abeam of him") -> 0x420e40. */
static int rail_pick(const Trajectory *tr, Vec3 c, float d, Vec3 Q, int first, Vec3 *out)
{
    int found = 0; float best = 0;
    for (uint32_t i = 0; i + 1 < tr->npoints; i++) {
        Vec3 o1, o2; if (!rail_seg_sphere(c, d, tr->points[i], tr->points[i + 1], 0, &o1, &o2)) continue;
        if (!first && found) { float e1 = v3d2(o1, Q), e2; if (e1 < best) { *out = o1; best = e1; } e2 = v3d2(o2, Q); if (e2 < best) { *out = o2; best = e2; } }
        else { *out = o1; best = v3d2(o1, Q); }
        found = 1;
    }
    return found;
}
/* 0x420e40: per segment the point nearest to the player; a segment nearer than the best so far is taken, and when it passes
 * within 100 of him in xz (0x421e80) the camera goes to where it crosses that cylinder, on the side of the current rail point R,
 * so it never sits on his head. The running "best" then holds that point's distance to R - the original compares the next
 * segment's distance to the player against it. */
static Vec3 rail_nearest(const Trajectory *tr, Vec3 c, Vec3 R)
{
    Vec3 cand = tr->points[0]; float best = 0; int have = 0;
    for (uint32_t i = 0; i + 1 < tr->npoints; i++) {
        Vec3 A = tr->points[i], B = tr->points[i + 1], v = { B.x - A.x, B.y - A.y, B.z - A.z }, q;
        float tb = v.x * (c.x - B.x) + v.y * (c.y - B.y) + v.z * (c.z - B.z), ta = v.x * (c.x - A.x) + v.y * (c.y - A.y) + v.z * (c.z - A.z);
        if (tb > 0) q = B; else if (ta < 0 || !(ta > tb)) q = A;         /* ta == tb: a doubled point, 0/0 would make the camera NaN for good */
        else { float s = ta / (ta - tb); q = (Vec3){ A.x + v.x * s, A.y + v.y * s, A.z + v.z * s }; }
        float e = v3d2(q, c); if (have && !(e < best)) continue;
        Vec3 o1, o2;
        if (rail_seg_sphere(c, 100.0f, A, B, 1, &o1, &o2)) { cand = v3d2(R, o2) <= v3d2(R, o1) ? o2 : o1; best = v3d2(R, cand); }
        else { cand = q; best = e; }
        have = 1;
    }
    return cand;
}
/* 0x425300 (A) = 0x425220 (H) = 0x4253e0 (Lat): a linear ramp in time, restarted from the current value when it is idle (t == 0)
 * or the target moved; the first call leaves the value where it is. f = t / T; f > 1 snaps to the target and idles the ramp
 * (0x4253c4), otherwise value = start + (target - start) f and t += dt. Called only while value != target. */
static void sv_ramp(float *v, float target, float rate, SvRamp *r, float dt)
{
    if (r->t == 0 || r->tgt != target) {                                          /* 0x425300..0x425386 */
        r->t = 0; r->tgt = target; r->diff = target - *v; r->T = r->diff / rate; if (r->T < 0) r->T = -r->T; r->start = *v;
    }
    float f = r->t / r->T;
    if (f > 1.0f) { r->t = 0; *v = target; return; }                              /* 0x42539a: C0 / C3 clear */
    *v = f * r->diff + r->start; r->t += dt;
}
/* 0x4250b0(dt, &ahead), ahead = the unit walking direction p->dir on entry. A < 0.001: ahead stays the unit vector (unscaled, no
 * sign) and a flip only toggles the sign byte. Otherwise ahead = (x A, 0, z A); on p->flip (p+0x20, one frame, 0x459c70) the sign
 * byte S+0x344 toggles, start = -blend (0x425141; the 1.0 test at 0x42512b gives the same -1), blend = -1, t = 0, duration
 * T = |ahead| / p+0x40 * (1 - start): so s = +-blend is continuous across the flip, and the look point then sweeps linearly from
 * where it was to the other side at the rate of A (700 u/s). While blend != 1.0 (bitwise, 0x425194): blend = start + (1 - start)
 * t / T, capped at 1 (0x4251ce), t += dt AFTER the use (the flip frame itself shows start). ahead *= (sign ? blend : -blend). */
static void sv_ahead(Vec3 *ahead, int flip, float rate, float dt)
{
    if (!(g_cam.sv_a >= 0.001f)) { if (flip) g_cam.sv_bsign = !g_cam.sv_bsign; return; }   /* 0x4250b0..0x4250e5 (0x4a94c4 = 0.001) */
    ahead->x *= g_cam.sv_a; ahead->y = 0; ahead->z *= g_cam.sv_a;
    if (flip) {                                                                   /* 0x42510c */
        g_cam.sv_bsign = !g_cam.sv_bsign;
        g_cam.sv_bstart = g_cam.sv_blend == 1.0f ? -1.0f : -g_cam.sv_blend;
        g_cam.sv_blend = -1.0f; g_cam.sv_bt = 0;
        g_cam.sv_bT = sqrtf(ahead->x * ahead->x + ahead->z * ahead->z) / rate * (1.0f - g_cam.sv_bstart);
    }
    if (g_cam.sv_blend != 1.0f) {                                                 /* 0x42518d */
        /* T = 0 happens when the flip byte stays up for a second frame (0x459c70 skipped under a move lock): start = -(-1) = 1, and
         * the original divides 0 / 0 at 0x4251a1 - a NaN that then sticks in the blend until the next SetMode(5) (derived). The
         * port takes t / T = 1 there: blend 1, s unchanged. The same for a NaN or negative T: 1110 [6, 0] (rate 0, the scripts send
         * it from a variable) gives T = x / 0 = inf, then inf * (1 - 1) = NaN on the next flip, a NaN camera until SetMode(5) */
        float f = g_cam.sv_bT > 0 ? g_cam.sv_bt / g_cam.sv_bT : 1.0f;
        float v = f * (1.0f - g_cam.sv_bstart) + g_cam.sv_bstart;
        g_cam.sv_blend = v > 1.0f ? 1.0f : v;
        g_cam.sv_bt += dt;
    }
    float s = g_cam.sv_bsign ? g_cam.sv_blend : -g_cam.sv_blend;                  /* 0x4251e8 */
    ahead->x *= s; ahead->y *= s; ahead->z *= s;
}
static void cam_update(Player *p, FreeCamera *cam, float dt, int behind_key)
{
    Vec3 P, T;
    if (g_cam.mode == 0x200) {                                   /* look-around (docs/PERSO_LOOK.md 3): eye camera, entered with a cut, no transition, no shake */
        player_look_camera(p, cam, dt); g_cam.pos = cam->pos; g_cam.active = 0; g_cam.look_off = (Vec3){ 0, 0, 0 }; p->cam_yaw = cam->yaw; return;
    }
    if (g_cam.mode == 0x80 && g_cam.anim_inst) {                 /* 0x41f1ee: camera from the animation of CamMgr+0x5d4 (0x42fa80): cut, no smoothing.
                                                                  * That instance is the cinematic's main instance (0x44ed3f, letterboxed) or the Perso
                                                                  * himself during a scripted door action (0x44df7d, never letterboxed). */
        Instance *I = g_cam.anim_inst; const Model *mo = I->model; Vec3 eye, tgt;
        float L = (uint32_t)I->anim < mo->nanims && mo->anims[I->anim].duration_s > 0 ? mo->anims[I->anim].duration_s : 1.0f;
        if (ins_camera_eval(I, I->anim, I->anim_time / L, &eye, &tgt)) { g_cam.anim_eye = eye; g_cam.anim_tgt = tgt; }
        /* 0x41f21d sits inside the "has a camera track" test and 0x41f240 outside it, so an animation without one
         * leaves the previous eye and target standing: the camera holds that frame instead of snapping elsewhere */
        {   Vec3 e = g_cam.anim_eye, t = g_cam.anim_tgt, to = { t.x - e.x, t.y - e.y, t.z - e.z };
            cam->pos = e; cam->yaw = atan2f(to.x, to.z); cam->pitch = atan2f(to.y, sqrtf(to.x * to.x + to.z * to.z));
            cam->letterbox = g_cam.anim_letterbox; cam->fov_deg = 2.0f * atanf(p->cam_zoom * (g_cam.anim_letterbox ? 0.5625f : 0.75f)) * 57.29578f;
            g_cam.pos = e; g_cam.active = 0; return; }
    }
    if (g_cam.mode == 0x20 && g_cam.plane_on) {                  /* 0x424bf0 */
        const float *q = g_cam.sv_par; Vec3 d = g_cam.plane_d, sidev = { -d.z, 0, d.x };   /* (0,-1,0) x dir */
        /* 0x424d4e: p->h (p+4, written by the Perso in 0x459c70) picks the height target p+0x24: 0 (up key) -> p+0x34, 1 -> p+0x30,
         * 2 (down / duck) -> p+0x38 */
        g_cam.sv_htarget = behind_key == 2 ? q[3] : behind_key == 3 ? q[4] : q[2];
        /* each ramp only runs while the value differs from its target (0x424d7f, 0x424d9d, 0x424dbb); the same dt as the VM frame */
        if (g_cam.sv_a != q[1]) sv_ramp(&g_cam.sv_a, q[1], q[6], &g_cam.sv_ra, dt);                            /* 0x425300, rate p+0x40 */
        if (g_cam.sv_h != g_cam.sv_htarget) sv_ramp(&g_cam.sv_h, g_cam.sv_htarget, q[5], &g_cam.sv_rh, dt);    /* 0x425220, rate p+0x3c */
        if (g_cam.sv_lat != q[0]) sv_ramp(&g_cam.sv_lat, q[0], q[7], &g_cam.sv_rl, dt);                        /* 0x4253e0, rate p+0x44 */
        float lat = g_cam.side == 0 ? -g_cam.sv_lat : g_cam.sv_lat;                                          /* 0x424dd3 */
        Vec3 ahead = d; sv_ahead(&ahead, p->side_flip, q[6], dt);                                              /* 0x4250b0 */
        Vec3 C = { p->pos.x + ahead.x, p->pos.y + ahead.y + g_cam.sv_h, p->pos.z + ahead.z };                  /* 0x424e30 / 0x424e35 */
        P = (Vec3){ C.x + sidev.x * lat, C.y, C.z + sidev.z * lat }; T = p->pos; g_cam.look_off = (Vec3){ C.x - T.x, C.y - T.y, C.z - T.z };
        if (wenv("WOODY_SIDELOG") && (p->side_flip || (int)(p->play_time * 10) != (int)((p->play_time - dt) * 10)))
            printf("  SIDE t %.2f flip %d A %.1f H %.1f Lat %.1f blend %.3f sign %d s %+.3f ahead %.1f %.1f\n", p->play_time, p->side_flip, g_cam.sv_a, g_cam.sv_h, g_cam.sv_lat,
                   g_cam.sv_blend, g_cam.sv_bsign, g_cam.sv_bsign ? g_cam.sv_blend : -g_cam.sv_blend, ahead.x, ahead.z);
    } else if (g_cam.mode == 8 && g_cam.rail) {
        /* 0x421570. c = p+0x20 = the player's position (vt[34], his feet) that 0x459090 writes every frame, its y filtered
         * y = (1-k) y + k y_prev with k = 0.95^(30 dt) and frozen while he rises or falls (p+0x50 bits 2/3) */
        int js = p->jumper.state, air = js == 0 || js == 1 || js == 7 || js == 3 || js == 4;
        if (g_cam.rail_first) g_cam.rail_y = p->pos.y; else if (!air) { float k = powf(0.95f, 30.0f * dt); g_cam.rail_y = (1 - k) * p->pos.y + k * g_cam.rail_y; }
        Vec3 c = { p->pos.x, g_cam.rail_y, p->pos.z }, want;
        if (!rail_pick(g_cam.rail, c, g_cam.rail_d, g_cam.rail_first ? g_cam.pos : g_cam.rail_pt, g_cam.rail_first, &want)) {
            c.y = p->pos.y;                                              /* 0x42181e: the fallback looks at the unfiltered height (the filter state goes on) */
            want = rail_nearest(g_cam.rail, c, g_cam.rail_pt);
        }
        Vec3 d = { want.x - g_cam.rail_pt.x, want.y - g_cam.rail_pt.y, want.z - g_cam.rail_pt.z }; float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z), step = 1000.0f * dt;
        if (g_cam.rail_first || len <= step) g_cam.rail_pt = want; else { g_cam.rail_pt.x += d.x / len * step; g_cam.rail_pt.y += d.y / len * step; g_cam.rail_pt.z += d.z / len * step; }
        g_cam.rail_first = 0;
        /* the camera looks straight at c: the look-at of 0x421985 aims at p+0x20, and the offset the transition blends,
         * CamMgr+0xc4 = (0, p+0x1c, 0), is 0 - both exits of 0x421570 write p+0x1c = 0 (0x42182f, 0x421bd6) */
        P = g_cam.rail_pt; T = c; g_cam.look_off = (Vec3){ 0, 0, 0 };
    } else
    if (g_cam.mode == 1 || !g_cam.fix_target) {
        player_camera(p, cam, dt, behind_key); P = cam->pos; T = p->pos; g_cam.look_off = (Vec3){ 0, 140.0f - p->cam_drop, 0 };
    } else {
        P = g_cam.fix_pos; T = g_cam.fix_target == p->inst ? p->pos : g_cam.fix_target->position; g_cam.look_off = (Vec3){ 0, g_cam.fix_f, 0 };
    }
    Vec3 look = { T.x + g_cam.look_off.x, T.y + g_cam.look_off.y, T.z + g_cam.look_off.z };
    if (g_cam.active) {                                          /* Transition_Travelling 0x41eb90 */
        Vec3 d = { P.x - g_cam.from_pos.x, P.y - g_cam.from_pos.y, P.z - g_cam.from_pos.z };
        if (g_cam.dur_from_speed) { g_cam.dur = g_cam.speed > 0 ? sqrtf(d.x * d.x + d.y * d.y + d.z * d.z) / g_cam.speed : 2.0f; g_cam.dur_from_speed = 0; }
        float t = g_cam.t;
        P = (Vec3){ g_cam.from_pos.x + d.x * t, g_cam.from_pos.y + d.y * t, g_cam.from_pos.z + d.z * t };
        g_cam.look_cur = (Vec3){ g_cam.look_from.x + (g_cam.look_off.x - g_cam.look_from.x) * t, g_cam.look_from.y + (g_cam.look_off.y - g_cam.look_from.y) * t, g_cam.look_from.z + (g_cam.look_off.z - g_cam.look_from.z) * t };
        look = (Vec3){ T.x + g_cam.look_cur.x, T.y + g_cam.look_cur.y, T.z + g_cam.look_cur.z };
        g_cam.t = g_cam.dur > 0 ? g_cam.elapsed / g_cam.dur : 1; if (g_cam.t > 1) g_cam.t = 1;
        g_cam.elapsed += dt; if (g_cam.elapsed > g_cam.dur) g_cam.active = 0;
    }
    if (g_cam.shake > 0) {                                       /* 0x41fbd0 after the mode update, modes 1 2 4 8 0x10 0x20 (0x41ffb4): only the view direction trembles, +-5 min(t,3) per axis */
        int m = g_cam.mode; if (m == 1 || m == 2 || m == 4 || m == 8 || m == 0x10 || m == 0x20) {
            float a = g_cam.shake < 3.0f ? g_cam.shake : 3.0f;
            look.x += (float)((int)(msvc_rand(NULL) % 1000) - 500) * 0.01f * a; look.y += (float)((int)(msvc_rand(NULL) % 1000) - 500) * 0.01f * a; look.z += (float)((int)(msvc_rand(NULL) % 1000) - 500) * 0.01f * a; }
        g_cam.shake -= dt;
    }
    Vec3 to = { look.x - P.x, look.y - P.y, look.z - P.z };
    cam->pos = P; cam->yaw = atan2f(to.x, to.z); cam->pitch = atan2f(to.y, sqrtf(to.x * to.x + to.z * to.z));
    if (g_cam.autozoom) {                                        /* 0x41f690: zoom from |CamMgr+0x278 - camera|, 1.1 up to 300, 0.2 from 3000 on */
        float dx = T.x - P.x, dy = T.y - P.y, dz = T.z - P.z, d = sqrtf(dx * dx + dy * dy + dz * dz), t = d < 300.0f ? 0 : d > 3000.0f ? 1 : (d - 300.0f) * (1.0f / 2700.0f);
        p->cam_zoom = (1 - t) * 1.1f + t * 0.2f;
    }
    /* tan(vfov/2) = zoom * sy: zoom CamMgr+0x678 (1.2; the race 1.5, messages 690/700/710), sy 0.5625 with the letterbox of mode 4, else 0.75 */
    cam->letterbox = g_cam.mode == 4; cam->fov_deg = 2.0f * atanf(p->cam_zoom * (g_cam.mode == 4 ? 0.5625f : 0.75f)) * 57.29578f;
    g_cam.pos = P; p->cam_yaw = cam->yaw;                         /* movement stays relative to the camera on screen */
}

/* script screen faders (app+0x9c, 0x401440 / 0x401480 / 0x4014c0): 1150 fades in from black over f s, 1151 fades out,
 * 1152 blacks out the current frame (scripts repeat it with DURING) */
static struct { float rest, total; int out, hold, script; } g_sfade; static int g_black_frame;
static void fade_start(float t, int out) { g_sfade.total = g_sfade.rest = t; g_sfade.out = out; g_sfade.hold = 0; g_sfade.script = 0; }

/* ---- real time cinematics (docs/CINEMATIC.md): object game+0x64, update 0x44f0a0 */
static void cin_update(EkoVM *vm, float dt, float now)
{
    Instance *m = g_cin.main_inst;
    if (wenv("WOODY_CINLOG") && g_cin.state && m) printf("CIN t=%.3f st=%d rem=%.3f anim=%d slot0=%d pos=%.3f spd=%.2f ended=%d at=%.3f P=(%.0f %.0f %.0f)\n", now, g_cin.state, g_cin.remain, g_cin.anim, m->slot[0], m->a_pos, m->a_speed, m->a_ended, m->anim_time, m->position.x, m->position.y, m->position.z);
    if (wenv("WOODY_CINLOG") && g_cin.state && m) { Vec3 c = ins_anim_centre(m); printf("CINROOT %.0f %.0f %.0f\n", c.x, c.y, c.z); }
    switch (g_cin.state) {
    case 1:
        if ((g_cin.timer -= dt) > 0) break;
        fade_start(0.5f, 0); g_black_frame = 1; g_cin.state = 2; eko_set_var(vm, g_cin.var, 0); audio_rtc(g_cin.rtc);   /* vt[0x98]: the /Rtc/ stream of this scene */
        {   /* 0x44eab0: the main instance goes to the vector P0, facing P0 -> P1; everything plays once at speed 3 */
            const Model *mo = g_cin.vec->model; int node = -1;
            for (uint32_t i = 0; i < mo->nnodes && node < 0; i++) if (mo->nodes[i].type_code == 5 && mo->nodes[i].npoints >= 2) node = (int)i;
            if (node < 0) { printf("cinematic: no vector on instance %u\n", g_cin.vec->index); g_cin.remain = 0; break; }
            Vec3 P0 = ins_point_world(g_cin.vec, mo->nodes[node].point_base), P1 = ins_point_world(g_cin.vec, mo->nodes[node].point_base + 1);
            plane_release(); if (g_player && g_player->inst == m) player_cin_place(g_player, P0, atan2f(P1.x - P0.x, P1.z - P0.z));
            m->scripted = 1; m->visible = 1; inst_play_once(m, g_cin.anim, 3.0f, now);
            for (int i = 0; i < g_cin.nactors; i++) { inst_play_once(g_cin.actor[i].inst, g_cin.actor[i].anim, 3.0f, now);
                g_cin.actor[i].inst->visible = 1; }                          /* 0x44ed03: 0x4077f0 puts the actor back into its cell even when the script hid it (W1B: Buzz 398 and his saucer 399) */
            g_cam.anim_inst = m; g_cam.anim_letterbox = 1;                  /* 0x44ed3f / 0x44ed53: CamMgr+0x5d4 = the main instance, +0x618 |= 2 */
            g_cam.cut = 1; cam_set_mode(0x80);
        }
        break;
    case 2:
        g_cin.remain -= dt; if (g_cin.remain > 0.5f) break;
        g_cin.timer = g_cin.remain; fade_start(g_cin.remain > 0.01f ? g_cin.remain : 0.01f, 1); g_cin.state = 3; break;
    case 3:
        if ((g_cin.timer -= dt) > 0) break;
        fade_start(0.5f, 0); g_cin.timer = 0.5f; g_cin.state = 4; audio_rtc(-1); audio_music_pause(0, 0.45f);
        {   /* 0x44edb0 + 0x445af9: Perso Reset 0x44ab20 (0x445b23), the player continues where the animation left the root,
             * follow camera behind him (hard cut 0x41f9f0(2), +0x368 = 0, SetMode(0, 0)) */
            Vec3 pos, fwd;
            if (g_player && g_player->inst == m) {
                m->scripted = 0;
                int have = ins_root_end(m, g_cin.anim, &pos, &fwd);
                player_cin_end(g_player, have, pos, have ? atan2f(fwd.x, fwd.z) : g_player->yaw);
                plane_release(); game_bombs_discard();                          /* Reset: +0x4ec = 0 (0x44ad22), 0x44db10 (0x44ad79) */
            }
            g_cam.cut = 1; cam_set_mode(1);
        }
        break;
    case 4: if ((g_cin.timer -= dt) <= 0) memset(&g_cin, 0, sizeof g_cin); break;
    default: break;
    }
}

/* ---- progress (docs/GAMEFLOW.md 6, MENU_LOAD.md 6): the active save struct is the only player state that survives a
 * level change. The file has the original Woody.sav layout byte for byte (4 slots + per-slot volumes, version
 * 0x11004), but is called woodyre.sav: the original's own Woody.sav is only ever read (imported), never written. */
#pragma pack(push, 1)
typedef struct { int32_t best; uint8_t done, uniq[32], pad[3]; int32_t st[4]; float time; } SaveRec;   /* st = app+0x74, +0x7c, +0x78, +0x80 (0x4503b0 / 0x4503e0) */
typedef struct { int32_t lives, unique, charges; float health; SaveRec rec[29]; } SaveChar;          /* 0x6dc */
typedef struct { int32_t sum; uint32_t ver, zero; SaveChar chr[3]; int32_t extra; } SaveSlot;          /* 0x14a4 */
typedef struct { uint32_t ver; SaveSlot slot[4]; uint32_t music[4], sfx[4]; float vib[4]; } SaveFile;  /* 0x52c4 */
#pragma pack(pop)
_Static_assert(sizeof(SaveRec) == 0x3c && sizeof(SaveChar) == 0x6dc && sizeof(SaveSlot) == 0x14a4 && sizeof(SaveFile) == 0x52c4, "Woody.sav layout");
#define SAVE_VER 0x11004u
static SaveSlot g_save;                                          /* app+0x48 */
static SaveFile g_file;                                          /* the slot manager app+0x4c */
static int g_char, g_unlock_all;                                 /* cfg+0x380: 0 Woody, 1 Knothead, 2 Splinter */
static void slot_reset(SaveSlot *s)                              /* 0x44ffa0 */
{
    memset(s, 0, sizeof *s); s->ver = SAVE_VER;
    for (int c = 0; c < 3; c++) { s->chr[c].lives = 9; s->chr[c].health = 3.0f; }
    int32_t sum = 0; for (size_t i = 0; i < sizeof *s; i++) sum += (int8_t)((uint8_t *)s)[i]; s->sum = sum;   /* 0x450030, never checked: always 432 */
}
static void save_reset(void) { slot_reset(&g_save); }
static void file_reset(void) { memset(&g_file, 0, sizeof g_file); g_file.ver = SAVE_VER; for (int s = 0; s < 4; s++) { slot_reset(&g_file.slot[s]); g_file.music[s] = 70; g_file.sfx[s] = 100; } }   /* 0x456e20 */
static int  file_write(void)                                     /* 0x450b30; the port writes a new file and then swaps it in, so a full disk */
{                                                                /* or a crash halfway cannot take the four slots with it */
    g_file.ver = SAVE_VER; FILE *f = fopen("woodyre.sav.new", "wb"); if (!f) return 0;
    int ok = fwrite(&g_file, sizeof g_file, 1, f) == 1; if (fflush(f)) ok = 0; if (fclose(f)) ok = 0;
    if (ok) ok = plat_replace("woodyre.sav.new", "woodyre.sav");
    if (!ok) remove("woodyre.sav.new");
    return ok;
}
/* 0x450be0: 1 = read, 0 = no file (page 7), -1 = unreadable / wrong version (page 0xa). Without a woodyre.sav an
 * original Woody.sav (game/ or the working directory) is taken as is; the port's earlier one-save file ("WSV2") is
 * converted into slot 0 so nobody loses progress. */
static int file_read(void)
{
    static const char *src[3] = { "woodyre.sav", "game/Woody.sav", "Woody.sav" };
    for (int k = 0; k < 3; k++) {
        FILE *f = fopen(src[k], "rb"); if (!f) continue;
        static uint8_t buf[sizeof(SaveFile)]; size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
        if (n == sizeof buf && !memcmp(buf, &(uint32_t){ SAVE_VER }, 4)) { memcpy(&g_file, buf, sizeof g_file); if (k) printf("save: imported %s\n", src[k]); return 1; }
        if (k == 0 && n >= 4 && !memcmp(buf, "WSV2", 4)) {
            typedef struct { int32_t lives, unique, charges; float health; uint8_t done[29]; int32_t best[29], stats[29][4]; float stat_time[29]; } V2;
            if (n < 4 + 3 * sizeof(V2)) return -1;
            file_reset(); SaveSlot *s = &g_file.slot[0];
            for (int c = 0; c < 3; c++) {
                V2 v; memcpy(&v, buf + 4 + c * sizeof v, sizeof v); SaveChar *sc = &s->chr[c];
                sc->lives = v.lives; sc->unique = v.unique; sc->charges = v.charges; sc->health = v.health;
                for (int L = 0; L < 29; L++) { SaveRec *r = &sc->rec[L]; r->done = v.done[L]; r->best = v.best[L]; r->st[0] = v.stats[L][0]; r->st[1] = v.stats[L][2]; r->st[2] = v.stats[L][1]; r->st[3] = v.stats[L][3]; r->time = v.stat_time[L]; }
            }
            puts("save: converted the old woodyre.sav into slot 1"); file_write(); return 1;
        }
        if (k == 0) return -1;
    }
    return 0;
}
static int char_of_level(int i) { return i <= 10 ? 0 : i <= 17 ? 1 : i <= 24 ? 2 : i == 25 ? 0 : -1; }   /* byte table 0x404830; -1 = unchanged */
static int slot_char_pct(const SaveSlot *s, int c)               /* 0x450050: weights of the finished levels, 100 each */
{
    static const int8_t W[29] = { [2]=6,[3]=7,[4]=8,[5]=9,[6]=12,[7]=13,[8]=14,[9]=15,[10]=16, [12]=14,[13]=15,[14]=16,[15]=17,[16]=18,[17]=20, [19]=14,[20]=15,[21]=16,[22]=17,[23]=18,[24]=20 };
    int p = 0; for (int L = 0; L < 29; L++) if (char_of_level(L) == c && s->chr[c].rec[L].done) p += W[L]; return p;
}
static int slot_pct(const SaveSlot *s) { return (slot_char_pct(s, 0) + slot_char_pct(s, 1) + slot_char_pct(s, 2)) / 3; }   /* 0x4501f0; 0 = free */
static int slot_char_open(const SaveSlot *s, int c) { return c == 0 || s->chr[0].rec[c == 1 ? 6 : 10].done; }            /* 0x4509b0: W2D / W3D done */
/* ---- options (docs/MENU_OPTIONS.md): the original keeps the sfx / music volume in Woody.cfg ([0x4c2c50] / [0x4c2c54],
 * written back at exit) and a copy per save slot. The port keeps them in its own woodyre.cfg, key=value lines, so
 * port-only settings (aspect ratio, resolution, issue #12) can be added later without a format bump. The master
 * volumes are linear amplitude v / 100: -2000 log10(100 / v) mB in 0x48bf50 is exactly that. */
static struct { int sfx, music, vib; int have_sfx, have_music; } g_opt = { 100, 70, 100, 0, 0 };   /* port defaults (audio.c's 1.0 / 0.7); vibration: the original's 100% with a joystick (0x4674b0), where rumble is a no-op; the port rumbles the pads of src/pad.c (key rumble=, PORT EXTRA);
                                                                   * have_*: woodyre.cfg has the key (else setup_import may take Woody.cfg +0x80 / +0x84) */
static void opt_apply(void) { audio_master(g_opt.sfx * 0.01f, g_opt.music * 0.01f); }   /* 0x469570 / 0x4695a0 */
/* two switches of Detect.exe's Sound page that the game reads (docs/SETUP.md 3): "Invert Left/Right" = Woody.cfg +0x74 ->
 * [0x5e81c0] = reverse stereo (0x46b7e0), and "Cinematic" = +0x70 -> [0x5e81bc], which only gates the sound of the HNM films
 * (0x426a57: no DirectSound for the film player without it). The port keeps them in woodyre.cfg (reverse_stereo=, film_sound=);
 * a key woodyre.cfg does not have yet comes from Woody.cfg when its sound section is live (setup_import), else the Setup
 * defaults (0x100032a0: invert 0, cinematic 1). -1 = not in woodyre.cfg. vsync: the same for the display page's key. */
static struct { int rev, film, vsync; } g_setup = { -1, -1, -1 };
static int g_logos = 1;                                 /* woodyre.cfg logos=0: never play the logo films (PORT EXTRA) */
static char g_bind_cfg[32][100]; static int g_nbind_cfg;   /* woodyre.cfg key_* / pad_* lines (PORT EXTRA): applied by in_read_cfg after Woody.cfg */
static void ctl_write(FILE *f);
static int g_cam_speed = 100;                           /* woodyre.cfg camera_speed= (percent): how fast the right stick turns / raises the follow camera (PORT EXTRA, Controls page) */
static const int k_cam_speeds[] = { 25, 50, 75, 100, 150, 200 };
static int g_pad_dz = 30;                               /* woodyre.cfg pad_deadzone= (percent): the stick dead zone of the pads (PORT EXTRA), default the original's 30 % (0x467a80) */
/* ---- display (docs/DISPLAY.md; everything here is a PORT EXTRA). The original runs exclusive fullscreen at the Woody.cfg mode
 * (Detect's list, default 640x480), always 4:3 in the layout, and paces itself only by Flip(DDFLIP_WAIT) = vsync (0x47ee90);
 * no frame cap, dt clamped to 0.1 s (0x40185b). The port: a window of any size or borderless fullscreen, 4:3 pillarboxed or a
 * wide Hor+ view (the vertical fov stays, the HUD / menus keep their 640x480 layout centred), vsync, an optional frame cap.
 * The Graphics page (all off by default, the original's look): ambient occlusion (gtao.c), texture sharpness = anisotropic
 * filtering (render_gl.c, 1 = off), edge smoothing SMAA 0..4 and multisampling MSAA 0/2/4/8 (postfx.c). */
typedef struct { int wide, w, h, full, vsync, cap, ao, aniso, smaa, msaa; } Display;
static Display g_disp = { 1, 1280, 800, 0, 1, 0, 0, 1, 0, 0 };     /* woodyre.cfg; 1280x800 wide = the port's window before these options */
static Display g_dnow;                                  /* what runs: g_disp, or the defaults for a --shot run, plus the overrides */
static int g_disp_dirty;                                /* the main loop applies g_dnow (window mode, vsync) */
static const int k_disp_res[][2] = { {640,480}, {800,600}, {1024,768}, {1280,960}, {1280,720}, {1280,800}, {1600,900}, {1920,1080}, {2560,1440}, {3840,2160} };
static const int k_disp_cap[] = { 0, 30, 60, 120, 144, 240 };
static const int k_gfx_aniso[] = { 1, 2, 4, 8, 16 }, k_gfx_msaa[] = { 0, 2, 4, 8 };
#define NRES (int)(sizeof k_disp_res / sizeof k_disp_res[0])
#define NCAP (int)(sizeof k_disp_cap / sizeof k_disp_cap[0])
static void opt_read(void)
{
    FILE *f = fopen("woodyre.cfg", "r"); char line[128];
    if (f) { while (fgets(line, sizeof line, f)) { int v, v2; char s[16];
        if (sscanf(line, "sfx=%d", &v) == 1) { g_opt.sfx = v; g_opt.have_sfx = 1; } else if (sscanf(line, "music=%d", &v) == 1) { g_opt.music = v; g_opt.have_music = 1; } else if (sscanf(line, "rumble=%d", &v) == 1) g_opt.vib = v;   /* the old key vibration= is dropped: it never did anything */
        else if (sscanf(line, "aspect=%15s", s) == 1) g_disp.wide = strcmp(s, "4:3") != 0;
        else if (sscanf(line, "window=%dx%d", &v, &v2) == 2) { if (v >= 320 && v2 >= 240 && v <= 7680 && v2 <= 4320) { g_disp.w = v; g_disp.h = v2; } }
        else if (sscanf(line, "fullscreen=%d", &v) == 1) g_disp.full = v != 0; else if (sscanf(line, "vsync=%d", &v) == 1) g_disp.vsync = g_setup.vsync = v != 0;
        else if (sscanf(line, "reverse_stereo=%d", &v) == 1) g_setup.rev = v != 0; else if (sscanf(line, "film_sound=%d", &v) == 1) g_setup.film = v != 0;
        else if (sscanf(line, "logos=%d", &v) == 1) g_logos = v != 0;
        else if (sscanf(line, "ao=%d", &v) == 1) g_disp.ao = v != 0;
        else if (sscanf(line, "aniso=%d", &v) == 1) g_disp.aniso = v < 1 ? 1 : v > 16 ? 16 : v;
        else if (sscanf(line, "smaa=%d", &v) == 1) g_disp.smaa = v < 0 ? 0 : v > 4 ? 4 : v;
        else if (sscanf(line, "msaa=%d", &v) == 1) g_disp.msaa = v < 2 ? 0 : v > 8 ? 8 : v;
        else if (sscanf(line, "pad_deadzone=%d", &v) == 1) g_pad_dz = v < 0 ? 0 : v > 90 ? 90 : v;
        else if (sscanf(line, "camera_speed=%d", &v) == 1) g_cam_speed = v < 10 ? 10 : v > 300 ? 300 : v;
        else if ((!strncmp(line, "key_", 4) || !strncmp(line, "pad_", 4)) && strchr(line, '=') && g_nbind_cfg < 32) {
            line[strcspn(line, "\r\n")] = 0; snprintf(g_bind_cfg[g_nbind_cfg++], sizeof g_bind_cfg[0], "%s", line); }
        else if (sscanf(line, "fpscap=%d", &v) == 1) g_disp.cap = v < 0 ? 0 : v > 1000 ? 1000 : v; } fclose(f); }
    int *o[3] = { &g_opt.sfx, &g_opt.music, &g_opt.vib }; for (int i = 0; i < 3; i++) { if (*o[i] < 0) *o[i] = 0; if (*o[i] > 100) *o[i] = 100; }
}
static void opt_write(void)
{
    FILE *f = fopen("woodyre.cfg", "w"); if (!f) return;
    fprintf(f, "sfx=%d\nmusic=%d\nrumble=%d\n", g_opt.sfx, g_opt.music, g_opt.vib);
    fprintf(f, "aspect=%s\nwindow=%dx%d\nfullscreen=%d\nvsync=%d\nfpscap=%d\n", g_disp.wide ? "wide" : "4:3", g_disp.w, g_disp.h, g_disp.full, g_disp.vsync, g_disp.cap);
    fprintf(f, "ao=%d\naniso=%d\nsmaa=%d\nmsaa=%d\n", g_disp.ao, g_disp.aniso, g_disp.smaa, g_disp.msaa);
    fprintf(f, "reverse_stereo=%d\nfilm_sound=%d\nlogos=%d\npad_deadzone=%d\ncamera_speed=%d\n", g_setup.rev > 0, g_setup.film != 0, g_logos, g_pad_dz, g_cam_speed);
    ctl_write(f);
    fclose(f);
}
/* the 3D view in the window (GL origin bottom left): narrower than 4:3 = letterboxed in both modes, wider = pillarboxed in 4:3 mode */
static void disp_view(const Window *w, int *x, int *y, int *vw, int *vh)
{
    int W = w->width, H = w->height; *x = *y = 0; *vw = W; *vh = H;
    if (W <= 0 || H <= 0) return;
    if (W * 3 < H * 4) { *vh = W * 3 / 4; *y = (H - *vh) / 2; }
    else if (!g_dnow.wide && W * 3 > H * 4) { *vw = H * 4 / 3; *x = (W - *vw) / 2; }
}
static void disp_apply(Window *w)
{
    win_mode(w, g_dnow.w, g_dnow.h, g_dnow.full);
    if (win_vsync(g_dnow.vsync) < 0) printf("display: no WGL_EXT_swap_control, vsync is up to the driver\n");
    if (g_dnow.cap > 0) timeBeginPeriod(1);                                          /* Sleep(1) of the frame cap: 1 ms, not 15.6 */
    printf("display: %dx%d %s, %s, vsync %s, fps cap %d\n", w->width, w->height, g_dnow.full ? "fullscreen" : "window", g_dnow.wide ? "wide (Hor+)" : "4:3", g_dnow.vsync ? "on" : "off", g_dnow.cap);
    g_disp_dirty = 0;
}
static void gfx_apply(void)                             /* the Graphics page's settings (all port extras); cheap, no window change */
{
    gtao_enable(g_dnow.ao); rnd_set_aniso(g_dnow.aniso); postfx_set(g_dnow.msaa, g_dnow.smaa);
    printf("graphics: ambient occlusion %s, texture sharpness %dx, SMAA %d, MSAA %dx\n", g_dnow.ao ? "on" : "off", g_dnow.aniso, g_dnow.smaa, g_dnow.msaa);
}

/* ---- input (docs/INPUT.md): the key bindings of Woody.cfg, the joystick and the action layer of 0x402940. Everything
 * the game reads goes through 14 actions (the controller [0x5e6188], PERSO_MOVE 3.2): 0/1 left/right, 2/3 forward/back
 * (values -1/+1, the stick gives its deflection), 4 jump (+12 = menu confirm), 5 duck (menu back), 6 attack, 7 look
 * around, 8 duck while riding, 9 pause, 10 camera behind, 11 special. Bindings: app+0x108, two per action (config 1 / 2
 * of Detect.exe); a code < 0x200 is a DirectInput key, 0x200 + n joystick button n. The port keeps VK codes. */
#define IN_JOY 0x200
static struct {
    int bind[12][4];                  /* per action: up to 4 VK codes or IN_JOY + button, 0 ends the list (Woody.cfg fills 2, the port defaults more) */
    int mode;                         /* app+0x104 (0x44fc05): 0 keyboard only (cfg+0x114 == 1), 1 / 2 joystick (cfg+0x110 != 0 / == 0): the
                                       * keyboard gives no directions then (0x40301e); 3 = port, no Woody.cfg: keyboard and joystick both */
    int have_cfg;
    int joy; unsigned jxmin, jxmax, jymin, jymax; double joy_scan;   /* the WinMM device (the original: the first attached DirectInput joystick, 0x4678d0) */
    float jx, jy; uint32_t jbtn; int jok;                        /* this frame: axes after the dead zone (-1..1), buttons */
    int down[14], prev[14]; float val[14];                       /* the controller: held this frame / last frame, value (0x467460) */
    int pbind[12][4];                 /* PORT EXTRA: the pad buttons per action (PAD_* + 1 of pad.h, 0 ends the list); Woody.cfg does not know them */
    PadState pad;                     /* this frame's pads (src/pad.c), all of them together */
} g_in = { .joy = -1, .mode = 3 };
static int in_dik_vk(int d)                                      /* DIK scan code (Woody.cfg) -> VK: the extended keys by table, the rest by the layout */
{
    static const unsigned char T[][2] = { {0x1c,VK_RETURN}, {0x9c,VK_RETURN}, {0x1d,VK_LCONTROL}, {0x9d,VK_RCONTROL}, {0x2a,VK_LSHIFT}, {0x36,VK_RSHIFT},
        {0x38,VK_LMENU}, {0xb8,VK_RMENU}, {0xc8,VK_UP}, {0xd0,VK_DOWN}, {0xcb,VK_LEFT}, {0xcd,VK_RIGHT}, {0xc7,VK_HOME}, {0xcf,VK_END}, {0xc9,VK_PRIOR},
        {0xd1,VK_NEXT}, {0xd2,VK_INSERT}, {0xd3,VK_DELETE}, {0x47,VK_NUMPAD7}, {0x48,VK_NUMPAD8}, {0x49,VK_NUMPAD9}, {0x4b,VK_NUMPAD4}, {0x4c,VK_NUMPAD5},
        {0x4d,VK_NUMPAD6}, {0x4f,VK_NUMPAD1}, {0x50,VK_NUMPAD2}, {0x51,VK_NUMPAD3}, {0x52,VK_NUMPAD0}, {0x53,VK_DECIMAL}, {0x37,VK_MULTIPLY},
        {0x4a,VK_SUBTRACT}, {0x4e,VK_ADD}, {0xb5,VK_DIVIDE}, {0x45,VK_NUMLOCK}, {0xdb,VK_LWIN}, {0xdc,VK_RWIN}, {0xdd,VK_APPS}, {0xc5,VK_PAUSE}, {0xb7,VK_SNAPSHOT} };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (T[i][0] == d) return T[i][1];
    return d > 0 && d < 0x80 ? plat_vsc_to_vk(d) : 0;
}
/* Woody.cfg (0x401000 at boot, 0x405e0f): u32 0x19072001, then 0x11c bytes into cfg 0x4c2bd0 (docs/INPUT.md 2). The keys are cfg+0xac (config 1)
 * and +0xdc (config 2), 12 each in the order up, down, left, right, 5, 6, 4, 8, 7, 9, 10, 11 (0x44fbd0); the mode flags cfg+0x110 / +0x114.
 * Not there: the port defaults below. WOODY_CFG=path points elsewhere; else Woody.cfg in the working directory (like the original), else next
 * to the Data folder (the install layout). */
static void in_defaults(void)
{
    static const int D[12][4] = { {VK_LEFT,'A'}, {VK_RIGHT,'D'}, {VK_UP,'W'}, {VK_DOWN,'S'},           /* the port's keys; the joystick buttons of Detect's */
        {VK_SPACE, IN_JOY + 2}, {'X', IN_JOY + 0}, {VK_LCONTROL, VK_SHIFT, IN_JOY + 1}, {VK_RETURN, 'V', IN_JOY + 4},   /* DefaultControlSettings (Setup.dll 0x10002650, */
        {'X', IN_JOY + 3}, {VK_ESCAPE, IN_JOY + 5}, {'C', VK_NUMPAD0, IN_JOY + 6}, {VK_RCONTROL, 'E', IN_JOY + 7} };   /* table 0x1000c060: 0x200..0x207) */
    memcpy(g_in.bind, D, sizeof D); g_in.mode = 3; g_in.have_cfg = 0;
    /* the pads (PORT EXTRA): D-pad = the directions, A / Cross jump (menu: confirm), B / Circle duck (menu: back, also duck
     * while riding), X / Square and RT attack, RB look around, Start / Options pause, LB and R3 camera behind, Y / Triangle and
     * LT special; the left stick moves (actions 0..3 with its deflection, as the joystick) */
    #define P_(b) (PAD_##b + 1)
    static const int PD[12][4] = { {P_(LEFT)}, {P_(RIGHT)}, {P_(UP)}, {P_(DOWN)}, {P_(A)}, {P_(B)}, {P_(X), P_(RT)}, {P_(RB)},
        {P_(B)}, {P_(START)}, {P_(LB), P_(RS)}, {P_(Y), P_(LT)} };
    #undef P_
    memcpy(g_in.pbind, PD, sizeof PD);
}
/* the Woody.cfg file (magic + 0x11c bytes) into b[0x120]: 1 = read, 0 = none, -1 = obsolete; *used = the path */
static int wcfg_read(const char *data_dir, unsigned char *b, const char **used)
{
    static char alt[600]; const char *try_[3] = { wenv("WOODY_CFG"), "Woody.cfg", alt }; snprintf(alt, sizeof alt, "%s/../Woody.cfg", data_dir);
    FILE *f = NULL; int k;
    for (k = 0; k < 3 && !f; k++) if (try_[k]) f = fopen(try_[k], "rb");
    if (!f) return 0;
    *used = try_[k - 1];
    size_t n = fread(b, 1, 0x120, f); fclose(f);
    return n < 0x120 || (b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24) != 0x19072001 ? -1 : 1;   /* 0x401092 */
}
#define CFG32(o) ((int)((uint32_t)b[(o) + 4] | (uint32_t)b[(o) + 5] << 8 | (uint32_t)b[(o) + 6] << 16 | (uint32_t)b[(o) + 7] << 24))   /* cfg offset o (the file has the magic first) */
/* boot, before the window (docs/SETUP.md): the Setup keys woodyre.cfg does not have yet come from Woody.cfg.
 * - vsync: +0x50 is Detect's "Activate VSync" box, which the game inverts at device creation on Windows NT (0x47ee0e:
 *   [0x4c2c20] = 1 - flag when [0x4c3aa0], GetVersionExA platform 2) and then flips on vsync when the result is not 0
 *   (0x47eea0); the port only runs on the NT line, so vsync = (flag != 1): the Setup default 0 gives vsync.
 * - reverse stereo +0x74, film sound +0x70, the sfx / music volumes +0x80 / +0x84: only from a live sound section (one of the switches +0x68/+0x6c/+0x70 on);
 *   the cfg that tools/native/mkcfg.c wrote before it stopped calling CoInitialize has the whole section 0 (Setup's
 *   0x10002770 returns early on S_FALSE), which the original plays without any sound - not something to copy.
 * WOODY_REVSTEREO=0/1 overrides the reverse stereo for a run without saving it (testing, main). */
static void setup_import(const char *data_dir)
{
    unsigned char b[0x120]; const char *path = NULL; int r = wcfg_read(data_dir, b, &path);
    if (r > 0) {
        int live = CFG32(0x68) || CFG32(0x6c) || CFG32(0x70);
        if (g_setup.vsync < 0) g_disp.vsync = CFG32(0x50) != 1;
        if (live && g_setup.rev < 0) g_setup.rev = CFG32(0x74) != 0;
        if (live && g_setup.film < 0) g_setup.film = CFG32(0x70) != 0;
        /* the Sound Fx / Music sliders +0x80 / +0x84 (0..100; Setup's defaults 100 / 30): the original's master volumes [0x4c2c50] /
         * [0x4c2c54] -> [0x5e81ec] / [0x5e81f0] = v / 100 (0x4691e2..0x4692b9, SOUND.md 2.4). Same rule: only from a live section and only
         * while woodyre.cfg has no sfx= / music= - opt_write puts both in at the first exit, after which woodyre.cfg leads */
        int vs = CFG32(0x80), vm = CFG32(0x84);
        if (live && !g_opt.have_sfx) g_opt.sfx = vs < 0 ? 0 : vs > 100 ? 100 : vs;
        if (live && !g_opt.have_music) g_opt.music = vm < 0 ? 0 : vm > 100 ? 100 : vm;
        printf("setup: %s: vsync flag %d%s, sound section %s (fx %d music %d cinematic %d invert %d, volumes %d %d %d%s)\n", path, CFG32(0x50),
               g_setup.vsync >= 0 ? " (woodyre.cfg has vsync=)" : CFG32(0x50) != 1 ? " -> vsync on" : " -> vsync off", live ? "live" : "off (ignored)", CFG32(0x68), CFG32(0x6c), CFG32(0x70), CFG32(0x74), CFG32(0x80), CFG32(0x84), CFG32(0x88),
               !live ? "" : g_opt.have_sfx && g_opt.have_music ? ", woodyre.cfg has sfx= / music=" : g_opt.have_sfx ? " -> music imported" : g_opt.have_music ? " -> sfx imported" : " -> sfx / music imported");
    }
    if (g_setup.rev < 0) g_setup.rev = 0;                                           /* Setup defaults 0x100032a0 */
    if (g_setup.film < 0) g_setup.film = 1;
}
static void in_read_wcfg(const char *data_dir)
{
    in_defaults();
    unsigned char b[0x120]; const char *path = NULL; int r = wcfg_read(data_dir, b, &path);
    if (!r) { puts("input: no Woody.cfg, port keys"); return; }
    if (r < 0) { puts("input: Configuration file is Obsolete... (Woody.cfg), port keys"); return; }   /* 0x401092 */
    static const int act_of[12] = { 2, 3, 0, 1, 5, 6, 4, 8, 7, 9, 10, 11 };   /* cfg key index -> action (0x44fc5a..0x44fe28) */
    memset(g_in.bind, 0, sizeof g_in.bind);
    for (int c = 0; c < 2; c++) for (int i = 0; i < 12; i++) {
        int code = CFG32(0xac + c * 0x30 + i * 4), vk = code >= IN_JOY ? (code < IN_JOY + 32 ? code : 0) : in_dik_vk(code);   /* 0x44fe80: an unknown key = 0x90, never pressed */
        g_in.bind[act_of[i]][c] = vk;
    }
    g_in.mode = CFG32(0x114) == 1 ? 0 : CFG32(0x110) == 0 ? 2 : 1;   /* 0x44fc05 */
    g_in.have_cfg = 1;
    printf("input: %s, mode %d (%s)\n", path, g_in.mode, g_in.mode ? "joystick" : "keyboard only");
}
#undef CFG32
static float in_deadzone(float v, float dz)                       /* 0x467a80: dead zone dz (30 %, 0x4aab98) of the range 4096 (0x4b6f84), the rest scaled to 0..1 */
{
    int iv = (int)(v * 4096.0f), d = (int)(4096.0f * dz);
    if (iv > 0) { iv -= d; if (iv < 0) iv = 0; } else { iv += d; if (iv > 0) iv = 0; }
    return d < 4096 ? (float)iv / (4096.0f - d) : 0;
}
static void in_joy_poll(double now, double tl)                   /* 0x467a40 poll + 0x467a80 axes + 0x467af0 buttons; tl = the level clock */
{
    g_in.jx = g_in.jy = 0; g_in.jbtn = 0; g_in.jok = 0;
    if (g_in.mode == 0) return;                                  /* keyboard only: the joystick object exists but 0x402d39 never polls it */
#ifdef _WIN32
    if (g_in.pad.kind != PADK_NONE) g_in.joy = -1;               /* a pad of src/pad.c is there: the WinMM device would be the same pad once more */
    else if (g_in.joy < 0 && now >= g_in.joy_scan) {             /* no device yet: look again every 3 s (the original enumerates once, at boot) */
        g_in.joy_scan = now + 3.0;
        for (UINT id = 0; id < 16 && g_in.joy < 0; id++) { JOYINFOEX ji = { sizeof ji, JOY_RETURNALL }; JOYCAPSA jc;
            if (joyGetPosEx(id, &ji) == JOYERR_NOERROR && joyGetDevCapsA(id, &jc, sizeof jc) == JOYERR_NOERROR && jc.wMid != 0x054c /* Sony: src/pad.c reads those */) {
                g_in.joy = (int)id; g_in.jxmin = jc.wXmin; g_in.jxmax = jc.wXmax; g_in.jymin = jc.wYmin; g_in.jymax = jc.wYmax; printf("input: joystick %u \"%s\"\n", id, jc.szPname); } }
    }
#endif
    const char *e = wenv("WOODY_JOY");                         /* testing: WOODY_JOY="T:X:Y:BUTTONS[:D] ...": stick at X,Y (-1..1, before the dead
                                                                  * zone) and the button mask for D s (default 0.08) from T s on the clock of --shot */
    if (e) { float x = 0, y = 0; unsigned m = 0; const char *s = e; int used;
        for (double t, d; sscanf(s, " %lf:%f:%f:%i%n", &t, &x, &y, (int *)&m, &used) == 4; ) { float xx = x, yy = y; unsigned mm = m; s += used; d = 0.08;
            if (*s == ':' && sscanf(s, ":%lf%n", &d, &used) == 1) s += used;
            if (tl >= t && tl < t + d) { g_in.jok = 1; g_in.jx = xx; g_in.jy = yy; g_in.jbtn = mm; } }
        if (g_in.jok) goto deadzone;
    }
    if (g_in.joy < 0) return;
#ifdef _WIN32                                                    /* elsewhere every pad is SDL's (pad_sdl.c) and g_in.joy stays -1 */
    { JOYINFOEX ji = { sizeof ji, JOY_RETURNX | JOY_RETURNY | JOY_RETURNBUTTONS };
      if (joyGetPosEx((UINT)g_in.joy, &ji) != JOYERR_NOERROR) { printf("input: joystick %d lost\n", g_in.joy); g_in.joy = -1; return; }   /* unplugged: scan again */
      float rx = g_in.jxmax > g_in.jxmin ? (float)g_in.jxmax - g_in.jxmin : 65535.0f, ry = g_in.jymax > g_in.jymin ? (float)g_in.jymax - g_in.jymin : 65535.0f;
      g_in.jx = ((float)ji.dwXpos - g_in.jxmin) / rx * 2.0f - 1.0f; g_in.jy = ((float)ji.dwYpos - g_in.jymin) / ry * 2.0f - 1.0f;   /* DIPROP_RANGE -4096..4096 (0x4677db) */
      g_in.jbtn = (uint32_t)ji.dwButtons; g_in.jok = 1; }
#endif
deadzone:
    g_in.jx = in_deadzone(g_in.jx, 0.3f); g_in.jy = in_deadzone(g_in.jy, 0.3f);
}
static int in_key(const Window *w, int vk, int fly)              /* a VK held (keyboard vt[4] 0x4675f0) */
{
    if (fly && (vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D' || vk == 'Q' || vk == 'E' || vk == 'X' || vk == VK_SPACE || vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT)) return 0;   /* the free camera's keys */
    if (vk == VK_LCONTROL) return w->keys[VK_LCONTROL] || (w->keys[VK_CONTROL] && !w->keys[VK_RCONTROL]);   /* WOODY_KEYS CTRL / SHIFT hold only VK_CONTROL / VK_SHIFT: the left one */
    if (vk == VK_LSHIFT) return w->keys[VK_LSHIFT] || (w->keys[VK_SHIFT] && !w->keys[VK_RSHIFT]);
    return vk > 0 && vk < 256 && w->keys[vk];
}
static void in_set(int a, float v) { g_in.down[a] = 1; g_in.val[a] = v; }   /* 0x4673b0 */
static void in_pad_poll(const Window *w, double tl)              /* PORT EXTRA: the pads of src/pad.c */
{
    pad_set_strength(g_opt.vib * 0.01f);                         /* the Vibration option */
    pad_poll(&g_in.pad, w->focused, w->dev_changes);
    /* testing: WOODY_PAD="T:LX:LY:BUTTONS[:D] ...": a pad with the left stick at LX,LY (-1..1, before the dead zone) and the
     * buttons 1 << PAD_* (pad.h) for D s (default 0.08) from T s on the level clock, as WOODY_JOY */
    for (const char *s = wenv("WOODY_PAD"); s && *s; ) {
        double t, d = 0.08; float x, y; unsigned m; int used;
        if (sscanf(s, " %lf:%f:%f:%i%n", &t, &x, &y, (int *)&m, &used) != 4) break;
        s += used; if (*s == ':' && sscanf(s, ":%lf%n", &d, &used) == 1) s += used;
        if (tl >= t && tl < t + d) { if (!g_in.pad.kind) g_in.pad.kind = PADK_DS5; g_in.pad.lx = x; g_in.pad.ly = y; g_in.pad.buttons = m; }
    }
    for (const char *s = wenv("WOODY_RSTICK"); s && *s; ) {   /* testing: WOODY_RSTICK="T:RX:RY[:D] ...", the right stick likewise (default 0.5 s) */
        double t, d = 0.5; float x, y; int used;
        if (sscanf(s, " %lf:%f:%f%n", &t, &x, &y, &used) != 3) break;
        s += used; if (*s == ':' && sscanf(s, ":%lf%n", &d, &used) == 1) s += used;
        if (tl >= t && tl < t + d) { if (!g_in.pad.kind) g_in.pad.kind = PADK_DS5; g_in.pad.rx = x; g_in.pad.ry = y; }
    }
}
static void in_frame(const Window *w, int fly, double now, double tl)   /* 0x402940: joystick first, then the keyboard; the value is the last one set */
{
    memcpy(g_in.prev, g_in.down, sizeof g_in.down); memset(g_in.down, 0, sizeof g_in.down); memset(g_in.val, 0, sizeof g_in.val);
    in_pad_poll(w, tl);
    in_joy_poll(now, tl);
    static const float dv[4] = { -1.0f, 1.0f, -1.0f, 1.0f };
    static const int btn_acts[8] = { 6, 11, 5, 4, 7, 10, 8, 9 }; /* 0x402df5..0x403002 */
    if (g_in.jok) {
        if (g_in.jx > 0) in_set(1, g_in.jx); else if (g_in.jx < 0) in_set(0, g_in.jx);   /* 0x402d58: joystick vt[3] X, vt[4] Y (down = +) */
        if (g_in.jy > 0) in_set(3, g_in.jy); else if (g_in.jy < 0) in_set(2, g_in.jy);
        for (int i = 0; i < 8; i++) { int a = btn_acts[i];
            for (int s = 0; s < 4 && g_in.bind[a][s]; s++) { int c = g_in.bind[a][s]; if (c >= IN_JOY && (g_in.jbtn >> (c - IN_JOY) & 1)) { in_set(a, 1.0f); if (a == 4) in_set(12, 1.0f); break; } } }
    }
    if (g_in.pad.kind != PADK_NONE) {                            /* PORT EXTRA: the pads after the joystick, before the keyboard */
        float x = in_deadzone(g_in.pad.lx, g_pad_dz * 0.01f), y = in_deadzone(g_in.pad.ly, g_pad_dz * 0.01f);
        if (x > 0) in_set(1, x); else if (x < 0) in_set(0, x);   /* the left stick as the joystick's X / Y */
        if (y > 0) in_set(3, y); else if (y < 0) in_set(2, y);
        for (int a = 0; a < 12; a++)
            for (int s = 0; s < 4 && g_in.pbind[a][s]; s++) if (g_in.pad.buttons >> (g_in.pbind[a][s] - 1) & 1) { in_set(a, a < 4 ? dv[a] : 1.0f); if (a == 4) in_set(12, 1.0f); break; }
    }
    int dirs = g_in.mode == 0 || g_in.mode == 3 || g_in.joy < 0; /* 0x40301e: directions from the keys only in mode 0 (port: also without a joystick) */
    for (int a = 0; a < 12; a++) {
        if (a < 4 && !dirs) continue;
        for (int s = 0; s < 4 && g_in.bind[a][s]; s++) { int c = g_in.bind[a][s]; if (c < IN_JOY && in_key(w, c, fly)) { in_set(a, a < 4 ? dv[a] : 1.0f); if (a == 4) in_set(12, 1.0f); break; } }
    }
}
static int in_held(int a) { return g_in.down[a]; }                                /* 0x467400 */
/* ---- the bindings as names, for woodyre.cfg and the Controls page (PORT EXTRA, docs/INPUT.md 6.2). The font has no "-",
 * "[", "*" and friends, so those keys are spelled out. */
static const struct { int vk; const char *n; } k_vk_names[] = {
    {VK_SPACE,"Space"}, {VK_RETURN,"Enter"}, {VK_ESCAPE,"Esc"}, {VK_TAB,"Tab"}, {VK_BACK,"Backspace"}, {VK_LEFT,"Left arrow"},
    {VK_RIGHT,"Right arrow"}, {VK_UP,"Up arrow"}, {VK_DOWN,"Down arrow"}, {VK_LCONTROL,"Left Ctrl"}, {VK_RCONTROL,"Right Ctrl"},
    {VK_CONTROL,"Ctrl"}, {VK_LSHIFT,"Left Shift"}, {VK_RSHIFT,"Right Shift"}, {VK_SHIFT,"Shift"}, {VK_LMENU,"Left Alt"},
    {VK_RMENU,"Right Alt"}, {VK_MENU,"Alt"}, {VK_INSERT,"Insert"}, {VK_DELETE,"Delete"}, {VK_HOME,"Home"}, {VK_END,"End"},
    {VK_PRIOR,"Page Up"}, {VK_NEXT,"Page Down"}, {VK_MULTIPLY,"Num Times"}, {VK_ADD,"Num Plus"}, {VK_SUBTRACT,"Num Minus"},
    {VK_DECIMAL,"Num Dot"}, {VK_DIVIDE,"Num Slash"}, {VK_CAPITAL,"Caps Lock"}, {VK_NUMLOCK,"Num Lock"}, {VK_SCROLL,"Scroll Lock"},
    {VK_PAUSE,"Pause"}, {VK_APPS,"Menu key"}, {VK_OEM_1,"Semicolon"}, {VK_OEM_PLUS,"Equals"}, {VK_OEM_COMMA,"Comma"},
    {VK_OEM_MINUS,"Minus"}, {VK_OEM_PERIOD,"Period"}, {VK_OEM_2,"Slash"}, {VK_OEM_3,"Backquote"}, {VK_OEM_4,"Left bracket"},
    {VK_OEM_5,"Backslash"}, {VK_OEM_6,"Right bracket"}, {VK_OEM_7,"Quote"}, {VK_OEM_102,"Angle bracket"} };
static void vk_name(int vk, char *b, size_t n)
{
    if (vk >= IN_JOY) { snprintf(b, n, "Joy%d", vk - IN_JOY + 1); return; }                       /* a WinMM joystick button */
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { snprintf(b, n, "%c", vk); return; }
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(b, n, "F%d", vk - VK_F1 + 1); return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(b, n, "Num %d", vk - VK_NUMPAD0); return; }
    for (unsigned i = 0; i < sizeof k_vk_names / sizeof k_vk_names[0]; i++) if (k_vk_names[i].vk == vk) { snprintf(b, n, "%s", k_vk_names[i].n); return; }
    snprintf(b, n, "Key %d", vk);
}
static int vk_parse(const char *s)                               /* a name of vk_name -> the code, 0 = unknown */
{
    char b[32];
    for (int vk = 1; vk < 256; vk++) { vk_name(vk, b, sizeof b); if (!_stricmp(b, s)) return vk; }
    if (!_strnicmp(s, "Joy", 3)) { int n = atoi(s + 3); if (n >= 1 && n <= 32) return IN_JOY + n - 1; }
    return 0;
}
static const char *const k_pad_cfg[PAD_NBUTTONS] = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Back", "Start", "LS", "RS", "Up", "Down", "Left", "Right", "Guide", "Touchpad" };
static const char *pad_btn_name(int b, int kind)                 /* b = PAD_*; the names printed on that pad */
{
    static const char *const sony[PAD_NBUTTONS] = { "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "Create", "Options", "L3", "R3", "Up", "Down", "Left", "Right", "PS", "Touchpad" };
    static const char *const dpad[4] = { "Dpad Up", "Dpad Down", "Dpad Left", "Dpad Right" };   /* no "-" in the font */
    if (b >= PAD_UP && b <= PAD_RIGHT) return dpad[b - PAD_UP];
    static const char *const nintendo[PAD_NBUTTONS] = { "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Minus", "Plus", "LS", "RS", "Up", "Down", "Left", "Right", "Home", "Touchpad" };
    if (kind == PADK_DS4 && b == PAD_BACK) return "Share";
    if (kind == PADK_SWITCH) return nintendo[b];
    return kind == PADK_DS4 || kind == PADK_DS5 ? sony[b] : k_pad_cfg[b];
}
/* the rows of the Controls page; Duck also sets action 8 (duck while riding), which the original binds on its own */
static const struct { const char *name, *cfg; int a, a2; } k_ctl_rows[11] = {
    {"Walk forward:", "forward", 2, -1}, {"Walk back:", "back", 3, -1}, {"Walk left:", "left", 0, -1}, {"Walk right:", "right", 1, -1},
    {"Jump:", "jump", 4, -1}, {"Attack:", "attack", 6, -1}, {"Special:", "special", 11, -1}, {"Duck:", "duck", 5, 8},
    {"Look around:", "look", 7, -1}, {"Camera behind:", "camera", 10, -1}, {"Pause:", "pause", 9, -1} };
static struct {
    int custom;                                                  /* the bindings came from woodyre.cfg or the Controls page: write them back */
    int dev;                                                     /* the page shows 0 the keyboard, 1 the pads */
    int cap, phase; float cap_t; uint32_t pad_prev;              /* cap 1: waiting for the key / button of row M.sel - 2 (phase 0: until all are let go), 2: done, until all are let go */
    int bak[12][4], pbak[12][4], bak_mode, bak_cfg;              /* the bindings when the page opened ("back" puts them back) */
    int last_kind;                                               /* the pad kind seen last, for the button names */
    int bak_speed;                                               /* g_cam_speed when the page opened */
} g_ctl;
static void ctl_apply_cfg(void)                                  /* woodyre.cfg key_<row>=Name,Name / pad_<row>=A,RT over Woody.cfg and the defaults */
{
    for (int i = 0; i < g_nbind_cfg; i++) {
        char l[100]; snprintf(l, sizeof l, "%s", g_bind_cfg[i]);
        int pad = l[0] == 'p'; char *eq = strchr(l, '='); if (!eq) continue;   /* a line longer than g_bind_cfg[0] lost its '=' */
        *eq = 0; int row = -1;
        for (int r = 0; r < 11; r++) if (!strcmp(l + 4, k_ctl_rows[r].cfg)) row = r;
        if (row < 0) { printf("input: woodyre.cfg: unknown binding %s\n", l); continue; }
        int codes[4] = { 0 }, n = 0;
        for (char *t = strtok(eq + 1, ","); t && n < 4; t = strtok(NULL, ",")) {
            while (*t == ' ') t++; char *e = t + strlen(t); while (e > t && e[-1] == ' ') *--e = 0;
            int c = 0;
            if (pad) { for (int b = 0; b < PAD_NBUTTONS; b++) if (!_stricmp(t, k_pad_cfg[b])) c = b + 1; }
            else c = vk_parse(t);
            if (c) codes[n++] = c; else if (*t) printf("input: woodyre.cfg: unknown %s \"%s\" in %s\n", pad ? "button" : "key", t, l);
        }
        int (*B)[4] = pad ? g_in.pbind : g_in.bind;
        memcpy(B[k_ctl_rows[row].a], codes, sizeof codes);
        if (k_ctl_rows[row].a2 >= 0) memcpy(B[k_ctl_rows[row].a2], codes, sizeof codes);
    }
    if (g_nbind_cfg) { g_ctl.custom = 1; printf("input: %d bindings from woodyre.cfg\n", g_nbind_cfg); }
}
static void in_read_cfg(const char *data_dir) { in_read_wcfg(data_dir); ctl_apply_cfg(); }
static void ctl_list(int dev, int a, int names, char *b, size_t n)   /* the codes of action a as text; names: the pad's own names, keyboard view without WinMM buttons */
{
    b[0] = 0;
    if (names && dev && a < 4) snprintf(b, n, "%s", hud_tr("Left stick"));    /* the stick always walks (as the original's joystick axes) */
    for (int s = 0; s < 4; s++) {
        int c = dev ? g_in.pbind[a][s] : g_in.bind[a][s]; if (!c) break;
        if (names && !dev && c >= IN_JOY) continue;
        char t[48]; if (dev) snprintf(t, sizeof t, "%s", names ? pad_btn_name(c - 1, g_ctl.last_kind) : k_pad_cfg[c - 1]); else vk_name(c, t, sizeof t);
        if (names) { const char *tt = hud_tr(t); if (tt != t) snprintf(t, sizeof t, "%s", tt); }   /* the Controls page in the CD's language; woodyre.cfg keeps the English names */
        size_t l = strlen(b); snprintf(b + l, n - l, "%s%s", l ? (names ? ", " : ",") : "", t);
    }
}
static void ctl_write(FILE *f)
{
    if (!g_ctl.custom) return;
    char b[100];
    for (int r = 0; r < 11; r++) { ctl_list(0, k_ctl_rows[r].a, 0, b, sizeof b); fprintf(f, "key_%s=%s\n", k_ctl_rows[r].cfg, b); }
    for (int r = 0; r < 11; r++) { ctl_list(1, k_ctl_rows[r].a, 0, b, sizeof b); fprintf(f, "pad_%s=%s\n", k_ctl_rows[r].cfg, b); }
}
static int in_pressed(int a) { return g_in.down[a] && !g_in.prev[a]; }            /* 0x467420 */
static int in_released(int a) { return !g_in.down[a] && g_in.prev[a]; }           /* 0x467440 */
/* LevelIsEnable 0x450470 (table 0x450694): the done flag of the predecessor. The original reads it in the block of the
 * current character; here in the block of the predecessor's own character (otherwise K1A could never open from KWS). */
static int level_is_enable(int level)
{
    if (g_unlock_all || level <= 2 || level > 25) return 1;
    int pred = (level == 11 || level == 12) ? 6 : (level == 18 || level == 19) ? 10 : level - 1;
    return g_save.chr[char_of_level(pred)].rec[pred].done;
}
static Instance *g_prop;                                         /* message 1142: the instance 1140 moves along with the player */
static int g_act_now[3], g_act_prev[3];                          /* input actions 0 (left), 1 (right), 6 (attack) for 1048 / 1049 / 1050 */
static Instance *g_pose;                                         /* message 1141: House menu pose (0x44e640); held every tick while level 0 runs (0x44e690) */
static uint32_t g_intro_var; static int g_have_intro;            /* message 1160: House intro state variable (0 rest, 1 start, 2/3 running, 4 done) */

/* script -> engine messages. Only the subset needed to see something happen is implemented;
 * everything else is logged. See docs/MESSAGES.md. */

/* ---- sound (docs/SOUND.md): script messages 1600..1657 (0x467fa0) -> the mixer in audio.c.
 * vol 0..100; pitch args are x0.01 (f > 0 = frequency factor), "dur" args are x-0.01 (wanted duration); dmin args are x0.01 m. */
/* vector marker of an instance (0x42f6b0): the `n`th 2-point node with typecode `tc`; P0 = start, dir = P1 - P0 (not
 * normalised). 0 when the instance has no such marker, which is how the callers that walk them count how many it has. */
static int inst_vector_at(const Instance *in, uint32_t tc, uint32_t n, Vec3 *p0, Vec3 *dir)
{
    const Model *mo = in->model; uint32_t seen = 0;
    for (uint32_t i = 0; i < mo->nnodes; i++) if (mo->nodes[i].kind != 0 && mo->nodes[i].type_code == tc && mo->nodes[i].npoints >= 2 && (mo->nodes[i].kind == 0x20 || tc == 5)) {
        if (seen++ != n) continue;
        Vec3 a = ins_point_world(in, mo->nodes[i].point_base), b = ins_point_world(in, mo->nodes[i].point_base + 1);
        *p0 = a; dir->x = b.x - a.x; dir->y = b.y - a.y; dir->z = b.z - a.z; return 1;
    }
    return 0;
}
static int inst_vector(const Instance *in, uint32_t tc, Vec3 *p0, Vec3 *dir) { return inst_vector_at(in, tc, 0, p0, dir); }
/* for player.c (Perso state 7, 0x44e1c0): 0x42f6b0 poses the instance (vtbl[2](1)) before it reads the marker */
int game_inst_vector(const Instance *in, uint32_t tc, Vec3 *p0, Vec3 *dir) { ins_pose((Instance *)in, in->anim, in->anim_time); return inst_vector(in, tc, p0, dir); }
/* ---- results screen (docs/GAMEFLOW.md 5.1 and 10.1): the hub script ends a level with 1140 [door, var], the engine
 * puts the Perso on the door vector with scripted action 0x4a (he comes down at the door with his parasol, animation
 * 74 with its own camera track) and runs the state machine perso+0x724 while menu page 0x1e is up:
 *   0 arriving  -> the Perso is free again: action 0x4b (he lies under the parasol), page 0x1e shown (state 1)
 *   1 panel     -> OK: n = categories with collected == total != 0 (0x453cf0), n unique items (0x44c840),
 *                  cheer action 0x4e (n != 0) or 0x4c (0x453fc0), state 2 / 3, panel hidden
 *   2 / 3 cheer -> cheer over: action 0x4d and state 4 (0x454020)
 *   4 score     -> score 0x453cb0 into the save when it beats the record (0x450230), then page 6 "Do you want to save?"
 *   5 leaving   -> 0x454050 started a 0.5 s fade-out and set +0x744 = 0.5; after it: prop hidden, fade-in 0.5 s,
 *                  camera back (0x41f9f0(2)), the Perso in front of the door and SetVar(perso+0x728, 1) (0x45422c),
 *                  which is what the hub script has been waiting for.
 * He ends on P0 facing P0 - P1 (0x454244). The 2D page (iris, counting lines) is in hud.c, docs/RESULTS.md. */
static struct { int have, level; int stats[4]; float time; } g_stats;   /* app+0x74: copied from perso+0x710 by EndLevel (0x404c21) */
static struct {
    int on, state;                          /* perso+0x724 */
    uint32_t var;                           /* perso+0x728: the variable 1140 came with */
    float t;                                /* perso+0x744 */
    int race, cats, score, best, high;
    Vec3 door_p, door_d;
} g_res;

static int results_race(int level) { return level == 0xd || level == 0xf || level == 0x11 || level == 0x14 || level == 0x16 || level == 0x18; }   /* the R-levels */
static int results_score(const int *st, float time, int race)                      /* 0x453cb0 */
{
    int s = 0, t = (int)time;
    if (!race) { s += (t < 1800 ? 1800 - t : 0) * 10; s += st[2] * 100 + (st[0] && st[2] == st[0] ? st[2] * 50 : 0); }
    return s + st[3] * 100 + (st[1] && st[3] == st[1] ? st[3] * 50 : 0);                                              /* a complete category is worth 50 % more */
}
static int results_cats(const int *st) { return (st[0] && st[2] == st[0]) + (st[1] && st[3] == st[1]); }               /* 0x453cf0 */

static void script_action_camera(void)      /* the tail of 0x44dda0: an action whose animation carries a camera track becomes the camera (0x44df67) */
{
    Instance *pi = g_player ? g_player->inst : NULL; Vec3 e, t;
    if (pi && (uint32_t)pi->anim < pi->model->nanims && ins_camera_eval(pi, pi->anim, 0.0f, &e, &t)) {
        g_cam.anim_inst = pi; g_cam.anim_letterbox = 0; g_cam.anim_eye = e; g_cam.anim_tgt = t;
        g_cam.cut = 1; cam_set_mode(0x80);                                         /* 0x41f9f0(2) = cut, 0x41f410(7, 0) = mask 1 << 7; no letterbox, unlike a cinematic */
    }
}
/* every action of the sequence goes through 0x44dda0 WITH the door vector perso+0x72c: he is put back on P0 facing P1,
 * so the chain 74 -> 75 -> 76/78 -> 77 (authored in one shared frame, 74 ends where 75 starts) is never offset by the
 * root motion the previous action ended with; and its tail cuts to the new action's camera track in the same frame,
 * over the follow camera that 0x44e5a0 asked for at the end of the previous one */
static void results_action(int act)
{
    if (!g_player) return;
    player_script_action(g_player, act, 1, g_res.door_p, g_res.door_d);
    g_player->cam_end_req = 0; script_action_camera();
}

static void results_capture(void)           /* 0x404c21: memcpy(app+0x74, perso+0x710, 20) before the level is unloaded */
{
    int race = g_player && (g_player->inst->type == 18 || g_player->inst->type == 19);
    g_stats.have = 1; g_stats.level = g_level;
    g_stats.stats[0] = g_enemies.total;                                              /* [0x4c5330] via 0x44a6a0 -> 0x453c80: every enemy of class 4..13 counts itself in PostLoad, bosses 14..16 not */
    g_stats.stats[1] = g_player ? (race ? g_player->race_total : g_player->bonus_total) : 0;   /* [0x5e54f4] / [0x5e54e4] */
    g_stats.stats[2] = g_enemies.killed;                                             /* 0x404c34: EndLevel copies [0x4c532c] (enemies REMOVED after their death, vtbl[29] 0x41aff0) over stat[2];
                                                                                      * perso+0x718 itself is never written. Dying but not yet gone does not count, a boss never does */
    g_stats.stats[3] = g_player ? (race ? g_player->race_bonus : g_player->bonus_got) : 0;     /* [0x5e54e8] */
    g_stats.time = g_player ? g_player->play_time : 0.0f;                            /* the accumulator perso+0x710+0x10 */
    printf("  RESULTS stats of %s: %d/%d enemies, %d/%d bonuses, %.0f s", k_levels[g_level], g_stats.stats[2], g_stats.stats[0], g_stats.stats[3], g_stats.stats[1], g_stats.time), puts("");
}

/* 0x453e0a..0x453f9b: the prop of message 1142 (the parasol, the deckchair and the glass: WWS model 67, one animation
 * exactly as long as Woody's anim 74) is put on P0 of the door vector, turned so that its model -y points out of the door
 * (rows of its 3x3 = F x up, F, up with F = the horizontal P0 - P1, normalised; 0x41af10 is a cross product), inserted
 * into the world (0x4077f0) and its animation 0 started on the clock of now (0x436ca0(prop, 1.0, {0,-1,-1,-1})). It
 * runs in step with Woody's action 0x4a and holds its last frame, him lying under the parasol, until state 5 hides it. */
static Quat quat_from_axes(Vec3 X, Vec3 Y, Vec3 Z);
static void results_prop(Instance *pr, Vec3 p0, Vec3 dir)
{
    Vec3 F = { -dir.x, 0, -dir.z }; float l = sqrtf(F.x * F.x + F.z * F.z);
    if (l > 1e-4f) { F.x /= l; F.z /= l; } else F = (Vec3){ 0, 0, 0 };                /* [0x4a9004]: too short, the zero vector stays */
    Vec3 U = { 0, 1, 0 }, R = { -F.z, 0, F.x };                                       /* F x U */
    pr->position = p0;
    if (l > 1e-4f) pr->quat = quat_from_axes(R, F, U);
    mat4_from_trs(&pr->world, pr->position, pr->quat, pr->scale);
    pr->visible = 1; pr->scripted = 1;
    pr->cell_ok = 0;                                                                  /* 0x453f82: 0x4077f0 re-cells it at the new +0xc (renderer chains_sync); it kept the cell of its .ins position otherwise and was never listed */
    inst_play_once(pr, 0, 3.0f, g_now);                                              /* 1.0 x [0x4a988c], like every other .ins clock */
}
static void results_begin(EkoVM *vm, Instance *door, uint32_t var)                    /* 0x453d90 */
{
    Vec3 p0, dir; int have = inst_vector(door, 5, &p0, &dir) || inst_vector(door, 0, &p0, &dir);   /* 0x444a0a asks typecode 5 only (0x42f6b0(door, 5, buf, 0)): every hub door has one */
    if (!have) { float y = inst_yaw(door) + 3.14159265f; p0 = door->position; dir = (Vec3){ sinf(y), 0, cosf(y) }; }   /* port fallbacks: the original would use the stale stack buffer */
    memset(&g_res, 0, sizeof g_res);
    g_res.on = 1; g_res.state = 0; g_res.var = var; g_res.door_p = p0; g_res.door_d = dir;
    if (!g_stats.have) { g_stats.level = g_prev_level; g_stats.time = 0; memset(g_stats.stats, 0, sizeof g_stats.stats); }   /* started straight in the hub */
    int lvl = g_stats.level, c = char_of_level(lvl) < 0 ? g_char : char_of_level(lvl);
    g_res.race = results_race(lvl);
    g_res.score = results_score(g_stats.stats, g_stats.time, g_res.race);
    g_res.best = (lvl >= 0 && lvl < 29) ? g_save.chr[c].rec[lvl].best : 0;
    g_res.high = g_res.score > g_res.best;
    eko_set_var(vm, var, 0);
    player_script_action(g_player, 0x4a, have, p0, dir);                              /* 0x453dbb: the arrival at the hub door */
    script_action_camera();
    if (g_prop) results_prop(g_prop, p0, dir);
    hud_results_enter(); audio_fx(63, NULL, NULL);                                    /* menu page 0x1e (0x404df0): 0x4544b0 + 0x45b8c0, SoundFx 0x3f */
    printf("  RESULTS begin: level %s, score %d, best %d%s", k_levels[lvl >= 0 && lvl < 29 ? lvl : 0], g_res.score, g_res.best, g_res.high ? " (new record)" : ""), puts("");
}

static void results_store(void)                                                       /* 0x450230 / 0x450260 / 0x450380..0x450440 */
{
    int lvl = g_stats.level; if (lvl < 0 || lvl >= 29) return;
    SaveChar *sc = &g_save.chr[char_of_level(lvl) < 0 ? g_char : char_of_level(lvl)];
    SaveRec *r = &sc->rec[lvl];
    if (g_res.score <= r->best) return;
    r->best = g_res.score;
    r->st[0] = g_stats.stats[0]; r->st[1] = g_stats.stats[2]; r->st[2] = g_stats.stats[1]; r->st[3] = g_stats.stats[3];   /* rec+0x28..0x34 */
    r->time = g_stats.time;
}
static void results_close(void) { fade_start(0.5f, 1); g_res.state = 5; g_res.t = 0.5f; }   /* 0x454050 */

/* ---- menu pages (docs/TITLE.md 5, MENU_NEWGAME.md, MENU_OPTIONS.md, MENU_LOAD.md): the page object app+0x3c with
 * its per-page handlers (0x404e90 -> table 0x405b1c). Pages used here: 0 title, 1 main menu, 2 load slot, 3 world
 * select, 5 save slot, 6 "Do you want to save?", 7 no save, 8 saved, 9 save failed, 0xa load failed, 0x17 overwrite,
 * 0x18 pause, 0x19 pause while riding, 0x1b options, 0x1c "Are you sure?", 0x1f intro running. -1 = no page (a level is being played). */
typedef struct { int ok, back, up, dn, left, right, esc_rel, esc_prs, atk_rel, syn; } MenuKeys;
typedef struct {                               /* the panel page base 0x45b830 (pages 1, 2, 3, 5) */
    float t, ti;                               /* +0x1c since opening / closing, +0x28 since enter / validate */
    float iris_from, iris_to, iris_t;          /* the iris +0x2c (0x4776b0 / 0x477920) */
    int iris_on, opening, closing, lock, result;
    float wait;                                /* how long after the validate the result is handed over (0.5 s, or 0 = next frame) */
} Panel;
static struct {
    int page, sel; float delay;                /* page+4 selection, page+8 input delay */
    Panel p;
    int p1_iris;                               /* page 1 +0x38: set by "Load game", so page 1 opens with the iris when you come back */
    int slot2_sel, slot5_sel;                  /* pages 2 / 5 remember their selection (ctor: 1) */
    int opt_bak[3];                            /* page 0x1b +0x14..0x1c: the values to restore on "back" */
    int newgame;                               /* app+0x94 */
    float attract;                             /* app+0x98: counts down on page 0 only, from 35 s at boot and after each intro */
    int title_music;                           /* app+0x54 */
    int results;                               /* the save pages were opened by the results screen */
    int quitting; float quit_t;                /* 0x404cb0: fade out, then leave */
    int save_s;                                /* app+0x60: the slot chosen on page 5 */
    int hs_char;                               /* page 4 +0x3c: whose high scores (set by page 3's SEE HIGH SCORES) */
    int wait, wait_r;                          /* app+0x5c: the frames a wait page 0xb / 0xc / 0xe still stands; the read result it hands on */
    Display disp;                              /* port page 0x40: the display settings being edited (applied on Continue) */
    float cred_t;                              /* page 0x20 +0x18: time on the credits page (0x45bd9e) */
    float go_t;                                /* page 0x1d +0x14: GAME OVER time left (0x45bbb0: 5.0)  */
} M ={ -1, 0, 0, { 0 }, 0, 1, 1, { 0 }, 0, 35.0f };
static float g_title_t;                        /* seconds since the title pose (action 0x49) started: the orbit phase (docs/TITLE.md 1.4) */
static int g_intro_obj;                        /* message 1160 arg 2 & 0xffffff: script object 115 */

static const MenuItem k_page0[] = { {21,1} };
static const MenuItem k_page1[] = { {22,1}, {23,1}, {36,1}, {2,1} };
static const MenuItem k_page6[] = { {35,2}, {1,2}, {5,1}, {6,1} };   /* 0x4b58a8 (4 items, 0x4600a0): an empty line (string 1) under the question, as on pages 7/8/9/0xa/0x17 */
static const MenuItem k_page7[] = { {64,2}, {65,2}, {66,2}, {1,2}, {4,1} };
static const MenuItem k_page8[] = { {67,2}, {1,2}, {4,1} };   /* 0x4b5878 */
static const MenuItem k_page9[] = { {59,2}, {1,2}, {4,1} };   /* 0x4b58e8 */
static const MenuItem k_pagea[] = { {60,2}, {1,2}, {4,1} };
static const MenuItem k_page17[] = { {61,2}, {1,2}, {5,1}, {6,1} };   /* 0x4b5c98; enter 0x45b370 puts the cursor on "No" */
static const MenuItem k_page18[] = { {4,1}, {36,1}, {2,1} };
static const MenuItem k_page19[] = { {4,1}, {19,1}, {36,1}, {2,1} };   /* 0x4b5d18: Continue (5), Start again (18), Options (6), Quit (7) */
static const MenuItem k_page1c[] = { {3,2}, {5,1}, {6,1} };
static MenuItem k_page1b[] = { {36,2}, {38,0x10}, {39,0x10}, {132,0x10}, {4,1}, {0,1}, {0,1}, {0,1} };   /* items 5 "Display", 6 "Graphics" and 7 "Controls": port extras, ids set on enter (the page starts at 0.17 instead of 0.4 to fit them) */
/* port page 0x40 "Display" (docs/DISPLAY.md 4), the list class of 0x1b with choices (flag 0x100): ids and values are
 * port strings or Common 133 "On" / 134 "Off", filled in by disp_items */
static MenuItem k_page40[7] = { {0,2}, {0,0x100}, {0,0x100}, {0,0x100}, {0,0x100}, {0,0x100}, {4,1} };
static int disp_res_index(const Display *d) { for (int i = 0; i < NRES; i++) if (k_disp_res[i][0] == d->w && k_disp_res[i][1] == d->h) return i; return -1; }
static void disp_items(void)
{
    const Display *d = &M.disp; char b[24];
    k_page40[0].id = hud_port_str("Display");
    k_page40[1].id = hud_port_str("Aspect ratio");     k_page40[1].value = (int)hud_port_str(d->wide ? "Wide" : "4:3");
    snprintf(b, sizeof b, "%dx%d", d->w, d->h);
    k_page40[2].id = hud_port_str("Window size");      k_page40[2].value = (int)hud_port_str(b);
    k_page40[3].id = hud_port_str("Fullscreen");       k_page40[3].value = d->full ? 133 : 134;
    k_page40[4].id = hud_port_str("VSync");            k_page40[4].value = d->vsync ? 133 : 134;
    snprintf(b, sizeof b, "%d", d->cap);
    k_page40[5].id = hud_port_str("Frame rate limit"); k_page40[5].value = d->cap ? (int)hud_port_str(b) : 134;
}
static void disp_step(int item, int dir)               /* left / right on a choice: round the list */
{
    Display *d = &M.disp;
    switch (item) {
    case 1: d->wide ^= 1; break;
    case 2: { int i = disp_res_index(d); i = i < 0 ? (dir > 0 ? 0 : NRES - 1) : (i + dir + NRES) % NRES; d->w = k_disp_res[i][0]; d->h = k_disp_res[i][1]; break; }
    case 3: d->full ^= 1; break;
    case 4: d->vsync ^= 1; break;
    case 5: { int i = 0; while (i < NCAP && k_disp_cap[i] != d->cap) i++; i = i == NCAP ? 0 : (i + dir + NCAP) % NCAP; d->cap = k_disp_cap[i]; break; }
    }
}
/* port page 0x42 "Graphics" (docs/DISPLAY.md 5), the same list class: what the GL cannot do shows "Not supported" */
static MenuItem k_page42[6] = { {0,2}, {0,0x100}, {0,0x100}, {0,0x100}, {0,0x100}, {4,1} };
static void gfx_items(void)
{
    const Display *d = &M.disp; char b[24]; int ns = (int)hud_port_str("Not supported");
    static const char *k_q[5] = { "Off", "Low", "Medium", "High", "Ultra" };
    k_page42[0].id = hud_port_str("Graphics");
    k_page42[1].id = hud_port_str("Ambient occlusion"); k_page42[1].value = !gtao_supported() ? ns : d->ao ? 133 : 134;
    snprintf(b, sizeof b, "%dx", d->aniso);
    k_page42[2].id = hud_port_str("Texture sharpness"); k_page42[2].value = rnd_aniso_max() < 2 ? ns : d->aniso > 1 ? (int)hud_port_str(b) : (int)hud_port_str("Original");
    k_page42[3].id = hud_port_str("Edge smoothing");    k_page42[3].value = !postfx_smaa_supported() ? ns : d->smaa ? (int)hud_port_str(k_q[d->smaa]) : 134;
    snprintf(b, sizeof b, "%dx", d->msaa);
    k_page42[4].id = hud_port_str("Multisampling");     k_page42[4].value = postfx_msaa_max() < 2 ? ns : d->msaa ? (int)hud_port_str(b) : 134;
}
static int gfx_round(const int *list, int n, int max, int cur, int dir)   /* the next / previous entry of list up to max, round */
{
    int m = 0; while (m < n && list[m] <= max) m++; if (m < 2) return cur;
    int i = 0; while (i < m && list[i] != cur) i++;
    return list[i == m ? 0 : (i + dir + m) % m];
}
static void gfx_step(int item, int dir)
{
    Display *d = &M.disp;
    switch (item) {
    case 1: if (gtao_supported()) d->ao ^= 1; break;
    case 2: d->aniso = gfx_round(k_gfx_aniso, 5, rnd_aniso_max(), d->aniso, dir); break;
    case 3: if (postfx_smaa_supported()) d->smaa = (d->smaa + dir + 5) % 5; break;
    case 4: d->msaa = gfx_round(k_gfx_msaa, 4, postfx_msaa_max(), d->msaa, dir); break;
    }
}
/* port page 0x41 "Controls" (PORT EXTRA, docs/INPUT.md 6.2): a choice of device, a row per action with its keys or buttons,
 * Defaults and Continue. Confirm on a row waits for a key (or button): one already in the row is taken out, any other is
 * added (and taken out of the other rows); Esc or 6 s without one leaves the row as it was. Changes count at once,
 * Continue saves them to woodyre.cfg, back puts the old ones back. Row 13 = the camera speed of the right stick (left / right
 * steps through k_cam_speeds; slow is gentler on motion sickness). */
static MenuItem k_page41[16] = { {0,2}, {0,0x100}, {0,0x100}, {0,0x100}, {0,0x100}, {0,0x100}, {0,0x100}, {0,0x100}, {0,0x100}, {0,0x100},
    {0,0x100}, {0,0x100}, {0,0x100}, {0,0x100}, {0,1}, {4,1} };
static void ctl_items(void)
{
    k_page41[0].id = hud_port_str("Controls");
    k_page41[1].id = hud_port_str("Device:"); k_page41[1].value = (int)hud_port_str(g_ctl.dev ? "Controller" : "Keyboard");
    for (int r = 0; r < 11; r++) {
        char b[100]; ctl_list(g_ctl.dev, k_ctl_rows[r].a, 1, b, sizeof b);
        if (g_ctl.cap == 1 && M.sel == 2 + r) snprintf(b, sizeof b, "%s", g_ctl.dev ? "Press a button" : "Press a key");
        else if (!b[0]) snprintf(b, sizeof b, "(none)");
        k_page41[2 + r].id = hud_port_str(k_ctl_rows[r].name); k_page41[2 + r].value = (int)hud_port_str_tmp(r, b);
    }
    char cs[8]; snprintf(cs, sizeof cs, "%d%%", g_cam_speed);
    k_page41[13].id = hud_port_str("Camera speed:"); k_page41[13].value = (int)hud_port_str_tmp(11, cs);
    k_page41[14].id = hud_port_str("Defaults");
}
static int key_side(int vk) { return vk == VK_LSHIFT || vk == VK_RSHIFT ? VK_SHIFT : vk == VK_LCONTROL || vk == VK_RCONTROL ? VK_CONTROL : vk == VK_LMENU || vk == VK_RMENU ? VK_MENU : vk; }
static int ctl_same(int dev, int a, int b) { return a == b || (!dev && (key_side(a) == b || key_side(b) == a)); }   /* Shift also matches Left / Right Shift */
static void ctl_remove(int *l, int dev, int code)
{
    int n = 0; for (int s = 0; s < 4; s++) if (l[s] && !ctl_same(dev, l[s], code)) l[n++] = l[s];
    while (n < 4) l[n++] = 0;
}
static void ctl_assign(int row, int code)                        /* code: a VK (keyboard) or PAD_* + 1 */
{
    int dev = g_ctl.dev, (*B)[4] = dev ? g_in.pbind : g_in.bind, a = k_ctl_rows[row].a, had = 0;
    for (int s = 0; s < 4; s++) if (B[a][s] && ctl_same(dev, B[a][s], code)) had = 1;
    if (had) ctl_remove(B[a], dev, code);
    else {
        for (int r = 0; r < 11; r++) if (r != row) { ctl_remove(B[k_ctl_rows[r].a], dev, code); if (k_ctl_rows[r].a2 >= 0) ctl_remove(B[k_ctl_rows[r].a2], dev, code); }
        int n = 0; while (n < 4 && B[a][n]) n++;
        if (n == 4) { int k = 0; while (k < 3 && B[a][k] >= IN_JOY) k++; for (int s = k; s < 3; s++) B[a][s] = B[a][s + 1]; n = 3; }   /* full: the oldest key makes room (not a WinMM button) */
        B[a][n] = code;
    }
    if (k_ctl_rows[row].a2 >= 0) memcpy(B[k_ctl_rows[row].a2], B[a], sizeof B[a]);
    char t[32]; if (dev) snprintf(t, sizeof t, "%s", k_pad_cfg[code - 1]); else vk_name(code, t, sizeof t);
    printf("input: %s %s %s\n", k_ctl_rows[row].name, had ? "without" : "with", t);
}
static void ctl_defaults(int dev)                                /* the port's defaults for one device (in_defaults sets both) */
{
    int keep[12][4], mode = g_in.mode, cfg = g_in.have_cfg;
    memcpy(keep, dev ? g_in.bind : g_in.pbind, sizeof keep);
    in_defaults();
    if (dev) { memcpy(g_in.bind, keep, sizeof keep); g_in.mode = mode; g_in.have_cfg = cfg; } else memcpy(g_in.pbind, keep, sizeof keep);
}
/* the key / button capture, in the main loop before the menu: the menu gets no keys meanwhile */
static void ctl_capture(const Window *w, const int *key_prev, MenuKeys *mk, float dt)
{
    if (g_in.pad.kind != PADK_NONE) g_ctl.last_kind = g_in.pad.kind;
    if (!g_ctl.cap) return;
    memset(mk, 0, sizeof *mk);
    int held = 0, vk = 0;
    for (int k = 8; k < 256; k++) {
        if (k == VK_SHIFT || k == VK_CONTROL || k == VK_MENU || k == VK_F11 || k == VK_LWIN || k == VK_RWIN) continue;   /* the sided ones are set too; F11 is fullscreen */
        if (w->keys[k]) { held = 1; if (!key_prev[k] && !vk) vk = k; }
    }
    uint32_t pb = g_in.pad.buttons, pnew = pb & ~g_ctl.pad_prev; g_ctl.pad_prev = pb;
    if (g_ctl.cap == 2 || g_ctl.phase == 0) { if (!held && !pb) { if (g_ctl.cap == 2) g_ctl.cap = 0; else g_ctl.phase = 1; } return; }
    g_ctl.cap_t += dt;
    int done = 0;
    if (vk == VK_ESCAPE || g_ctl.cap_t > 6.0f) done = 1;
    else if (!g_ctl.dev && vk) { ctl_assign(M.sel - 2, vk); done = 1; }
    else if (g_ctl.dev && pnew) { int b = 0; while (!(pnew >> b & 1)) b++; ctl_assign(M.sel - 2, b + 1); done = 1; }
    if (done) { g_ctl.cap = 2; ctl_items(); hud_menu_blink(0.25f); }
}
static const MenuItem *menu_items(int page, int *n, float *yfrac)
{
    #define PG(t, y) { *n = (int)(sizeof t / sizeof t[0]); *yfrac = y; return t; }
    switch (page) {
    case 0: PG(k_page0, 0.7f)  case 1: PG(k_page1, 0.55f)  case 6: PG(k_page6, 0.4f)  case 7: PG(k_page7, 0.4f)
    case 8: PG(k_page8, 0.4f)  case 9: PG(k_page9, 0.4f)   case 0xa: PG(k_pagea, 0.4f) case 0x17: PG(k_page17, 0.4f)
    case 0x18: PG(k_page18, 0.05f) case 0x19: PG(k_page19, 0.05f) case 0x1b: PG(k_page1b, 0.17f) case 0x1c: PG(k_page1c, 0.55f)
    case 0x40: PG(k_page40, 0.25f) case 0x41: PG(k_page41, 0.03f) case 0x42: PG(k_page42, 0.25f)
    }
    #undef PG
    *n = 0; *yfrac = 0; return NULL;
}
/* 0x446920 up: back one, round, over the headers, blink phase 0. 0x446970 down: on one, phase 0 if it moved, else 0.25 */
static void menu_move(int dir)
{
    int n; float y; const MenuItem *it = menu_items(M.page, &n, &y); if (!it || !n) return;
    int s = M.sel;
    for (int k = 0; k < n; k++) { s = (s + dir + n) % n; if (!(it[s].flags & 2)) break; }
    hud_menu_blink(dir < 0 || s != M.sel ? 0.0f : 0.25f); M.sel = s;
}
static int menu_first(void) { int n; float y; const MenuItem *it = menu_items(M.page, &n, &y); for (int i = 0; i < n; i++) if (!(it[i].flags & 2)) return i; return 0; }

static void panel_iris(float from, float to) { M.p.iris_from = from; M.p.iris_to = to; M.p.iris_t = 0; }
static float panel_iris_v(void) { float f = M.p.iris_t / 0.5f; if (f > 1) f = 1; return M.p.iris_from - (M.p.iris_from - M.p.iris_to) * f; }
static void panel_enter(void) { memset(&M.p, 0, sizeof M.p); M.p.opening = 1; M.p.iris_on = 1; panel_iris(0, 0.37f); M.delay = 0.5f; audio_fx(63, NULL, NULL); }   /* 0x45b8c0 */
static void panel_close(int ok, int result) { audio_fx(63, NULL, NULL); panel_iris(0.37f, 0); M.p.closing = M.p.lock = 1; M.p.t = M.p.ti = 0; M.p.result = ok ? result : 24; M.p.wait = 0.5f; }   /* 0x45bae0 / 0x45bb40 */
static float panel_slide(void) { return M.p.ti <= 0.5f ? (M.p.closing ? M.p.t * -600.0f : (0.5f - M.p.t) * -600.0f) : 0.0f; }   /* 0x45d330 */

static void slots_info(HudSlots *h, int page)
{
    memset(h, 0, sizeof *h);
    for (int i = 0; i < 4; i++) { const SaveSlot *s = &g_file.slot[i]; h->pct[i] = slot_pct(s); h->open[i] = slot_char_open(s, 1) | slot_char_open(s, 2) << 1; }
    h->cross = page == 2; h->title = page == 2 ? 25 : 24; h->slide = panel_slide(); h->sel = page == 2 ? M.slot2_sel : M.slot5_sel;
}
/* page 2 moves only between used slots (0x45dfb0 / 0x45e000 / 0x45df20 / 0x45df60), page 5 over all four */
static int slot_step(int page, int cur, int key)                 /* key 0 down, 1 up, 2 right, 3 left */
{
    static const int8_t T2[4][4][2] = {                          /* [key][cur-1] = { first choice, fallback } */
        { {3,4}, {4,3}, {3,4}, {4,3} }, { {1,2}, {2,1}, {1,2}, {2,1} }, { {2,4}, {2,4}, {4,2}, {4,2} }, { {1,3}, {1,3}, {3,1}, {3,1} } };
    static const int8_t T5[4][4] = { {3,4,3,4}, {1,2,1,2}, {2,2,4,4}, {1,1,3,3} };
    if (page == 5) return T5[key][cur - 1];
    for (int k = 0; k < 2; k++) { int s = T2[key][cur - 1][k]; if (slot_pct(&g_file.slot[s - 1])) return s; }
    return cur;
}

/* ---- page 3: the world-select carousel (docs/MENU_LOAD.md 4, class 0x45e560). The eight class-110 instances of House
 * (slots 105..114, registered by message 58) stay hidden everywhere else; this page shows the four records' figure +
 * pedestal and puts them in front of the title camera every frame (0x45edc0 -> 0x451890 / 0x489210). The camera itself
 * keeps orbiting: the carousel hangs in front of the lens and the tree house turns behind it. */
static struct {
    Instance *fig[4], *ped[4]; int role[4];      /* record +0x60 / +0x64; role = the figure's registration n (inst+0x184): 0..3, 4 = "?" */
    int open[4];                                  /* record +0x4c */
    int sel;                                      /* +0x114: place 1..4 */
    float pos, start, target, rt;                 /* +0x118 / +0x11c / +0x120 / +0x138 (turn time) */
    int rot_l, rot_r, slide_out, slide_in;        /* +0x13c / +0x13d / page +0x20 / +0x21 */
    int shown;
} g_car = { .sel = 1, .pos = 1.0f, .target = 1.0f };
static MenuItem k_page3[2] = { {42,1}, {41,0x21} };   /* 0x4b5e50 "PLAY", 0x4b5e60 "SEE HIGH SCORES" (x 0.8) */
static void car_forget(void) { memset(g_car.fig, 0, sizeof g_car.fig); memset(g_car.ped, 0, sizeof g_car.ped); g_car.shown = 0; }   /* level_free: the pointers die with the level */
static void car_reset(void) { g_car.pos = g_car.target = 1.0f; g_car.sel = 1; }   /* 0x45e620, from the validate of page 2: always back on Woody */
static void car_hide(void)                                                         /* 0x45e870 -> 0x4891d0(0) */
{
    for (int i = 0; i < 10; i++) { Instance *in = slot_instance(0x1000000u | (uint32_t)(105 + i)); if (in && in->type == 110) { in->visible = 0; in->tint_scale = 0; } }
    g_car.shown = 0;
}
/* 0x45f310: the records from the active save struct. Knothead is free with W2D done, Splinter with W3D, BlackBox with S3R;
 * the next place shows the "?" figure until the one before it is free, and a figure that is shown but locked is drawn
 * with its lit colour x 0.1 (vt[26] 0x451a40), its pedestal too */
static void car_fill(void)
{
    const SaveSlot *s = &g_save; Instance *I[10];
    for (int i = 0; i < 10; i++) I[i] = slot_instance(0x1000000u | (uint32_t)(105 + i));
    int o[4] = { 1, slot_char_open(s, 1), slot_char_open(s, 2), s->chr[2].rec[24].done };
    g_car.fig[0] = I[0]; g_car.fig[1] = I[1]; g_car.fig[2] = o[1] ? I[2] : I[4]; g_car.fig[3] = o[2] ? I[3] : I[5];
    g_car.role[0] = 0; g_car.role[1] = 1; g_car.role[2] = o[1] ? 2 : 4; g_car.role[3] = o[2] ? 3 : 4;
    car_hide();
    for (int k = 0; k < 4; k++) {
        g_car.ped[k] = I[6 + k]; g_car.open[k] = o[k];
        int dark_fig = k == 1 ? !o[1] : k >= 2 ? !o[k] && o[k - 1] : 0;              /* the "?" figure is never darkened */
        if (g_car.fig[k]) { g_car.fig[k]->visible = 1; g_car.fig[k]->tint_scale = dark_fig ? 0.1f : 0; }
        if (g_car.ped[k]) { g_car.ped[k]->visible = 1; g_car.ped[k]->tint_scale = o[k] ? 0 : 0.1f; }
    }
    g_car.shown = 1;
}
/* 0x45f5d0: the figure that comes to the front: clock = now, speed 3, .ins anim 1 once and then anim 2 in a loop;
 * BlackBox anim 1 once (0x436ca0(1.0 x 3.0, {1,-1,-1,-1})); the "?" figure only gets the clock and the speed */
static void car_anim_sel(void)
{
    Instance *f = g_car.fig[g_car.sel - 1]; int r = g_car.role[g_car.sel - 1]; if (!f) return;
    f->a_speed = f->a_base_speed = 3.0f; f->a_start = g_now; f->a_ended = 0;
    if (r <= 2) { f->slot[0] = 1; f->slot[1] = f->slot[2] = f->slot[3] = 2; }
    else if (r == 3) { f->slot[0] = 1; f->slot[1] = f->slot[2] = f->slot[3] = -1; }
}
/* 0x45f690: the figure that leaves the front: anim 1 on the running clock, then anim 0 (BlackBox: anim 1 once, restarted) */
static void car_anim_prev(void)
{
    Instance *f = g_car.fig[g_car.sel - 1]; if (!f) return;
    if (g_car.role[g_car.sel - 1] == 3) { f->a_speed = f->a_base_speed = 3.0f; f->a_start = g_now; f->a_ended = 0; f->slot[0] = 1; f->slot[1] = f->slot[2] = f->slot[3] = -1; }
    else { f->slot[0] = 1; f->slot[1] = f->slot[2] = f->slot[3] = 0; }
}
/* 0x45efb0 right / 0x45f050 left: not while turning or closing; 90 deg in 0.5 s, the page texts slide out */
static void car_turn(int right)
{
    if (g_car.rot_l || g_car.rot_r || M.p.closing) return;
    float a = g_car.pos; g_car.rt = 0;
    if (right) { if (a == 4.0f) a = 0; g_car.target = a == 4.0f ? 1.0f : a + 1.0f; }
    else { if (a == 1.0f) a = 5.0f; g_car.target = a == 1.0f ? 4.0f : a - 1.0f; }
    g_car.pos = g_car.start = a; g_car.rot_r = right; g_car.rot_l = !right;
    car_anim_prev(); M.p.t = 0; g_car.slide_out = 1;
}
/* vt[16] 0x45e800 + 0x45f1d0 / 0x45f0f0, every frame: linear, the selection (and with it name, stats, location) switches
 * at half time while the texts slide back in; at the end the new figure starts its animation */
static void car_rotate(float dt)
{
    g_car.rt += dt;
    if (g_car.rot_r || g_car.rot_l) {
        int right = g_car.rot_r; float t = g_car.target;
        if (g_car.rt < 0.5f) {
            g_car.pos = g_car.start + (right ? g_car.rt : -g_car.rt) / 0.5f;
            if (g_car.rt >= 0.25f) { g_car.sel = right ? (t == 5.0f ? 1 : (int)t) : (t == 0.0f ? 4 : (int)t); g_car.slide_in = 1; g_car.slide_out = 0; }
        } else {
            g_car.pos = right ? (t == 5.0f ? 1.0f : t) : (t == 0.0f ? 4.0f : t);
            g_car.rot_r = g_car.rot_l = 0; g_car.rt = 0; car_anim_sel(); M.p.t = 0; g_car.slide_in = 0;
        }
    }
    if (g_car.slide_in && !g_car.rot_r && !g_car.rot_l && M.p.t >= 0.5f) g_car.slide_in = 0;   /* the opening slide (0x45b990: done at +0x1c >= 0.5) */
    if (g_car.role[g_car.sel - 1] == 3) { k_page3[1].id = 1; k_page3[1].flags = 2; M.sel = 0; }   /* BlackBox: no high scores, item 1 = "" as a header */
    else { k_page3[1].id = 41; k_page3[1].flags = 0x21; }
}
/* 0x451890 (pos, 90, 150, 560, 100) -> 0x489210: a ring of radius 150, 560 ahead of and 100 below the eye, tilted 10 deg,
 * one figure every 90 deg and the one at `pos` in front, 7 deg to the right. Design camera space: x right, y DOWN,
 * z ahead, scaled back by the projection (1, 1/1.3333, 1/1.2) so that it lands on screen at (P.x / P.z, P.y / P.z).
 * The .ins models are z-up and face -y: model z goes to camera up, model -y to the outside of the ring, so the front
 * figure looks into the lens, and everything leans 10 deg with the ring. Verified live (tools/wverify.py --probe carousel):
 * 0x489780(-10 deg, theta, 0) builds L = Rx * Ry * Rz * Basis (row-vector matrices) and 0x489210 takes the COLUMNS of L as
 * the model axes, each put through the scaled inverse camera matrix and normalised - exactly the axes below. */
static void car_place(const FreeCamera *cam)
{
    Vec3 F = cam_forward(cam), R = cam_right(cam), U = { R.y * F.z - R.z * F.y, R.z * F.x - R.x * F.z, R.x * F.y - R.y * F.x };
    const float ct = cosf(10.0f * 3.14159265f / 180.0f), st = sinf(10.0f * 3.14159265f / 180.0f);
    for (int k = 0; k < 4; k++) {
        float th = ((k + 1 - g_car.pos) * 90.0f + 7.0f) * 3.14159265f / 180.0f, s = sinf(th), c = cosf(th);
        float px = 150.0f * s, py = 100.0f + 26.047f * c, pz = 560.0f - 150.0f * c;
        Vec3 P = { cam->pos.x + R.x * px - U.x * py * 0.75f + F.x * pz / 1.2f, cam->pos.y + R.y * px - U.y * py * 0.75f + F.y * pz / 1.2f, cam->pos.z + R.z * px - U.z * py * 0.75f + F.z * pz / 1.2f };
        float ax[3][3] = { { c, 0, s }, { -s, 0, c }, { 0, -1, 0 } }, m[16] = { 0 };   /* model x, y, z in camera space, before the tilt */
        for (int a = 0; a < 3; a++) {   /* model axis a = column a of 0x489780's Rx(-10)*Ry(theta)*Basis, through the same scaled inverse as the position, then normalised (0x489210) */
            float x = ax[a][0], y = ax[a][1] * ct - ax[a][2] * st, z = ax[a][1] * st + ax[a][2] * ct;   /* tilt about camera x: the front goes down */
            y *= 0.75f; z /= 1.2f;
            Vec3 v = { R.x * x - U.x * y + F.x * z, R.y * x - U.y * y + F.y * z, R.z * x - U.z * y + F.z * z };
            float l = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); if (l > 1e-6f) { v.x /= l; v.y /= l; v.z /= l; }
            m[a * 4 + 0] = v.x; m[a * 4 + 1] = v.y; m[a * 4 + 2] = v.z;
        }
        m[12] = P.x; m[13] = P.y; m[14] = P.z; m[15] = 1;
        Instance *two[2] = { g_car.fig[k], g_car.ped[k] };
        /* 0x489650 = 0x489210 then 0x4077f0(NULL): re-celled at the placed point every frame. Left to its clock (the animated
         * root), a figure the house swept through got no sector, dropped out of the list and never came back */
        for (int j = 0; j < 2; j++) if (two[j]) { two[j]->position = P; memcpy(two[j]->world.m, m, sizeof m); two[j]->cell_ok = 0; }
    }
}
static void carousel_frame(const FreeCamera *cam, float dt)                          /* after the camera, before the renderer */
{
    if (M.page != 3) { if (g_car.shown) car_hide(); return; }
    car_rotate(dt); car_place(cam);
}
static void carousel_enter(void)                                                    /* 0x45e660 */
{
    panel_enter(); M.sel = 0; g_car.slide_in = 1; g_car.slide_out = 0; g_car.rot_l = g_car.rot_r = 0; g_car.rt = 0;
    car_fill(); car_anim_sel();
}
static void carousel_update(const MenuKeys *k, float dt)
{
    (void)dt; int kk = g_car.sel - 1;
    if (k->right) car_turn(1); else if (k->left) car_turn(0);
    if (k->up || k->dn) { int moved = g_car.role[kk] != 3; if (moved) M.sel ^= 1; hud_menu_blink(k->up || moved ? 0.0f : 0.25f); }   /* 0x45bb90 / 0x45bba0: two items, round */
    if (k->ok) {                                                                    /* 0x45ee50: only a free figure; no sound */
        if (!g_car.open[kk]) return;
        if (M.sel == 0) { panel_iris(0.37f, 0); M.p.closing = M.p.lock = 1; M.p.t = M.p.ti = 0; M.p.result = 14 + kk; M.p.wait = 0.5f; g_car.slide_out = 1; g_car.slide_in = 0; M.p1_iris = 0; }
        else if (kk != 3) { panel_iris(0.37f, 0); M.p.closing = M.p.lock = 1; M.p.t = M.p.ti = 0; M.p.result = 4; M.p.wait = 0.5f; g_car.slide_out = 1; g_car.slide_in = 0; M.hs_char = kk; }   /* the same close, result 4, [0x5e5a8c]+0x3c = k */
    } else if (k->back) { panel_close(0, 24); g_car.slide_out = 1; g_car.slide_in = 0; }
}
static int car_location(const SaveSlot *s, int c)                                  /* 0x450790: the first unfinished level of that character, -1 = all done */
{
    int a = c == 0 ? 2 : c == 1 ? 12 : 19, b = c == 0 ? 10 : c == 1 ? 17 : 24;
    for (int L = a; L <= b; L++) if (!s->chr[c].rec[L].done) return L;
    return -1;
}
/* 0x45ec50 (table 0x45ed60), the same pairs as the rows of page 4: 47 Space / 48 Pirate / 49 House + 51..54 Part A..D / 55 Race */
static const uint8_t k_loc_w[29] = { [2]=47,[3]=47,[4]=48,[5]=48,[6]=48,[7]=49,[8]=49,[9]=49,[10]=49, [12]=47,[13]=47,[14]=48,[15]=48,[16]=49,[17]=49, [19]=47,[20]=47,[21]=48,[22]=48,[23]=49,[24]=49 };
static const uint8_t k_loc_p[29] = { [2]=51,[3]=52,[4]=51,[5]=52,[6]=53,[7]=51,[8]=52,[9]=53,[10]=54, [12]=51,[13]=55,[14]=51,[15]=55,[16]=51,[17]=55, [19]=51,[20]=55,[21]=51,[22]=55,[23]=51,[24]=55 };
static void carousel_draw(float dt)
{
    (void)dt;
    if (M.p.lock && M.p.ti >= 0.5f) return;                                        /* 0x45b990: no content once the close has run out */
    static const int face[4] = { 0, 2, 1, 1 };                                     /* record +0x58 */
    const SaveSlot *s = &g_save; int k = g_car.sel - 1;
    HudCarousel h; memset(&h, 0, sizeof h);
    h.name = g_car.open[k > 0 ? k - 1 : 0] ? 30u + (uint32_t)k : 34u;               /* 0x45fe30: revealed once the one before is free */
    h.stats = g_car.open[k] && k != 3;                                             /* record +0x5c: BlackBox has no stats */
    if (h.stats) {
        const SaveChar *sc = &s->chr[k]; int L = car_location(s, k);
        h.face = face[k]; h.lives = sc->lives; h.unique = sc->unique; h.charges = sc->charges; h.health = sc->health; h.pct = slot_char_pct(s, k);
        if (L >= 0) { h.world = k_loc_w[L]; h.part = k_loc_p[L]; }
    }
    float t = M.p.t, sl = g_car.slide_in ? 0.5f - t : g_car.slide_out ? t : 0;
    h.off = sl * -600.0f;
    h.list = g_car.open[k] && !M.p.closing && !M.p.opening; h.items = k_page3; h.nitems = 2; h.list_sel = M.sel; h.yfrac = 0.85f + sl * 0.2f / 0.5f;   /* 0x45ffa0 */
    float ti = M.p.ti < 0.5f ? M.p.ti : 0.5f;
    h.arrow_s = (M.p.closing ? ti : 0.5f - ti) * 600.0f;                            /* 0x45fac0 */
    float g = g_car.slide_out ? 128.0f - t * 96.0f / 0.25f : g_car.slide_in ? (t - 0.25f) * 96.0f / 0.25f + 32.0f : 128.0f;
    h.arrow_r = g_car.rot_r ? g : 128.0f; h.arrow_l = g_car.rot_l ? g : 128.0f;     /* the arrow of the turn dims to 32 and back */
    for (int c = 0; c < 3; c++) for (int L = 0; L < 29; L++) h.total += s->chr[c].rec[L].best;
    h.total += s->extra;                                                           /* 0x450a10 */
    hud_carousel(&h);
}

/* ---- page 4: the high scores of one character (docs/MENU_LOAD.md 4.8; class 0x45bfb0, 0x40 B, vtable 0x4ab368,
 * global [0x5e5a8c]). A panel page whose enter 0x45bfd0 zeroes the iris target +0x30 right after the base enter, so
 * the panel base draws a black screen instead of the ring (0x45ba29); the page has one empty item (0x4b5e20: string 1,
 * result 5, y 0.8) and never draws a list. Confirm does nothing (vt[19] is an empty function), "back" closes with
 * SoundFx 0x3f and result 24, which the handler 0x405749 turns into page 3 again. The rows (0x45ca40 / 0x45cd80 /
 * 0x45cf90) follow the play order of the character's levels and stop at the first one not done (0x4509e0). */
static void scores_draw(void)
{
    static const int8_t first[3] = { 2, 12, 19 }, last[3] = { 10, 17, 24 }, face[3] = { 0, 2, 1 };   /* 0x45c150 -> 0x45c230(k): 0 -> 0, 1 -> 2, 2 -> 1 */
    int c = M.hs_char >= 0 && M.hs_char < 3 ? M.hs_char : 0;
    HudScores h; memset(&h, 0, sizeof h); h.face = face[c];
    float ti = M.p.ti;                                                             /* 0x45bff0: +0x28 against +0x34 = 0.5 */
    if (ti <= 0.5f) { h.slide = (M.p.closing ? ti : 0.5f - ti) * 600.0f / 0.5f; h.grow = (M.p.closing ? 0.5f - ti : ti) / 0.5f; } else { h.slide = 0; h.grow = 1; }
    for (int L = first[c]; L <= last[c] && h.nrows < 9; L++) {
        const SaveRec *r = &g_save.chr[c].rec[L]; if (!r->done) break;
        HudScoreRow *w = &h.row[h.nrows++];
        w->world = k_loc_w[L]; w->part = k_loc_p[L]; w->best = r->best; w->time = r->time;
        w->race = L == 13 || L == 15 || L == 17 || L == 20 || L == 22 || L == 24;  /* 0x45c702: 0xd 0xf 0x11 0x14 0x16 0x18 */
        w->en_got = r->st[1]; w->en_tot = r->st[0]; w->w_got = r->st[3]; w->w_tot = r->st[2];   /* 0x4502c0 / 0x450290, 0x450320 / 0x4502f0 */
    }
    hud_scores(&h);
}

static void title_music_next(void) { if (++M.title_music == 2) M.title_music = 0; audio_music(M.title_music == 1 ? 0 : 48); }   /* 0x404e30: track 0 "Menu" after a load, 48 "Menu02" after an attract */

static void menu_enter(int page)
{
    M.page = page; M.delay = 0; hud_menu_blink(0);
    if (wenv("WOODY_MENULOG")) printf("menu: page 0x%x at %.2f s\n", page, g_now);
    switch (page) {
    case 1:                                                            /* 0x460070 */
        M.sel = 0; audio_fx(63, NULL, NULL);
        memset(&M.p, 0, sizeof M.p); M.p.iris_on = M.p1_iris; M.p.opening = 1; panel_iris(0, 0.85f);
        break;
    case 2: panel_enter(); for (int k = 0; k < 4 && !slot_pct(&g_file.slot[M.slot2_sel - 1]); k++) if (M.slot2_sel < 4) M.slot2_sel++; break;   /* 0x45dd30 */
    case 3: carousel_enter(); break;
    case 4: panel_enter(); panel_iris(0, 0); M.sel = 0; break;          /* 0x45bfd0: base enter, then +0x30 = 0 */
    case 5: panel_enter(); panel_iris(1.0f, 0.37f); break;           /* 0x45e230: you come from the game */
    case 0x1b:                                                         /* 0x460240: the cursor on "Sound FX volume", the values backed up */
        M.opt_bak[0] = g_opt.sfx; M.opt_bak[1] = g_opt.music; M.opt_bak[2] = g_opt.vib;
        k_page1b[1].value = g_opt.sfx; k_page1b[2].value = g_opt.music; k_page1b[3].value = g_opt.vib; M.sel = 1;
        k_page1b[5].id = hud_port_str("Display"); k_page1b[6].id = hud_port_str("Graphics"); k_page1b[7].id = hud_port_str("Controls"); break;   /* port extras */
    case 0x40: M.disp = g_dnow; disp_items(); M.sel = 1; break;        /* port page: the cursor on the first choice */
    case 0x42: M.disp = g_dnow; gfx_items(); M.sel = 1; break;
    case 0x41: g_ctl.dev = g_in.pad.kind != PADK_NONE; g_ctl.cap = 0;  /* port page: the pads when one is there */
        memcpy(g_ctl.bak, g_in.bind, sizeof g_ctl.bak); memcpy(g_ctl.pbak, g_in.pbind, sizeof g_ctl.pbak); g_ctl.bak_mode = g_in.mode; g_ctl.bak_cfg = g_in.have_cfg;
        g_ctl.bak_speed = g_cam_speed; ctl_items(); M.sel = 1; break;
    case 0x1c: M.sel = 2; break;                                       /* 0x45bd40: on "No" */
    case 0x17: M.sel = 3; break;                                       /* 0x45b370: base enter, then sel = 3 = "No" */
    case 0x18: case 0x19: case 0x1f: M.sel = 0; hud_logo_off(); break; /* 0x45b390 */
    case 0x1d: M.go_t = 5.0f; break;                                   /* 0x45bbb0: base enter, +0x14 = 5.0 */
    case 0x20: M.sel = 0; M.cred_t = 0; hud_credits_enter(); break;   /* 0x45bd60: base enter, the roll 0x4538f0(0), +0x14 = +0x18 = +0x1c = 0 */
    default: M.sel = menu_first(); break;
    }
}
static void menu_off(void) { M.page = -1; M.results = 0; }
/* 0x404d80 Pause_Open: page 0x19 when the Perso rides (state 1) and this is no BlackBox, with the follow distance at 200
 * (0x41fa80(200.0)); otherwise 0x18. The app goes to state 0 (0x401400), so the world stands still (table 0x405af8). */
static void pause_open(void)
{
    if (g_player && g_player->race_char && g_level != 25) { g_player->cam_dist = 200.0f; menu_enter(0x19); }
    else menu_enter(0x18);
}
static void menu_back_to_level_menu(void) { if (g_level == 0) menu_enter(1); else pause_open(); }   /* 0x4057b9 / 0x404d80 */
static void menu_title_page0(void) { title_music_next(); menu_enter(0); }   /* 0x404e30 */

/* page 1 "New game" (and the attract, the same script start): the House script object 115 plays the intro */
static void menu_new_game(EkoVM *vm, int attract)
{
    int32_t *iv = g_have_intro && (g_intro_var & 0xffffff) < vm->nvars ? &vm->varval[g_intro_var & 0xffffff] : NULL;
    hud_logo_off(); M.newgame = !attract;
    if (iv) eko_set_var(vm, g_intro_var, 1);                          /* SetVar(app+0x8c, 1) (0x4051b0), always */
    else if (!attract) { save_reset(); request_level(1, 0.5f); menu_off(); return; }   /* no intro in this House script */
    M.page = 0x1f; M.sel = 0;
}
static void menu_load_chain(void)                                     /* 0x4051da: no file -> page 7, else the wait page 0xb (app+0x5c = 0) */
{
    int r = file_read();
    if (r == 0) { menu_enter(7); return; }
    M.wait = 0; M.wait_r = r; menu_enter(0xb);                           /* 0x405276 reads one frame later: failed -> 0xa, else page 2 */
}
static void menu_save_slot(int s)                                     /* 0x405536 / 0x405609 -> 0x456dc0, then the wait page 0xc (app+0x5c = 2) */
{
    g_file.slot[s] = g_save; g_file.music[s] = (uint32_t)g_opt.music; g_file.sfx[s] = (uint32_t)g_opt.sfx; g_file.vib[s] = g_opt.vib * 0.01f;
    M.save_s = s; M.wait = 2; menu_enter(0xc);
}
/* the wait pages 0xb (read for "Load game"), 0xc (write) and 0xe (read before page 5): empty pages over the dim layer;
 * the handler counts app+0x5c down once per frame and acts on the frame after it reached 0 (0x4052a4) */
static void menu_wait(void)
{
    if (M.wait > 0) { M.wait--; return; }
    switch (M.page) {
    case 0xb: menu_enter(M.wait_r < 0 ? 0xa : 2); break;                 /* 0x405276 */
    case 0xe: menu_enter(M.wait_r < 0 ? 6 : 5); break;                   /* 0x405483: unreadable -> back to "Do you want to save?" */
    case 0xc: { int ok = file_write(); menu_enter(ok ? 8 : 9); break; }   /* 0x405662 */
    }
}

/* one frame of the current page; runs after the world (0x404e90: Game_Frame first, then the menu), not while a
 * level change or the quit fade runs (0x404f71). Returns nothing: the pages act on the globals directly. */
static void menu_update(EkoVM *vm, const MenuKeys *k, float dt)
{
    if (M.quitting || g_next_level >= 0) return;
    /* the panel clock: the deferred result of pages 1, 2, 3, 5 */
    if (M.page == 1 || M.page == 2 || M.page == 3 || M.page == 4 || M.page == 5) {
        M.p.t += dt; M.p.ti += dt; M.p.iris_t += dt;
        if (M.p.opening && M.p.t >= 0.5f) M.p.opening = 0;
        if (M.p.closing && M.p.ti > M.p.wait) {
            int r = M.p.result; M.p.closing = 0;
            switch (M.page) {
            case 1:
                if (r == 1) { M.p1_iris = 0; M.page = 0x1f; hud_logo_off(); }       /* 0x4051c5; the script start went out with result 2 */
                else if (r == 3) menu_load_chain();
                else if (r == 6) menu_enter(0x1b);
                else if (r == 7) menu_enter(0x1c);
                return;
            case 2:
                if (r == 24) { menu_enter(1); return; }
                {   int s = r - 10; g_save = g_file.slot[s];                                              /* 0x456df0, 0x4052db: the slot's own volumes too */
                    g_opt.music = g_file.music[s] > 100 ? 100 : (int)g_file.music[s]; g_opt.sfx = g_file.sfx[s] > 100 ? 100 : (int)g_file.sfx[s];   /* unsigned: a damaged slot cannot give a negative volume */
                    opt_apply(); car_reset(); menu_enter(3); }
                return;
            case 3:
                car_hide();                                                                               /* 0x45e870: the 8 figures go with the result */
                if (r == 24) { menu_enter(1); return; }
                if (r >= 14 && r <= 17) { static const int hub[4] = { 1, 11, 18, 25 }; M.p1_iris = 0; menu_off(); request_level(hub[r - 14], 0.4f); }   /* 0x4056c8 */
                else if (r == 4) menu_enter(4);                                                           /* 0x40573b: SEE HIGH SCORES */
                return;
            case 4:
                if (r == 24) menu_enter(3);                                                               /* 0x405749 */
                return;
            case 5:
                if (r == 24) { menu_enter(6); return; }
                M.save_s = r - 10;
                if (slot_pct(&g_file.slot[M.save_s])) menu_enter(0x17); else menu_save_slot(M.save_s);   /* 0x4054ac */
                return;
            }
        }
    }
    if (M.page == 0xb || M.page == 0xc || M.page == 0xe) { menu_wait(); return; }
    if (M.delay > 0) { M.delay -= dt; return; }                      /* 0x4464f0: no input while the delay runs */
    int32_t *iv = g_have_intro && (g_intro_var & 0xffffff) < vm->nvars ? &vm->varval[g_intro_var & 0xffffff] : NULL;
    int n; float yf; const MenuItem *it = menu_items(M.page, &n, &yf);
    int list = it && M.page != 0 && M.page != 0x1f;                   /* plain list navigation */
    if (list) { if (k->up) menu_move(-1); if (k->dn) menu_move(+1); }
    switch (M.page) {
    case 0: {                                                          /* 0x404fe4 */
        if (k->syn) { menu_new_game(vm, 0); break; }
        int was = M.attract > 0; M.attract -= dt;
        if (M.attract <= 0) {
            if (was && iv) eko_set_var(vm, g_intro_var, 1);
            if (M.attract < -0.5f) { M.newgame = 0; M.page = 0x1f; hud_logo_off(); }
            break;
        }
        if (k->ok || k->esc_rel) menu_enter(1);                       /* confirm (result 5) or Esc released; nothing else, despite the text */
        break; }
    case 1:                                                            /* 0x4600e0 validate, 0x40519f handler */
        if (k->syn) { menu_new_game(vm, 0); break; }
        if (M.p.lock || !k->ok) break;
        M.p.lock = M.p.closing = 1; M.p.ti = 0;
        switch (M.sel) {
        case 0: if (!iv) { M.p.lock = 0; menu_new_game(vm, 0); break; }                                   /* result 2 now (the script start), 1 in the next frame */
                M.p1_iris = 0; M.newgame = 1; hud_logo_off(); eko_set_var(vm, g_intro_var, 1); M.p.result = 1; M.p.wait = 0; break;
        case 1: M.p1_iris = M.p.iris_on = 1; panel_iris(0.85f, 0); hud_logo_off(); M.p.result = 3; M.p.wait = 0.5f; break;
        case 2: M.p.result = 6; M.p.wait = 0; break;
        case 3: M.p.result = 7; M.p.wait = 0; break;
        }
        break;
    case 0x1f: {                                                       /* 0x40508c */
        int v = iv ? *iv : 4;
        if (v != 4 && !k->atk_rel && !k->esc_rel && !k->syn && !k->ok) break;   /* the attack key or Esc, RELEASED; Enter does not skip in the
                                                                       * original - PORT EXTRA: confirm (Enter released / jump pressed) skips too */
        M.attract = 35.0f;
        if (M.newgame) { save_reset(); menu_off(); request_level(1, v == 4 ? 0.0f : 0.5f); break; }   /* 0x44ffa0 in memory only, no write */
        if (v != 4) {                                                  /* the attract was broken off: stop the script, the cinematic and its stream */
            if (g_intro_obj) eko_cancel_timers(vm, (uint32_t)g_intro_obj);
            eko_set_var(vm, g_intro_var, 4);
            memset(&g_cin, 0, sizeof g_cin); audio_rtc(-1); audio_music_pause(0, 0.45f);
        }
        memset(&g_sfade, 0, sizeof g_sfade); hud_text_reset(); g_black_frame = 1; fade_start(0.5f, 0);
        g_title_t = 0; menu_title_page0();
        break; }
    case 0x1c:                                                         /* 0x4057a5 */
        if (k->ok && M.sel == 1) {
            if (g_level == 0) { M.quitting = 1; M.quit_t = 0.5f; fade_start(0.5f, 1); audio_music_stop(0.45f); audio_rtc(-1); }   /* 0x404cb0 */
            else { menu_off(); request_level(0, 0.5f); }
        } else if ((k->ok && M.sel == 2) || k->back) menu_back_to_level_menu();
        break;
    case 0x1b: {                                                       /* 0x4601f0; left/right +-5 in 0..100, applied at once */
        int step = k->right ? 5 : k->left ? -5 : 0;
        if (step && M.sel >= 1 && M.sel <= 3) {
            MenuItem *e = &k_page1b[M.sel]; e->value += step; if (e->value > 100) e->value = 100; if (e->value < 0) e->value = 0; hud_menu_blink(0.25f);
            if (M.sel == 1) g_opt.sfx = e->value; else if (M.sel == 2) g_opt.music = e->value; else { g_opt.vib = e->value; pad_set_strength(g_opt.vib * 0.01f); pad_rumble(1.0f, 0.15f); }   /* port extra: a short rumble to feel the new strength */
            opt_apply();
        }
        if (k->ok && M.sel == 4) { opt_write(); menu_back_to_level_menu(); }                          /* Continue keeps the values */
        else if (k->ok && M.sel == 5) menu_enter(0x40);                                                /* port extra: the Display page */
        else if (k->ok && M.sel == 6) menu_enter(0x42);                                                /* port extra: the Graphics page */
        else if (k->ok && M.sel == 7) menu_enter(0x41);                                                /* port extra: the Controls page */
        else if (k->back) { g_opt.sfx = M.opt_bak[0]; g_opt.music = M.opt_bak[1]; g_opt.vib = M.opt_bak[2]; opt_apply(); menu_back_to_level_menu(); }   /* 0x4602a0 */
        break; }
    case 0x40:                                                         /* port page (docs/DISPLAY.md 4): left/right change a choice, Continue applies + saves, back drops the edit */
        if ((k->right || k->left) && M.sel >= 1 && M.sel <= 5) { disp_step(M.sel, k->right ? 1 : -1); disp_items(); hud_menu_blink(0.25f); }
        if (k->ok && M.sel == 6) { g_disp = g_dnow = M.disp; g_disp_dirty = 1; opt_write(); }
        if ((k->ok && M.sel == 6) || k->back) { M.page = 0x1b; M.sel = 5; M.delay = 0; hud_menu_blink(0); }   /* back to Options on "Display", its backups kept */
        break;
    case 0x42:                                                         /* port page "Graphics": as the Display page; Continue applies at once (no window change) */
        if ((k->right || k->left) && M.sel >= 1 && M.sel <= 4) { gfx_step(M.sel, k->right ? 1 : -1); gfx_items(); hud_menu_blink(0.25f); }
        if (k->ok && M.sel == 5) { g_dnow.ao = M.disp.ao; g_dnow.aniso = M.disp.aniso; g_dnow.smaa = M.disp.smaa; g_dnow.msaa = M.disp.msaa;
            g_disp.ao = g_dnow.ao; g_disp.aniso = g_dnow.aniso; g_disp.smaa = g_dnow.smaa; g_disp.msaa = g_dnow.msaa; gfx_apply(); opt_write(); }
        if ((k->ok && M.sel == 5) || k->back) { M.page = 0x1b; M.sel = 6; M.delay = 0; hud_menu_blink(0); }
        break;
    case 0x41:                                                         /* port page "Controls": the capture itself runs in ctl_capture */
        if ((k->right || k->left) && M.sel == 1) { g_ctl.dev ^= 1; ctl_items(); hud_menu_blink(0.25f); }
        if (k->ok && M.sel >= 2 && M.sel <= 12) { g_ctl.cap = 1; g_ctl.phase = 0; g_ctl.cap_t = 0; g_ctl.pad_prev = g_in.pad.buttons; ctl_items(); }
        if ((k->right || k->left) && M.sel == 13) {                    /* camera speed: the next / previous step (a cfg value between steps goes to a neighbour) */
            int n = (int)(sizeof k_cam_speeds / sizeof *k_cam_speeds), i = 0; while (i < n - 1 && k_cam_speeds[i] < g_cam_speed) i++;
            if (k->right && k_cam_speeds[i] <= g_cam_speed && i < n - 1) i++; else if (k->left && i > 0) i--;
            g_cam_speed = k_cam_speeds[i]; ctl_items(); hud_menu_blink(0.25f); }
        if (k->ok && M.sel == 14) { ctl_defaults(g_ctl.dev); g_cam_speed = 100; ctl_items(); hud_menu_blink(0.25f); puts("input: defaults"); }
        if (k->ok && M.sel == 15) {
            if (memcmp(g_ctl.bak, g_in.bind, sizeof g_ctl.bak) || memcmp(g_ctl.pbak, g_in.pbind, sizeof g_ctl.pbak)) g_ctl.custom = 1;
            opt_write();
        }
        if (k->back) { memcpy(g_in.bind, g_ctl.bak, sizeof g_ctl.bak); memcpy(g_in.pbind, g_ctl.pbak, sizeof g_ctl.pbak); g_in.mode = g_ctl.bak_mode; g_in.have_cfg = g_ctl.bak_cfg; g_cam_speed = g_ctl.bak_speed; }
        if ((k->ok && M.sel == 15) || k->back) { M.page = 0x1b; M.sel = 7; M.delay = 0; hud_menu_blink(0); }
        break;
    case 3: if (!M.p.lock) carousel_update(k, dt); break;
    case 4: if (!M.p.lock && k->back) { panel_close(0, 24); panel_iris(0, 0); } break;   /* 0x45bb30 -> 0x45bb40: iris +0x30 = 0 -> 0; confirm is vt[19] = ret */
    case 7: case 0xa: if (k->ok) menu_enter(1); break;               /* 0x405075: only "Continue" */
    case 2: case 5: {
        int *sel = M.page == 2 ? &M.slot2_sel : &M.slot5_sel;
        if (M.p.lock) break;
        if (k->dn) *sel = slot_step(M.page, *sel, 0); if (k->up) *sel = slot_step(M.page, *sel, 1);
        if (k->right) *sel = slot_step(M.page, *sel, 2); if (k->left) *sel = slot_step(M.page, *sel, 3);
        if (k->ok && (M.page == 5 || slot_pct(&g_file.slot[*sel - 1]))) { panel_close(1, 10 + *sel - 1); if (M.page == 5) panel_iris(0.37f, 1.0f); }   /* a free slot cannot be loaded: nothing, no sound */
        else if (k->back) panel_close(0, 24);
        break; }
    case 6:                                                            /* 0x405358 */
        if (k->ok && M.sel == 2) { int r = file_read(); if (r == 0) { file_reset(); menu_enter(5); } else { M.wait = 0; M.wait_r = r; menu_enter(0xe); } }   /* 0x405358: no file -> four free slots (0x456e20) */
        else if (k->ok && M.sel == 3) menu_off();                                                   /* results_update closes the panel */
        break;
    case 0x17: if (k->ok && M.sel == 2) menu_save_slot(M.save_s); else if ((k->ok && M.sel == 3) || k->back) menu_enter(6); break;   /* 0x405586 */
    case 8: if (k->ok) menu_off(); break;                              /* 0x4056c0: "Game Saved" leaves the menu */
    case 0x20:                                                         /* the credits (docs/CREDITS.md): the draw counts +0x18, validate 0x446e90 = 5 once it is past 5 s; */
        M.cred_t += dt;                                                /* back / Esc give 24, which the handler ignores: no way out in the first 5 s */
        if (k->ok && M.cred_t > 5.0f) request_level(0, 0.5f);         /* 0x40577b -> 0x405780: 0x404b60(0.5, 0, 0, 0), the page stays up during the fade */
        break;
    case 9: if (k->ok) menu_enter(6); break;
    case 0x1d:                                                         /* GAME OVER: the draw 0x45bbd0 counts +0x14 down and sets +0xc = 1 every frame (no confirm), */
        if (M.go_t > 0 && (M.go_t -= dt) <= 0) {                       /* back gives 24, ignored; result 5 in the frame the timer crosses 0 -> handler 0x405796 */
            save_reset();                                              /* 0x44ffa0: the active save wiped in memory (9 lives, 3.0 health), Woody.sav is not written */
            request_level(0, 0.5f);                                    /* 0x405780: 0x404b60(0.5, 0, 0, 0) -> the title; the page stays up during the fade */
        }
        break;
    case 0x18: case 0x19: {                                            /* 0x4057f5, table 0x405cfc on result - 5; "back" does nothing */
        static const int res18[3] = { 5, 6, 7 }, res19[4] = { 5, 18, 6, 7 };
        int r = !k->ok ? 0 : M.page == 0x19 ? res19[M.sel & 3] : res18[M.sel % 3];
        if (r == 5) { if (M.page == 0x19 && g_player) g_player->cam_dist = 4.0f; menu_off(); }   /* 0x40580c: Continue, back to state 1 (3 in BlackBox) */
        else if (r == 18) { if (g_player) { g_player->cam_dist = 4.0f; player_restart(g_player); } menu_off(); }   /* 0x40584d: "Start again" */
        else if (r == 6) menu_enter(0x1b);                             /* 0x40587a */
        else if (r == 7) menu_enter(0x1c);                             /* 0x405888 */
        break; }
    }
}

/* table 0x405af8: the half-black backdrop and whether the world stands still */
static int menu_overlay(int page) { return page == 7 || page == 0xa || page == 0xb || page == 0xc || page == 0xe || page == 6 || page == 8 || page == 9 || page == 0x17 || (g_level != 0 && ((page >= 0x18 && page <= 0x1c) || page == 0x40 || page == 0x41 || page == 0x42)); }   /* 0x40..0x42: port pages, as 0x1b */
static int menu_pauses_world(void) { return g_level != 0 && ((M.page >= 0x18 && M.page <= 0x1c) || M.page == 0x40 || M.page == 0x42 || M.page == 0x20 || M.page == 0x1d); }   /* 0x20: the credits level stands still; 0x1d GAME OVER (table 0x405af8 = 0) */

/* the page layer of a frame: items, then the iris, then the logo (docs/TITLE.md 5.4) */
static void menu_draw(float dt)
{
    int page = M.page;
    if (page >= 0) hud_menu_tick(dt);
    if (page >= 0 && menu_overlay(page)) hud_rect(0x80000000);
    int n; float yf; const MenuItem *it = menu_items(page, &n, &yf);
    switch (page) {
    case 1: hud_menu_items(it, n, yf, M.sel, 1); if (M.p.iris_on) hud_iris(M.p.opening && M.p.t == 0 ? 0 : panel_iris_v()); break;
    case 2: case 5: {
        if (M.p.opening && M.p.t == 0) hud_iris(0);
        hud_iris(panel_iris_v());
        HudSlots h; slots_info(&h, page); if (!(M.p.lock && M.p.ti >= 0.5f)) hud_slot_list(&h, dt);
        break; }
    case 3: if (M.p.opening && M.p.t == 0) hud_iris(0); hud_iris(panel_iris_v()); carousel_draw(dt); break;
    case 4: hud_rect(0xfe000000); if (!(M.p.lock && M.p.ti >= 0.5f)) scores_draw(); break;   /* 0x45ba29: iris target 0 = a black rect, then vt[17] */
    case 0x20: hud_credits(g_prev_level, dt); break;                  /* 0x45bd90: the page draws no item list */
    case 0x1d: hud_gameover(); break;                                  /* 0x45bbd0 */
    case 0x1f: case -1: break;
    default: if (it) hud_menu_items(it, n, yf, M.sel, M.delay <= 0); break;
    }
    if (g_level == 0) hud_logo(page == 0 || (page == 1 && !M.p.closing && !(M.p.iris_on && M.p.opening)), dt);   /* 0x446ac0 / 0x446b00 */
}

static void results_update(EkoVM *vm, float dt, int ok)               /* the table 0x4542c4 of 0x454090 */
{
    if (!g_res.on || !g_player) return;
    switch (g_res.state) {
    case 0:                                                                            /* he is coming down; wait until the Perso is free again (+0x21c == 0) */
        if (ok) hud_results_confirm();                                                 /* the page already takes input: after 0.5 s this marks every line as done (RESULTS.md 4.1) */
        if (!g_player->script_act) { results_action(0x4b); g_res.state = 1; }
        break;
    case 1:                                                                            /* 0x454560: iris, texts and the counting lines; OK when everything has been counted */
        hud_results_show();
        if (!g_player->script_act) results_action(0x4b);                               /* 0x45410a: 0x4b again, he stays lying under the parasol */
        if (ok && hud_results_confirm()) {                                             /* 0x4545a0 result 5; the first OK while counting only skips to the end */
            g_res.cats = results_cats(g_stats.stats);
            g_player->unique_items += g_res.cats;                                      /* 0x44c840: n unique items straight into the save block */
            g_save.chr[g_char].unique = g_player->unique_items;
            results_action(g_res.cats ? 0x4e : 0x4c);                                  /* 0x453fc0: 0x453fd6 cheering, 0x453ffd shrugging */
            g_res.state = g_res.cats ? 2 : 3; hud_results_hide();                         /* 0x454580: texts gone at once, the iris opens */
        }
        break;
    case 2: case 3:
        if (!g_player->script_act) { results_action(0x4d); results_store(); g_res.state = 4; menu_enter(6); M.results = 1; }   /* 0x454020: page 6, the cursor on "Yes" */
        break;
    case 4:                                                                            /* the save pages 6 -> 5 -> 0x17 -> 8 / 9 run as menu pages; "No" or "Game Saved" -> Continue ends them */
        if (!g_player->script_act) results_action(0x4d);                               /* 0x454164: 0x4d in a loop */
        if (M.page < 0) results_close();
        break;
    default:                                                                           /* 5: the fade-out is running */
        if (!g_player->script_act) results_action(0x4d);                               /* 0x454192 */
        if ((g_res.t -= dt) > 0) break;
        if (g_prop) g_prop->visible = 0;                                               /* 0x407850 */
        fade_start(0.5f, 0);
        g_player->script_act = 0; g_player->use_root = 0;                              /* "No" can come before 0x4d has played out; its root motion must not move him after this */
        player_place(g_player, g_res.door_p, (g_res.door_d.x * g_res.door_d.x + g_res.door_d.z * g_res.door_d.z) > 1e-6f ? atan2f(-g_res.door_d.x, -g_res.door_d.z) : g_player->yaw);   /* 0x454244: facing P0 - P1, away from the door */
        player_ground_snap(g_player);                                                  /* 0x45423f */
        g_cam.cut = 1; cam_set_mode(1); g_player->cam_init = 0;                        /* 0x41f9f0(2) + SetMode(0, 0) */
        eko_set_var(vm, g_res.var, 1);                                                 /* 0x45422c: the hub script opens the next door */
        g_res.on = 0; g_stats.have = 0;
        puts("  RESULTS done");
        break;
    }
}

/* ---- lasers: classes 50 / 51 / 52 (docs/OBJECTS.md 2.1). One beam per typecode-0 vector marker; off until message 50.
 * 51: marker start along the marker direction for `len` (message 52, default 400), cut at the first world polygon or instance press node;
 * 50: endless until the first hit; 52: to the marker start of the target instance (message 53). Touching a beam kills (Kill(2)). */
/* one lazer effect object per marker (0x46e4a0, 0x40 B, flags +0x34 = 0x3c0 for every laser): the timers of the
 * travelling pulse, the lightning arc and the impact at the hit point, plus the arc's own ring of 32 side offsets
 * (pool 0x5e82a8, 100 lazers x 0x20 points of 0x1c B) */
typedef struct { float acc, s, arc_t, salvo, imp_t, imp_r; int sub, regen, npts; float pts[32][2]; } LaserFx;
typedef struct { Instance *inst, *target; int type, on; float len, phase; LaserFx fx[8]; } Laser;
static Laser g_lasers[64]; static int g_nlasers;
static Laser *laser_of(const Instance *in) { for (int i = 0; i < g_nlasers; i++) if (g_lasers[i].inst == in) return &g_lasers[i]; return NULL; }
static int laser_segment(const Laser *z, uint32_t marker, const GelFile *gel, Vec3 *a, Vec3 *b, int *kind)
{
    const Model *mo = z->inst->model; uint32_t seen = 0;
    for (uint32_t i = 0; i < mo->nnodes; i++) {
        const InsNode *n = &mo->nodes[i]; if (n->kind != 0x20 || n->type_code != 0 || n->npoints < 2) continue;
        if (seen++ != marker) continue;
        Vec3 p0 = ins_point_world(z->inst, n->point_base), p1 = ins_point_world(z->inst, n->point_base + 1), d = { p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
        float l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z); if (l < 1e-4f) return 0;
        float len = z->type == 50 ? 100000.0f : z->len; *a = p0; *kind = 0;
        if (z->type == 52) { Vec3 t0, td; if (z->target && inst_vector(z->target, 0, &t0, &td)) { *b = t0; *kind = 2; return 1; } if (marker) return 0; }
        b->x = p0.x + d.x / l * len; b->y = p0.y + d.y / l * len; b->z = p0.z + d.z / l * len;
        /* 51: Ray 0x4359b0(a, b, -1), 50: the endless ray 0x435810(a, a + dir, -1); both stop at the first world polygon (kind 1) OR
         * press node of an instance (kind 2), and the -1 is the start cell (look it up), not an instance to leave out: the beam's own
         * housing is not skipped, the marker simply starts outside its press node (W1A model 33: marker z -62.9, press tip z -60.4)
         * or inside it, which the one-sided test lets the beam leave (W3B 737, model 47).
         * Woody and the enemies have no press nodes, so the beam passes through them; only the hit test below kills (the player) */
        const Instance *hi = NULL;
        if (z->type == 51 && inst_point_in_press(gel, &g_ins,NULL, *a, &hi)) {       /* hit kind 3 (raw answer 2, 0x4330c0 in the instance test 0x432ab0 = vtbl[5]): the
                                                                                    * start lies inside a press node; [0x53a558] = 0 (0x4330d4), so 0x4513cd puts b on a,
                                                                                    * rec+0x2c = 1 and the normal 0x4b3108 stays stale: no beam, only the impact at a. The
                                                                                    * endless test 0x431de0 (vtbl[6], type 50) has no such answer */
            g_hit_frac = 0; *b = *a; *kind = 1;
            if (wenv("WOODY_FXLOG")) { static const Instance *last3[64][8]; int zi = (int)(z - g_lasers); if (zi >= 0 && zi < 64 && marker < 8 && last3[zi][marker] != hi) { last3[zi][marker] = hi; printf("laser %u marker %u starts inside instance %u (hit kind 3)", z->inst->index, marker, hi->index), puts(""); } }
            return 1;
        }
        float t = gel_ray_frac(gel, *a, *b), tw = t < 1.0f ? t : 1.0f, ti;       /* the instances only up to the wall: a small box to cull with */
        Vec3 bw = { a->x + (b->x - a->x) * tw, a->y + (b->y - a->y) * tw, a->z + (b->z - a->z) * tw };
        if (inst_ray_press(gel, &g_ins,NULL, *a, bw, &ti, NULL, &hi) && ti * tw < t) t = ti * tw; else hi = NULL;
        if (t <= 1.0f) { b->x = a->x + (b->x - a->x) * t; b->y = a->y + (b->y - a->y) * t; b->z = a->z + (b->z - a->z) * t; *kind = 1; g_hit_frac = z->type == 50 ? t * len : t; }   /* [0x53a558]: 50 = the distance (unit dir) */
        else if (z->type == 50) {                                                   /* 0x4511a2: nothing hit, b = a + dir * [0x53a558] with the STALE value (the last writer's) */
            b->x = a->x + d.x / l * g_hit_frac; b->y = a->y + d.y / l * g_hit_frac; b->z = a->z + d.z / l * g_hit_frac;
            if (wenv("WOODY_FXLOG")) printf("laser %u marker %u: the endless ray finds nothing, stale length %.1f", z->inst->index, marker, g_hit_frac), puts("");
        }
        if (z->type == 50) *kind = 1;                                               /* 0x45122d: rec+0x2c = 1 whether or not the ray found anything */
        if (hi && wenv("WOODY_FXLOG")) { static const Instance *last[64][8]; int zi = (int)(z - g_lasers); if (zi >= 0 && zi < 64 && marker < 8 && last[zi][marker] != hi) { last[zi][marker] = hi; printf("laser %u (type %d) marker %u stops on instance %u (model %d) at %.0f %.0f %.0f, %.0f long", z->inst->index, z->type, marker, hi->index, (int)(hi->model - g_ins.models), b->x, b->y, b->z, sqrtf((b->x - a->x) * (b->x - a->x) + (b->y - a->y) * (b->y - a->y) + (b->z - a->z) * (b->z - a->z))), puts(""); } }
        return 1;
    }
    return 0;
}
/* hit test 0x450f80: vtbl[24] 0x44cd60 of the Perso = {pos + (0, H/2, 0), 69, H/2}; 0x433de0(a, b, centre, 69 * 0.85 (0x4aa3d8), H/2)
 * answers 0.5 or -1, and 0 <= t <= 1 kills: y range feet + 0.1 .. feet + H - 0.1 (H = 61 while ducked: he ducks under the beam) */
static int laser_hits_player(Vec3 a, Vec3 b, const Player *p)
{
    float hh = player_body_height(p) * 0.5f, t = seg_cyl(a, b, (Vec3){ p->pos.x, p->pos.y + hh, p->pos.z }, 69.0f * 0.85f, hh);
    return t >= 0 && !(t > 1.0f);                                                   /* 0x450ff2, 0x451002 */
}
static float fx_rnd(void);
static void laser_fx_init(LaserFx *fx)                                             /* 0x46e4a0 */
{
    memset(fx, 0, sizeof *fx);
    fx->s = -1.0f; fx->salvo = -1.0f; fx->regen = 1;
    fx->arc_t = fx_rnd() * 2.0f; fx->imp_r = fx_rnd() * 50.0f;
}
/* the rest of Lazer_Draw 0x46e530 after core and glow: g = the glow pulse, kind = rec+0x2c (0 free end, 1 on geometry, 2 to a target) */
static void laser_fx_draw(LaserFx *fx, Vec3 a, Vec3 b, int kind, float g, const float *eye, float dt)
{
    Vec3 d = { b.x - a.x, b.y - a.y, b.z - a.z };
    float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    fx->acc += fx_rnd() * 0.8f * dt; fx->arc_t += dt; fx->imp_t += dt;             /* 0x46e59b.. */
    /* 0x80, 0x46ea1d: a blob that runs down the beam in 0.25 s, on average every 2.5 s: two textured quads of 0.1 of the length either side of it */
    if (fx->acc > 1.0f) { fx->acc -= 1.0f; fx->s = 0; }
    if (fx->s >= 0 && fx->s < 1.0f) {
        fx->s += dt * 4.0f;
        if (fx->s < 1.0f) {
            if (len > 0) {                                                         /* a zero-length beam (hit kind 3): degenerate quads, nothing to see */
            static const float pc[3] = { 1, 0.6f, 0.6f };
            Vec3 p = { a.x + d.x * fx->s, a.y + d.y * fx->s, a.z + d.z * fx->s };
            Vec3 p1 = { a.x + d.x * (fx->s + 0.1f), a.y + d.y * (fx->s + 0.1f), a.z + d.z * (fx->s + 0.1f) }, p2 = { a.x + d.x * (fx->s - 0.1f), a.y + d.y * (fx->s - 0.1f), a.z + d.z * (fx->s - 0.1f) };
            hud_world_beam(&p.x, &p1.x, eye, 25.0f, pc, g, 0); hud_world_beam(&p.x, &p2.x, eye, 25.0f, pc, g, 0);
            }
        } else fx->s = -1.0f;
    } else fx->s = -1.0f;
    /* 0x100, 0x46ebd4: every 2 s a 0.9 s salvo of lightning; its zigzag is new at 0, 0.3 and 0.6 s */
    if (fx->arc_t > 2.0f) { fx->arc_t -= 2.0f; fx->salvo = 0; }
    if (fx->salvo >= 0 && fx->salvo < 0.9f) {
        float t = fx->salvo += dt;
        int sub = t < 0.3f ? 1 : t < 0.6f ? 2 : t < 0.9f ? 3 : fx->sub;
        if (sub != fx->sub) { fx->sub = sub; fx->regen = 1; }
        int n = len < 1600.0f ? (int)(len * 0.02f) : 32;                           /* a kink every 50 units, at most 32 */
        float step = n > 0 ? len / (float)n : 0;
        if (fx->regen) {                                                            /* 0x47d370 + 0x47d160: offsets of +-25 across the beam */
            fx->npts = 0;
            for (int i = 0; i < n; i++) { float *q = fx->pts[fx->npts % 32]; q[0] = fx_rnd() * 50.0f - 25.0f; q[1] = fx_rnd() * 50.0f - 25.0f; if (fx->npts < 32) fx->npts++; }
            fx->regen = 0;
        }
        float al = fx_rnd();                                                        /* one flicker per frame for the whole arc */
        if (n > 0 && len > 0) {
            Vec3 w = { d.x / len, d.y / len, d.z / len }, u, v;                     /* 0x46d320: rows u, v, w = the beam direction */
            if (w.x < 0.001f && w.x > -0.001f && w.z < 0.001f && w.z > -0.001f) {
                v = (Vec3){ 0, w.z, -w.y }; float l = sqrtf(v.y * v.y + v.z * v.z); if (l > 0) { v.y /= l; v.z /= l; }
                u = (Vec3){ v.y * w.z - v.z * w.y, v.z * w.x - v.x * w.z, v.x * w.y - v.y * w.x };
            } else {
                u = (Vec3){ w.z, 0, -w.x }; float l = sqrtf(u.x * u.x + u.z * u.z); if (l > 0) { u.x /= l; u.z /= l; }
                v = (Vec3){ w.y * u.z - w.z * u.y, w.z * u.x - w.x * u.z, w.x * u.y - w.y * u.x };
            }
            static const float white[3] = { 1, 1, 1 };
            Vec3 prev = a;
            for (int i = 0; i < n; i++) {                                           /* point i sits step*(i+1) down the beam, the last one on b */
                const float *q = fx->pts[i < 32 ? i : 31]; float o = step * (float)(i + 1);
                Vec3 p = { a.x + w.x * o + u.x * q[0] + v.x * q[1], a.y + w.y * o + u.y * q[0] + v.y * q[1], a.z + w.z * o + u.z * q[0] + v.z * q[1] };
                float a0 = kind == 0 && i == 0 ? 0 : al, a1 = kind == 0 && i == n - 1 ? 0 : al;   /* a free end fades out */
                hud_world_line(&prev.x, &p.x, eye, 3.0f, white, a0, a1);
                prev = p;
            }
        }
    } else fx->salvo = -1.0f;
    /* 0x200, 0x46efb9: where the beam hits geometry, four crossed quads of bank 0 image 5. The spark loop there
     * (50 a second) only draws two random numbers: its spawn is compiled out, so there are no sparks */
    if (kind == 1) {
        if (fx->imp_t > 0.1f) { fx->imp_t -= 0.1f; fx->imp_r = fx_rnd() * 50.0f; }
        static const float ic[3] = { 1, 0.4f, 0.4f };
        static const float in[4][3] = { { 0.7f, 0.7f, 0 }, { -0.7f, 0.7f, 0 }, { 0, 0.7f, 0.7f }, { 0, 0.7f, -0.7f } };
        for (int q = 0; q < 4; q++) hud_world_fx_plane(5, &b.x, in[q], 60.0f + fx->imp_r, ic, 0.2f + fx->imp_r * 0.01f);
    }
}
static uint32_t msvc_rand(void *user);
static Quat quat_from_axes(Vec3 X, Vec3 Y, Vec3 Z);

/* ---- exhaust smoke and explosion flashes -------------------------------------------------------------------------
 * Shared by the missiles (visual 0/1, docs/PROJECTILES.md 5.3-5.4) and the rideable rocket (class 20, docs/ROCKET.md 5.1):
 * both hang on the one exhaust list of 0x475440 and both end in flash records 0x4762e0, one per explosion radius. */
typedef struct { Vec3 pos; float t, rot; } Puff;              /* 0x475380: one smoke cloud, 0.2 s, image 14 */
typedef struct { Vec3 pos; float t, R; } Blast;               /* 0x4762e0: the nine flat quads of one flash, 0.3 s, image 12 */
static Puff g_puffs[96]; static int g_puff_next; static Blast g_blasts[16];
static void puff_add(Vec3 p) { Puff *q = &g_puffs[g_puff_next++ % 96]; q->pos = p; q->t = 1e-4f; q->rot = (float)msvc_rand(NULL) / 32767.0f; }
/* the particles of an explosion 0x477060 (docs/PARTICLES.md 5): kind 0 (bombs) = the dust burst 0x476cd0 and the smoke ring
 * 0x476140(pos, n, 0, 0.25, 1.5) next to its flash and ring (bombfx); kind 1 = the burning debris 0x476b50 and the dust burst
 * next to the two flashes 0x4762e0 (blast_add). Defined with the effect pool below. */
static void fx_explode(int kind, Vec3 pos, Vec3 n);
static void blast_add(Vec3 p, float R) { for (int i = 0; i < 16; i++) if (g_blasts[i].t <= 0) { g_blasts[i] = (Blast){ p, 1e-4f, R }; return; } }
/* 200 clouds a second, spread over the way the nozzle covered since the previous frame */
static void exhaust_smoke(Vec3 from, Vec3 to, float *acc, float dt)
{
    if (dt <= 0) return;
    *acc += dt * 200.0f; int n = (int)*acc; *acc -= n; if (n > 24) n = 24;
    for (int k = 0; k < n; k++) { float u = (k + 0.5f) / n;
        puff_add((Vec3){ from.x + (to.x - from.x) * u, from.y + (to.y - from.y) * u, from.z + (to.z - from.z) * u }); }
}
static void fx_smoke_draw(float dt)
{
    static const float white[3] = { 1, 1, 1 };
    static const float k_blast_n[9][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0.7f, 0, 0.7f }, { -0.7f, 0, 0.7f },
                                           { 0.7f, 0.7f, 0 }, { -0.7f, 0.7f, 0 }, { 0, 0.7f, 0.7f }, { 0, -0.7f, 0.7f } };   /* 0x4762e0 */
    for (int i = 0; i < 96; i++) if (g_puffs[i].t > 0) { Puff *p = &g_puffs[i]; float u = p->t / 0.2f;
        hud_world_fx(14, &p->pos.x, 15.0f * (1 + u), p->rot, white, 0.3f * (1 - u)); if ((p->t += dt) >= 0.2f) p->t = 0; }
    for (int i = 0; i < 16; i++) if (g_blasts[i].t > 0) { Blast *b = &g_blasts[i]; float u = b->t / 0.3f;
        float size = b->R * (0.3f + 0.7f * sinf(u * 1.5707963f)), a = 0.3f * cosf(u * 1.5707963f);
        for (int q = 0; q < 9; q++) hud_world_fx_plane(12, &b->pos.x, k_blast_n[q], size, white, a);
        if ((b->t += dt) >= 0.3f) b->t = 0; }
}

/* ---- missile models, type 41 (docs/PROJECTILES.md 5.4) -------------------------------------------------------------
 * The type-41 instances of a level are a pool of missile models. They wait hidden (0x472530); a projectile with visual
 * 0/1 takes one (0x4722f0), it then follows that projectile with its +Z axis along the flight direction (0x46d320) and
 * its typecode-9 markers trailing flame, glow and smoke (0x475440), and at the end of the flight it goes back to the
 * pool (0x472370). An empty pool means no model and nothing else: the trail, the head and the explosion do not care.
 * Port deviation: the original swaps the taken entry to the front of the pool; here a flag keeps the records in place,
 * because the projectile holds a pointer to one. */
#define MISSILE_MARKS 4                                       /* typecode-9 nozzles per model that the port follows */
typedef struct { Instance *inst; int taken, nmark; float f1, f2, f3, acc; Vec3 prev[MISSILE_MARKS]; int has_prev[MISSILE_MARKS]; } Missile;   /* the exhaust object at inst+0x100 (0x4723f0) */
static Missile g_missiles[50]; static int g_nmissiles;
static Missile *missile_of(const Instance *in) { for (int i = 0; i < g_nmissiles; i++) if (g_missiles[i].inst == in) return &g_missiles[i]; return NULL; }
static void missile_add(Instance *in)                                              /* SetTypeInstance 41, 0x403b5d */
{
    if (missile_of(in) || g_nmissiles >= 50) return;                               /* 0x5e8344[]: 50 at most, the original stops with an error */
    Missile *mi = &g_missiles[g_nmissiles++]; memset(mi, 0, sizeof *mi); mi->inst = in;
    Vec3 m, d; while (mi->nmark < MISSILE_MARKS && inst_vector_at(in, 9, (uint32_t)mi->nmark, &m, &d)) mi->nmark++;   /* 0x4723f0 counts them the same way */
    in->visible = 0;                                                               /* 0x472530: hidden until a projectile takes it */
}
static void missile_place(Missile *mi, Vec3 pos, Vec3 dir)                         /* 0x4724e0 with the frame of 0x46d320: rows = model axes in world space */
{
    Instance *in = mi->inst; Vec3 X = { 1, 0, 0 }, Y = { 0, 1, 0 }, Z = { 0, 0, 1 };   /* a direction of length 0 leaves the model unturned */
    if (fabsf(dir.x) < 0.001f && fabsf(dir.z) < 0.001f) {                          /* straight up or down (0x46d375) */
        Vec3 v = { 0, dir.z, -dir.y }; float l = sqrtf(v.y * v.y + v.z * v.z);
        if (l > 1e-6f) { v.y /= l; v.z /= l; Z = dir; Y = v;
                         X = (Vec3){ v.y * dir.z - v.z * dir.y, v.z * dir.x - v.x * dir.z, v.x * dir.y - v.y * dir.x }; }
    } else {                                                                       /* 0x46d417 */
        Vec3 a = { dir.z, 0, -dir.x }; float l = sqrtf(a.x * a.x + a.z * a.z);
        if (l > 1e-6f) { a.x /= l; a.z /= l; Z = dir; X = a;
                         Y = (Vec3){ dir.y * a.z - dir.z * a.y, dir.z * a.x - dir.x * a.z, dir.x * a.y - dir.y * a.x }; }
    }
    in->position = pos; in->quat = quat_from_axes(X, Y, Z);
    mat4_from_trs(&in->world, in->position, in->quat, in->scale);
}
static Missile *missile_take(Vec3 pos, Vec3 dir)                                   /* 0x4722f0 */
{
    for (int i = 0; i < g_nmissiles; i++) {
        Missile *mi = &g_missiles[i]; if (mi->taken) continue;
        mi->taken = 1; mi->f1 = mi->f2 = mi->f3 = mi->acc = 0;
        for (int k = 0; k < MISSILE_MARKS; k++) mi->has_prev[k] = 0;               /* +0x2c[i] = 0: the trail starts here */
        mi->inst->visible = 1; mi->inst->noncollide = 1;                           /* 0x4077f0 puts it back in the cells; a missile model has no hull nodes, so nothing can walk into it */
        missile_place(mi, pos, dir);
        if (wenv("WOODY_FXLOG")) printf("missile %u (model %d, %d nozzle%s) takes off at %.0f %.0f %.0f", mi->inst->index, (int)(mi->inst->model - g_ins.models), mi->nmark, mi->nmark == 1 ? "" : "s", pos.x, pos.y, pos.z), puts("");
        return mi;
    }
    if (wenv("WOODY_FXLOG")) printf("missile pool (%d) empty: this shot flies without a model", g_nmissiles), puts("");
    return NULL;                                                                   /* the rest of the visual runs anyway */
}
static void missile_release(Missile *mi) { if (mi && mi->taken) { mi->taken = 0; mi->inst->visible = 0; } }   /* 0x472370 -> 0x407850 */
/* exhaust 0x475440 per nozzle, size table 0x4abcc8 index 3: flame 45, glows 40 / 35 (the rocket is index 6). The flame
 * is three quads crossed on the nozzle axis (0x4759cd): frame M = 0x46d320(d), quad k spanned by d and
 * M.row0 cos a + M.row1 sin a with a = f3 * 512 + 85 k (1/512 turns), sprite mode 0x13 (a 2:1 quad along d), image 31,
 * alpha 0.8, UV set 2, flags 0x62, centred d * size (cos(base) - 1/64) behind the nozzle point. State 2 (on) from the
 * moment the projectile takes the model, so there is no start-up ramp; the smoke comes out over the way the nozzle
 * covered, or the trail would be a string of dots. */
static void missile_exhaust(Missile *mi, float dt)
{
    static const float white[3] = { 1, 1, 1 };
    mi->f1 += dt * 0.05f; mi->f2 += dt * 0.15f; mi->f3 += dt * 3.0f;
    for (int k = 0; k < mi->nmark; k++) {
        Vec3 m, d; if (!inst_vector_at(mi->inst, 9, (uint32_t)k, &m, &d)) break;
        float l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z); if (l > 1e-4f) { d.x /= l; d.y /= l; d.z /= l; }
        float fl = 45.0f + (float)msvc_rand(NULL) / 32767.0f * 10.0f - 5.0f, off = fl * (0.8944272f - 0.015625f);   /* mode 0x13: cos(atan 0.5); 0x4abcf8 = 1/64 */
        float fp[3] = { m.x + d.x * off, m.y + d.y * off, m.z + d.z * off };
        hud_world_fx(32, &m.x, 40.0f, 1.0f - mi->f1, white, 1.0f - 0.2f * sinf(3.14159265f * mi->f1));
        hud_world_fx(32, &m.x, 35.0f, 1.0f - mi->f2, white, 1.0f - 0.2f * sinf(3.14159265f * mi->f2));
        Vec3 R0 = { 1, 0, 0 }, R1 = { 0, 1, 0 };                                    /* rows 0 / 1 of 0x46d320(d); row 2 = d */
        if (fabsf(d.x) < 0.001f && fabsf(d.z) < 0.001f) { float vl = sqrtf(d.z * d.z + d.y * d.y); if (vl > 1e-6f) { R1 = (Vec3){ 0, d.z / vl, -d.y / vl }; R0 = (Vec3){ R1.y * d.z - R1.z * d.y, R1.z * d.x - R1.x * d.z, R1.x * d.y - R1.y * d.x }; } }
        else { float al = sqrtf(d.z * d.z + d.x * d.x); R0 = (Vec3){ d.z / al, 0, -d.x / al }; R1 = (Vec3){ d.y * R0.z - d.z * R0.y, d.z * R0.x - d.x * R0.z, d.x * R0.y - d.y * R0.x }; }
        for (int q = 0; q < 255; q += 85) {
            int a = ((int)(mi->f3 * 512.0f + (float)q)) & 511; float t = 6.2831853f * a / 512.0f, c = cosf(t), sn = sinf(t);
            float basis[6] = { d.x, d.y, d.z, R0.x * c + R1.x * sn, R0.y * c + R1.y * sn, R0.z * c + R1.z * sn };
            hud_world_spr_mode(0x13, 31, fp, fl, 0, white, 0.8f, 0x62, basis, 2);
        }
        if (mi->has_prev[k]) exhaust_smoke(mi->prev[k], m, &mi->acc, dt);
        mi->prev[k] = m; mi->has_prev[k] = 1;
    }
}

/* ---- class 40, the bomb (docs/BOMB.md) ------------------------------------------------------------------------------
 * A bomb is an ordinary instance from a pool of at most 16 per level (0x5e4880; the level's own type-40 instances, which
 * stay visible where the .ins parked them until one is used). While it is out, a projectile of template 0 carries it
 * (PROJECTILES.md 2.4): gravity 15 * 200, bouncing without end off the terrain, 5 frames within 1 unit of a floor = lying
 * (then it rolls out with 0.95 per 1/60 s). The fuse is the template's life; 2 frames after it runs out the bomb blows up:
 * radius 400 on the player (Kill(6)), every enemy and every chest 120/121. It never goes off on contact with Woody.
 * Users: the bomb thrower (launcher kind 0), the bomb cannon (class 21), the dispenser (message 1090) and the player's
 * throw (Perso state 6, docs/BOMB_CARRY.md). The projectile lives inside the Bomb here: nothing else reads it. */
typedef struct { float radius, gravity, speed, damp_g, damp_a, life, damage; } BombT;
static const BombT BOMB_T0 = { 30, 15, 1500, 0.95f, 0.99f, 2.0f, 1000 };             /* template 0 (0x448c70) */
typedef struct Bomb {
    Instance *inst; int state;                     /* +0x108: 0 free, 1 started, 2/3 fuse burning, 4/5 igniting, 6 exploded */
    float fuse, warn, t, blink_acc; int blink_n;   /* +0x10c +0x110 +0x114 +0x118 +0x11c */
    Instance *launcher; int kind; int32_t var;     /* +0x120 (the launcher's instance here), +0x128 explosion kind, +0x12c script variable (-1 none) */
    int in_use, held, ridden;                      /* +0x131 +0x132 +0x133 */
    BombT T; int p_active, press, grounded, bounced; Vec3 p, vel, n;   /* the carrying projectile: point, velocity, plane of the last bounce */
    int puffed;                                    /* the fuse record 0x478e40: the muzzle smoke of a launcher's bomb, once */
    Enemy *owner_e; int owner_pl;                  /* T.owner (+0x58): the bomb thrower (type 12) or the Perso; neither = a launcher (b->launcher) or nobody */
    Instance *target; float age; Vec3 dir0;        /* T.target (+0x38: the previous owner after a throw), projectile age, T.dir0 */
    uint32_t col_cur;                              /* the projectile's press probe P+0 (+0x20): its world_collision, 0xffffffff none */
} Bomb;
static Bomb g_bombs[16]; static int g_nbombs;
typedef struct { Vec3 pos, n; float t; int kind; } BombFx;                 /* explosion kind 0 (0x4765f0 + 0x476710) and the muzzle smoke (0x478aa0) */
static BombFx g_bombfx[32];
static void bombfx_add(Vec3 p, Vec3 n, int kind) { for (int i = 0; i < 32; i++) if (g_bombfx[i].t <= 0) { g_bombfx[i] = (BombFx){ p, n, 1e-4f, kind }; return; } }
static Bomb *bomb_of(const Instance *in) { for (int i = 0; i < g_nbombs; i++) if (g_bombs[i].inst == in) return &g_bombs[i]; return NULL; }
static void bomb_place(Bomb *b) { Instance *in = b->inst; mat4_from_trs(&in->world, in->position, in->quat, in->scale); ins_pose(in, in->anim, in->anim_time); }
static void bomb_reset(Bomb *b)                                                    /* vtbl[17] 0x44d320: out of the cells, back in the pool */
{
    if (wenv("WOODY_BOMBLOG") && b->state != 6) printf("  BOMB %u reset in state %d", b->inst->index, b->state), puts("");
    b->p_active = 0; b->state = 0; b->in_use = b->held = b->ridden = 0; b->inst->visible = 0; b->inst->noncollide = 0; b->inst->tint_mode = 0;
}
static void bombs_discard_all(void)                                                /* 0x44db10 from the Perso reset: no explosion, the variables say "done" */
{
    for (int i = 0; i < g_nbombs; i++) if (g_bombs[i].in_use) { if (g_bombs[i].var >= 0) game_var_set((uint32_t)g_bombs[i].var, 1); bomb_reset(&g_bombs[i]); }
}
/* 0x44d5d0 + Launch 0x44d4d0: the first free bomb of the pool, on `pos`, moving along `dir` (normalised by the caller) */
static Bomb *bomb_start(const BombT *T, Vec3 pos, Vec3 dir, int ground, int32_t var, int kind)
{
    Bomb *b = NULL; for (int i = 0; i < g_nbombs && !b; i++) if (!g_bombs[i].in_use) b = &g_bombs[i];
    if (!b) { puts("  BOMB: Pas de bombes, ou plus assez de bombes dans ce niveau..."); if (var >= 0) game_var_set((uint32_t)var, 1); return NULL; }
    b->T = *T; b->fuse = T->life; b->warn = T->life < 2.0f ? T->life : 2.0f;       /* [0x5e48c4] = 2.0 */
    /* 0x44d50e: the bomb instance goes to T.pos, with `ground` (only the 1090 dispenser) onto the floor below it + 1.0 (0x435650);
     * the projectile (0x4490a0 with the caller's T) still starts at T.pos itself, so the dispenser's bomb falls out of the
     * marker in the air, and the next projectile step puts the bomb back on its point + (0, 1, 0) */
    Vec3 ip = pos;
    if (ground && g_player) { int f; float gy = gel_floor_below(g_player->gel, pos, 0, 1e5f, &f); if (f) ip.y = gy + 1.0f; }
    b->p = pos; b->vel = (Vec3){ dir.x * T->speed, dir.y * T->speed, dir.z * T->speed };
    b->inst->position = ip; b->inst->visible = 1; b->inst->fade = b->inst->fade_target = 0;
    b->var = var; b->kind = kind; b->launcher = NULL; b->in_use = 1; b->held = b->ridden = 0; b->owner_e = NULL; b->owner_pl = 0; b->target = NULL; b->age = 0; b->dir0 = dir;
    b->t = 0; b->state = 1; b->blink_acc = 0; b->blink_n = 0; b->puffed = 0;
    b->p_active = 1; b->press = b->grounded = b->bounced = 0; b->n = (Vec3){ 0, 1, 0 };
    b->col_cur = 0xffffffffu;                                                      /* a new projectile 0x449130: probe ctor 0x436cf0 (a throw 0x4492d0 keeps it) */
    bomb_place(b);
    if (wenv("WOODY_BOMBLOG")) printf("  BOMB %u start at %.1f %.1f %.1f (instance y %.1f) dir %.4f %.4f %.4f speed %.0f fuse %.2f kind %d var %d", b->inst->index, pos.x, pos.y, pos.z, ip.y, dir.x, dir.y, dir.z, T->speed, b->fuse, kind, var), puts("");
    return b;
}
/* 0x44d3a0 + 0x4492d0: start the carrying projectile again from where the bomb is (the throw / the drop from Woody's hands) */
void game_bomb_launch(struct Bomb *b, Vec3 dir, float speed)
{
    if (!b || !b->p_active) return;
    b->held = 0; b->p = b->inst->position; b->vel = (Vec3){ dir.x * speed, dir.y * speed, dir.z * speed }; b->press = b->grounded = 0;
    b->target = b->owner_e ? b->owner_e->inst : b->launcher; b->owner_e = NULL; b->owner_pl = 1; b->dir0 = dir;   /* 0x463894: T.target = the old owner, T.owner = the Perso */
    /* the age P+0xd4 runs on: Reinit 0x4492d0 copies T, pos, dir, speed and clears +0xe8/+0xec/+0x100 only, so a bomb that was
     * in flight and then carried (pick-up 1.2 s + release 0.47 s) is past the 2 s of steering (T+0x48) when thrown and does
     * not turn back to its launcher / thrower */
}
/* Fire 0x411e80 of the bomb thrower (type 12, docs/ENEMY2.md 4.3): template 0 with his speed and fuse, owner = him, no target */
int game_enemy_bomb(Enemy *e, Vec3 pos, Vec3 dir, float speed, float fuse)
{
    BombT t = BOMB_T0; t.speed = speed; t.life = fuse;
    Bomb *b = bomb_start(&t, pos, dir, 0, -1, 0); if (!b) return 0;
    b->owner_e = e; return 1;
}
/* 0x463430: the pick-up test does not look at the bomb's state; the port leaves out the invisible 0.5 s after a blast */
struct Bomb *game_bomb_pick(Vec3 pos, float r)
{
    for (int i = 0; i < g_nbombs; i++) { Bomb *b = &g_bombs[i]; if (!b->in_use || b->ridden || b->state == 6) continue;
        Vec3 d = { b->inst->position.x - pos.x, b->inst->position.y - pos.y, b->inst->position.z - pos.z };
        if (d.x * d.x + d.y * d.y + d.z * d.z < r * r) { b->held = 1; if (wenv("WOODY_BOMBLOG")) printf("  BOMB %u picked up", b->inst->index), puts(""); return b; } }
    return NULL;
}
void game_bomb_hold(struct Bomb *b, Vec3 pos, Quat q) { b->inst->position = pos; b->p = pos; b->inst->quat = q; bomb_place(b); }   /* 0x46371c..0x463794 */
static void chests_blast(Vec3 c, float r);
/* 0x44d6e0 */
static void bomb_explode(Bomb *b)
{
    if (b->state == 6) return; b->state = 6;
    if (b->var >= 0) game_var_set((uint32_t)b->var, 1);
    Vec3 c = b->inst->position; Player *pl = g_player;                              /* 0x44d650: radius 400, 3D, through walls */
    if (pl && !pl->dead_kind && pl->inst && !wenv("WOODY_GOD")) {                /* Perso vtbl[40] 0x44d040 */
        Vec3 d = { pl->inst->position.x - c.x, pl->inst->position.y - c.y, pl->inst->position.z - c.z };
        if (d.x * d.x + d.y * d.y + d.z * d.z < 400.0f * 400.0f && pl->invuln_respawn <= 0) { float l = sqrtf(d.x * d.x + d.z * d.z);
            player_hit(pl, 0, l > 1e-3f ? (Vec3){ d.x / l, 0, d.z / l } : (Vec3){ 0, 0, 1 }); pad_rumble(1.0f, 0.5f); player_kill(pl, 6);   /* rumble 0x44d1b0(+0x1d4, +0x1d0) = P+0xc4, P+0xc0 */
            printf("  BOMB %u blast kills the player", b->inst->index), puts(""); }
    }
    enemies_blast(&g_enemies, c, 400.0f);
    chests_blast(c, 400.0f);
    audio_fx(6, b->inst, &b->inst->position.x);                                    /* the original only while +0x124 exists: the port keeps the projectile in the bomb */
    Vec3 e = { b->p.x, b->p.y + 20.0f, b->p.z };
    if (b->kind == 1) game_explosion(e);
    else { Vec3 bn = b->bounced ? b->n : (Vec3){ 0, 1, 0 }; bombfx_add(e, bn, 0); fx_explode(0, e, bn); }   /* 0x477060(kind, pos, plane of the last bounce) */
    b->p_active = 0; b->t = 0; b->inst->fade = b->inst->fade_target = 1.0f;       /* not drawn from now on (> 0.98) */
    if (wenv("WOODY_BOMBLOG")) printf("  BOMB %u explodes at %.0f %.0f %.0f", b->inst->index, c.x, c.y, c.z), puts("");
}
/* render colour vtbl[26] 0x44d9a0: black, with 0.1 s flashes of +128 red that come faster as the fuse runs out, every
 * other frame in the last 2 s and red for good once it is spent */
static void bomb_colour(Bomb *b, float dt)
{
    Instance *in = b->inst; float rem = b->fuse - b->t, A, B = 0.1f; int lit;
    if (dt <= 0) { in->tint_mode = 1; in->tint_rgb[0] = in->tint_rgb[1] = in->tint_rgb[2] = 0; return; }
    if (rem >= 4.0f) A = 0.5f; else if (rem >= 3.0f) A = 0.2f; else if (rem >= 2.0f) A = 0.15f; else if (rem >= 0) A = B = 0; else A = 0.08f;
    if (A + B < dt) { lit = ++b->blink_n == 2; if (lit) b->blink_n = 0; }
    else { b->blink_acc += dt; if (b->blink_acc >= A) { b->blink_acc -= A; lit = 1; } else lit = b->blink_acc <= B; }
    if (lit) { in->tint_mode = 2; in->tint_rgb[0] = 128.0f / 255.0f; in->tint_rgb[1] = in->tint_rgb[2] = 0; }
    else { in->tint_mode = 1; in->tint_rgb[0] = in->tint_rgb[1] = in->tint_rgb[2] = 0; }
}
/* 0x44d820 -> 0x44d850 for every bomb in use: frame step 18, after the Perso, before the VM tick */
static void bombs_update(float dt)
{
    for (int i = 0; i < g_nbombs; i++) {
        Bomb *b = &g_bombs[i]; if (!b->in_use) continue;
        b->t += dt; b->inst->noncollide = b->held || b->ridden;
        int gone = !b->p_active;                                                   /* CheckProj 0x44d800: the projectile died (a hull) -> ignite */
        switch (b->state) {
        case 1: b->state = 2; break;                                               /* 0x478e40: the fuse record starts */
        case 2: if (gone) b->state = 4; if (b->fuse - b->warn <= b->t) b->state = 3; break;
        case 3: if (gone) b->state = 4; if (b->t >= b->fuse) b->state = 4; break;
        case 4: b->state = 5; break;
        case 5: bomb_explode(b); break;
        case 6: if (b->t >= 0.5f) { b->inst->fade = b->inst->fade_target = 0; bomb_reset(b); } break;
        }
    }
}
/* the carrying projectile, 0x4493c0 with T.carried set: frame step 32, after the VM tick. The ray 0x4359b0 (Move 0x449cc0):
 * terrain (hit kind 1) and the press nodes of instances (2) are a bounce with no loss. Hit kind 3 (0x4330c0: the segment STARTS
 * inside a press node of another instance, e.g. one that moved onto the bomb) kills the projectile and sets the bomb off 2 frames
 * later (CheckProj). A floor within 1 unit for 5 frames running is "lying". Moving platforms do not carry a lying bomb: the
 * wall probe 0x437040 of Move is dead code (its sphere query 0x435b60 is a stub) and nothing reads the press probe's platform
 * delta 0x436d20 for a projectile (docs/BOMB.md 5.2). */
static float vdot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static void bombs_fly(float dt, const GelFile *gel)
{
    for (int i = 0; i < g_nbombs; i++) {
        Bomb *b = &g_bombs[i]; if (!b->in_use || !b->p_active || dt <= 0) continue;
        b->age += dt;                                                              /* 0x4493e0: P+0xd4 runs from Init 0x449130 on, also while Woody holds it */
        if (b->held) continue;                                                     /* 0x449440 */
        float damp;
        if (!b->grounded) { b->vel.y -= dt * b->T.gravity * 200.0f; if (b->vel.y < -800.0f) b->vel.y = -800.0f; damp = b->T.damp_a; }
        else { b->vel.y = 0; damp = b->T.damp_g; }
        float k = powf(damp, dt * 60.0f); b->vel.x *= k; b->vel.y *= k; b->vel.z *= k;
        if (b->target && b->age < 2.0f) {                                          /* 0x4494df, template 0: xz steer 0.025 per 1/60 s for 2 s at target + 125, limits 0.7 */
            Vec3 old = b->vel, tg = { b->target->position.x, b->target->position.y + 125.0f, b->target->position.z };
            float s = sqrtf(vdot3(b->vel, b->vel)), dx = tg.x - b->p.x, dz = tg.z - b->p.z, dl = sqrtf(dx * dx + dz * dz);
            if (s > 1e-3f && dl > 1e-3f) {
                float kk = powf(1.0f - 0.025f, dt * 60.0f); Vec3 v = { b->vel.x / s, b->vel.y / s, b->vel.z / s };
                v.x = dx / dl * (1 - kk) + v.x * kk; v.z = dz / dl * (1 - kk) + v.z * kk;
                float vl = sqrtf(vdot3(v, v)); b->vel = (Vec3){ v.x / vl * s, v.y / vl * s, v.z / vl * s };
                if (1.0f + b->vel.y * b->dir0.y < 0.7f) { b->vel.y = old.y; float l2 = sqrtf(vdot3(b->vel, b->vel)); if (l2 > 1e-6f) b->vel = (Vec3){ b->vel.x / l2 * s, b->vel.y / l2 * s, b->vel.z / l2 * s }; }   /* 0x4498ec, literally */
                if (b->vel.x * b->dir0.x + b->vel.z * b->dir0.z < 0.7f) { b->vel.x = old.x; b->vel.z = old.z; float l2 = sqrtf(vdot3(b->vel, b->vel)); if (l2 > 1e-6f) b->vel = (Vec3){ b->vel.x / l2 * s, b->vel.y / l2 * s, b->vel.z / l2 * s }; }
            }
        }
        Vec3 a = b->p, e = { a.x + b->vel.x * dt, a.y + b->vel.y * dt, a.z + b->vel.z * dt }, n, ni; float fi = 2.0f;
        {   /* HitActors 0x44a0a0 before the ray: template 0 only tests category 2 subtype 8 / 12 = the bomb thrower (type 12); sphere 2 * 30 */
            Enemy *hit = enemies_bomb_contact(&g_enemies, b->owner_e, a, e, 2.0f * b->T.radius);
            if (hit) {
                if (!b->owner_e && !b->owner_pl) enemy_hit(hit, b->T.damage, (Vec3){ 0, 0, 0 }, e, 0);   /* a launcher's or nobody's bomb: vtbl[39](0, 1000) = his "pecked" stun; owner Perso / enemy: carried, no damage */
                b->p = e; b->inst->position = (Vec3){ e.x, e.y + 1.0f, e.z }; bomb_explode(b);
                if (wenv("WOODY_BOMBLOG")) printf("  BOMB %u hits the thrower %u", b->inst->index, hit->inst->index), puts("");
                continue;
            }
        }
        { const Instance *hi = NULL;                                               /* hit kind 3: the start inside a press node (not its own) -> projectile gone -> CheckProj ignites */
          if (inst_point_in_press(gel, &g_ins,b->inst, a, &hi)) { b->p_active = 0; if (wenv("WOODY_BOMBLOG")) printf("  BOMB %u inside instance %u (hit kind 3): projectile gone", b->inst->index, hi->index), puts(""); continue; } }
        float f = gel_ray_hit(gel, a, e, &n);
        if (g_player && player_ray_instances(g_player, b->inst, a, e, &fi, &ni, NULL) && fi < f) { f = fi; n = ni; }   /* a press node: the same bounce */
        if (f <= 1.0f) {                                                           /* Bounce 0x449eb0: mirror the end point in the plane, keep the speed */
            Vec3 d = { e.x - a.x, e.y - a.y, e.z - a.z }; float L = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z), s = L * f - 0.01f; if (s < 0) s = 0;
            Vec3 h = L > 1e-6f ? (Vec3){ a.x + d.x / L * s, a.y + d.y / L * s, a.z + d.z / L * s } : a;
            /* 0x449f6f: e is mirrored in the hit PLANE (n.e + D, D through the exact hit a + d f), not in a plane through h, which sits
             * 0.01 short of it: the new direction r - h is a touch flatter than the mirrored d (live trace, S2A dispenser) */
            float q = 2.0f * ((e.x - (a.x + d.x * f)) * n.x + (e.y - (a.y + d.y * f)) * n.y + (e.z - (a.z + d.z * f)) * n.z); Vec3 r = { e.x - q * n.x, e.y - q * n.y, e.z - q * n.z };
            Vec3 u = { r.x - h.x, r.y - h.y, r.z - h.z }; float ul = sqrtf(u.x * u.x + u.y * u.y + u.z * u.z), sp = sqrtf(b->vel.x * b->vel.x + b->vel.y * b->vel.y + b->vel.z * b->vel.z);
            if (ul > 1e-6f) b->vel = (Vec3){ u.x / ul * sp, u.y / ul * sp, u.z / ul * sp };
            b->p = h; b->bounced = 1; b->n = n;
        } else b->p = e;
        b->inst->position = (Vec3){ b->p.x, b->p.y + 1.0f, b->p.z };
        if (gel_cell(gel, b->inst->position) < 0) { bomb_explode(b); continue; }  /* 0x449bb7: out of the world */
        int found = 0; uint32_t col = 0xffffffffu; float gy = g_player ? player_ground_query_col(g_player, b->inst, (Vec3){ b->p.x, b->p.y + b->T.radius, b->p.z }, &found, &col) : 0;   /* probe 0x436dc0 (0x449c04) */
        game_col_probe(&b->col_cur, found && b->p.y - gy < 1.0f, col, b->inst);   /* Press / In / UnPress with the bomb's id */
        if (found && b->p.y - gy < 1.0f) { if (b->press == 0) audio_fx(12, b->inst, &b->inst->position.x); b->press++; } else b->press = 0;
        b->grounded = b->press >= 5; if (b->grounded) { b->vel.y = 0; b->p.y = gy; }
        b->inst->position = (Vec3){ b->p.x, b->p.y + 1.0f, b->p.z };
        bomb_place(b);
        if (wenv("WOODY_BOMBLOG") && (wenv("WOODY_BOMBLOG")[0] == '3' || (wenv("WOODY_BOMBLOG")[0] == '2' && (int)(b->t * 8) != (int)((b->t - dt) * 8)))) printf("  BOMB %u t %.3f state %d at %.1f %.1f %.1f vel %.1f %.1f %.1f ground %d", b->inst->index, b->t, b->state, b->p.x, b->p.y, b->p.z, b->vel.x, b->vel.y, b->vel.z, b->grounded), puts("");
    }
}
/* per frame for every bomb in use: its colour, and in fuse states 2/3 the fuse (0x478b70): a line along the bomb's own marker
 * that burns from its tip towards the bomb, a spark (bank 0 image 18) on the burning end, and once the muzzle smoke of the
 * launcher that fired it. Explosion kind 0: a yellow flash (image 4, 500 sin^3; 0x4766b3, sprite flags 3 = additive) and a smoke
 * ring on the ground (image 24, 1300 u in 0.3 s; 0x4767d3, flags 0xa = alpha blended, texture x 2c); the muzzle puff (image 24,
 * 0x478b58) has flags 0xb, alpha blended too. */
static void bombs_draw(const float *eye, float dt)
{
    static const float yellow[3] = { 1, 1, 0 }, white[3] = { 1, 1, 1 }, grey[3] = { 0.8f, 0.8f, 0.8f }, g0[3] = { 0.5f, 0.5f, 0.5f };
    for (int i = 0; i < g_nbombs; i++) {
        Bomb *b = &g_bombs[i]; if (!b->in_use) continue;
        bomb_colour(b, dt);
        if (b->state != 2 && b->state != 3) continue;
        if (!b->puffed && b->launcher) { b->puffed = 1; Vec3 m0, md; if (inst_vector(b->launcher, 0, &m0, &md)) { float l = sqrtf(md.x * md.x + md.y * md.y + md.z * md.z);
            if (l > 1e-4f) { Vec3 mz = { m0.x + md.x / l * 10.0f, m0.y + md.y / l * 10.0f, m0.z + md.z / l * 10.0f };
                             game_smoke_ring(mz, (Vec3){ md.x / l, md.y / l, md.z / l }, 0, 0.25f, 1.5f); bombfx_add(mz, (Vec3){ 0, 1, 0 }, 1); } } }   /* 0x478c0e: the smoke ring round the muzzle, then 0x478aa0 */
        Vec3 v0, dv; if (!inst_vector(b->inst, 0, &v0, &dv)) continue;
        float f = b->fuse > 0 ? b->t / b->fuse : 1; if (f > 1) f = 1;
        float tip[3] = { v0.x + dv.x * (1 - f), v0.y + dv.y * (1 - f), v0.z + dv.z * (1 - f) };
        hud_world_line(&v0.x, tip, eye, 1.0f, g0, 1.0f, 0.4f);
        hud_world_fx(18, tip, 5.0f + (float)msvc_rand(NULL) / 32767.0f * 10.0f, (float)msvc_rand(NULL) / 32767.0f, white, 0.7f);
    }
    for (int i = 0; i < 32; i++) {
        BombFx *x = &g_bombfx[i]; if (x->t <= 0) continue;
        if (x->kind == 0) {
            float u = x->t / 0.25f, s = sinf(6.2831853f * (u < 1 ? u : 1)); s = fabsf(s * s * s);
            if (u < 1) hud_world_fx(4, &x->pos.x, 500.0f * s, 0, yellow, 1.0f);
            if (u < 1) { float sn = sinf(6.2831853f * u); static const float wh[3] = { 255, 255, 255 }; rnd_light_add(0, x->pos, wh, 500.0f * sn * sn * sn + 100.0f); }   /* 0x4766ee: radius = signed size + 100 (docs/LIGHTING.md 7) */
            float w = x->t / 0.3f; if (w < 1) hud_world_fx_plane(24, &x->pos.x, &x->n.x, 1300.0f * w, grey, cosf(w * 1.5707963f));
            if ((x->t += dt) >= 0.3f) x->t = 0;
        } else {
            float u = x->t / 0.15f; hud_world_fx(24, &x->pos.x, 80.0f, 0, white, 0.7f * cosf(u * 1.5707963f));
            if ((x->t += dt) >= 0.15f) x->t = 0;
        }
    }
}
/* ---- chests, types 120/121 ("Exploding objects (as Chest)", docs/BOMB_CARRY.md 3): a bomb blast within 400 of the origin
 * plays animation 0 once (speed 3), explosion kind 1, fades out at 0.5/s (no collision past 0.9) and sets msgmask 0x20 */
static Instance *g_chests[32]; static int g_nchests;
static EkoVM *g_vm;
static void chest_reset(Instance *in)                                              /* 0x451730, message 29 */
{
    in->slot[0] = 0; in->slot[1] = in->slot[2] = in->slot[3] = -1; in->a_speed = in->a_base_speed = 0; in->a_pos = 0; in->anim = 0; in->anim_time = 0;
    in->visible = 1; in->fade_rate = 100.0f; in->fade_target = 0; if (g_vm) eko_msgmask_clear(g_vm, in->id, 0x20);
}
static void chests_blast(Vec3 c, float r)                                          /* vtbl[28] 0x451770 / vtbl[29] 0x4517d0 */
{
    for (int i = 0; i < g_nchests; i++) {
        Instance *in = g_chests[i]; if (g_vm && eko_msgmask_test(g_vm, in->id, 0x20)) continue;
        Vec3 d = { in->position.x - c.x, in->position.y - c.y, in->position.z - c.z }; if (d.x * d.x + d.y * d.y + d.z * d.z >= r * r) continue;
        inst_play_once(in, 0, 3.0f, g_now);                                        /* 0x436ca0(1.0): anim 0 once at speed 1.0 * 3 (W2A 537: 4.8 s / 3 = 1.6 s) */
        game_explosion(in->position);
        in->fade_rate = 0.5f; in->fade_target = 1.0f;
        if (g_vm) eko_msgmask_set(g_vm, in->id, 0x20);
        printf("  CHEST %u blown open", in->index), puts("");
    }
}

/* ---- launcher type 42 + projectiles (docs/PROJECTILES.md). A projectile is a point with a velocity made from a 0x68-byte
 * template (four of them, 0x448c70): gravity, damping, seeking of a target instance (xz and vertical, with the two clamps),
 * bouncing off world polygons and press nodes up to max_bounce, the hit on the Perso's cylinder (and on the bomb thrower /
 * Boss2 of actor list 1), lifetime. Visual 2 (the energy bolt every level script uses), visual 0/1 (the missile of the
 * shooting enemies, section 5.3) and the fireball of visual 3 (section 5.5); launcher kind 0 throws a class-40 bomb on its
 * template instead (docs/BOMB.md 6). */
typedef struct { float radius, gravity, speed, damp_g, damp_a, life, damage; int max_bounce; float aim_h, steer, vsteer, t_xz, t_y, lim_xz, lim_y; int visual, hits_all; } ProjT;   /* T+0x18..+0x64 */
static const ProjT PROJ_T[4] = {                                                   /* 0x448c70 -> 0x5d7ba8 + kind * 0x68 */
    { 30, 15, 1500, 0.95f, 0.99f,  2, 1000, -1, 125, 0.025f,   0,  2,  0, 0.7f, 0.7f, 4, 0 },   /* 0: thrown (the bomb) */
    {  5,  0, 1000, 1, 1, 15, 1, 0, 125, 0.025f, 2.5f, 15, 15, 0,  0, 2, 1 },                  /* 1: the launcher default, the enemies' base */
    {  5,  0, 1000, 1, 1, 15, 1, 0, 150, 0.5f,   100, 15, 15, 0, -1, 2, 1 },                   /* 2: aggressive seeking (races) */
    {  5,  0, 1000, 1, 1, 15, 1, 0,  25, 0.5f,   100, 15, 15, 0, -1, 2, 1 } };                 /* 3: the same aimed at the feet */
typedef struct { Instance *inst, *target; int active, count, aim, kind, anim; float T, t0, last, anim_dur; ProjT t; } Launcher;   /* t = L+0x108 (1001 / 1002); anim/anim_dur: 1002 [7] / [8] (+0x17c / +0x180) */
#define SHOT_HIST 24
typedef struct { int active, visual; const Instance *owner, *target; ProjT T; Vec3 pos, dir, dir0, origin, vel; float age, life, dying, speed, damage; int bounces; Enemy *enemy; Missile *missile; float fb_acc; Vec3 fb_prev;
                 Vec3 hist[SHOT_HIST]; float hist_t[SHOT_HIST], hist_acc; int nhist; } Shot;   /* hist: the ribbon points of 0x47d090 (position and age), a ring */
typedef struct { Vec3 pos; float t; int kind; } Flash;                             /* kind 0 = the flash of visual 2 (0x46f180), 1 = the muzzle flash of the missile (0x46fa40), 2 = the launch glow of the fireball (0x4702b0, 1 s) */
typedef struct { Vec3 pos; float t, rot; } Spark;                                  /* 0x470370: one spark of the fireball's trail, 0.4 s, image 13 */
static Spark g_sparks[256]; static int g_spark_next;
#define MAX_LAUNCHERS 256                                                          /* instances, no pool in the original: W3C has 119, W3A/W3D 44, K3A 34 */
static Launcher g_launchers[MAX_LAUNCHERS]; static int g_nlaunchers;
static Shot g_shots[200]; static Flash g_flashes[64];
static Launcher *launcher_of(const Instance *in) { for (int i = 0; i < g_nlaunchers; i++) if (g_launchers[i].inst == in) return &g_launchers[i]; return NULL; }
static void flash_add(Vec3 p, int kind) { for (int i = 0; i < 64; i++) if (g_flashes[i].t <= 0) { g_flashes[i].pos = p; g_flashes[i].t = 1e-4f; g_flashes[i].kind = kind; return; } }
static int shot_is_missile(const Shot *s) { return s->visual == 0 || s->visual == 1; }
/* how long the trail keeps shrinking after the projectile is gone: seconds per segment / (3 * that per second),
 * 0.04 / 0.3 for the bolt (0x46f8a0) and 0.025 / 0.15 for the missile (0x46fb30); the fireball's record lives on
 * 0.03125 / 0.09375 s but draws nothing in that time (its ribbon is never submitted, docs/PROJECTILES.md 5.5) */
static float shot_fade_len(const Shot *s) { return shot_is_missile(s) ? 0.16667f : s->visual == 3 ? 0.33333f : 0.13333f; }
/* the ribbon of the bolt / missile (pools 0x5e82c8 / 0x5e82e8): N = 11 / 21 points, one pushed every span / ((N - 1) * speed) s
 * (fx+0x3c), drawn as N - 1 segments of span / (speed * (N - 1)) s each (fx+0x10) back along the path actually flown */
static float shot_span(const Shot *s) { return shot_is_missile(s) ? 500.0f : 400.0f; }
static int shot_nseg(const Shot *s) { return shot_is_missile(s) ? 20 : 10; }
static void shot_hist_push(Shot *s) { int k = s->nhist % SHOT_HIST; s->hist[k] = s->pos; s->hist_t[k] = s->age; s->nhist++; }
static Vec3 shot_hist_at(const Shot *s, float tq)                                  /* 0x46d0d0: the ribbon at age tq, between its points; the head is s->pos */
{
    int n = s->nhist < SHOT_HIST ? s->nhist : SHOT_HIST; if (n <= 0) return s->pos;
    int newest = (s->nhist - 1) % SHOT_HIST;
    if (tq >= s->hist_t[newest]) { float d = s->age - s->hist_t[newest], u = d > 1e-6f ? (tq - s->hist_t[newest]) / d : 1; if (u > 1) u = 1;
        Vec3 a = s->hist[newest]; return (Vec3){ a.x + (s->pos.x - a.x) * u, a.y + (s->pos.y - a.y) * u, a.z + (s->pos.z - a.z) * u }; }
    for (int k = 1; k < n; k++) {
        int j = (s->nhist - 1 - k) % SHOT_HIST, j1 = (s->nhist - k) % SHOT_HIST;
        if (tq >= s->hist_t[j]) { float d = s->hist_t[j1] - s->hist_t[j], u = d > 1e-6f ? (tq - s->hist_t[j]) / d : 0; Vec3 a = s->hist[j], b = s->hist[j1];
            return (Vec3){ a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u }; }
    }
    return s->hist[(s->nhist - n) % SHOT_HIST];                                     /* older than the ribbon: its first point (all start on the muzzle, 0x47d160) */
}
/* 0x449130: the sound and the visual of a new projectile. Visual 0/1 take a missile model from the pool and open with
 * their own muzzle flash, 2 opens with the flash of the bolt, 3 with the 1-second glow of 0x4702b0, and from 4 on a projectile is silent and invisible
 * (the thrown bomb of template 0). SoundFx 17/18/19/20 by visual (SOUND.md 5), 3D on the owner. */
static void shot_begin(Shot *s, const Instance *owner, int sound_fx)
{
    if (s->visual >= 4) return;                                                    /* jump table 0x4492b8 has four entries */
    if (owner) audio_fx(sound_fx, owner, &owner->position.x);
    if (shot_is_missile(s)) { s->missile = missile_take(s->pos, s->dir); flash_add(s->pos, 1); }
    else if (s->visual == 3) { s->fb_prev = s->pos; s->fb_acc = 0; flash_add(s->pos, 2); }   /* 0x470af0: fx+0x24 = start, fx+0x40 = 0, record 0x4702b0 at the start */
    else flash_add(s->pos, 0);
}
static int shot_sound(int visual) { return visual == 0 ? 17 : visual == 1 ? 18 : visual == 2 ? 19 : 20; }
/* 0x4490a0 -> Init 0x449130: the first free of the 200 slots (none: nothing happens) */
static Shot *shot_spawn(const ProjT *T, Vec3 pos, Vec3 dir, const Instance *owner, Enemy *enemy, const Instance *target, int sound_fx)
{
    for (int i = 0; i < 200; i++) if (!g_shots[i].active) {
        Shot *s = &g_shots[i]; memset(s, 0, sizeof *s); s->active = 1; s->T = *T; s->owner = owner; s->enemy = enemy; s->target = target;
        float l = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z); if (l > 1e-6f) { dir.x /= l; dir.y /= l; dir.z /= l; }
        s->pos = s->origin = pos; s->dir = s->dir0 = dir; s->speed = T->speed; s->vel = (Vec3){ dir.x * T->speed, dir.y * T->speed, dir.z * T->speed };
        s->life = T->life; s->damage = T->damage; s->visual = T->visual;
        shot_hist_push(s);
        shot_begin(s, owner, sound_fx); return s;
    }
    return NULL;
}
static Vec3 shot_target_pos(const Instance *t) { return g_player && t == g_player->inst ? g_player->pos : t->position; }   /* target+0xc: the Perso's feet */
static void launcher_fire(Launcher *l)                                             /* 0x452560 -> 0x4490a0 / 0x449130 */
{
    Vec3 p0, d; if (!inst_vector(l->inst, 0, &p0, &d)) return;                      /* marker 0 in the current pose, before the anim restarts */
    if (l->aim && l->target) { Vec3 tp = shot_target_pos(l->target); d.x = tp.x - p0.x; d.y = tp.y + l->t.aim_h - p0.y; d.z = tp.z - p0.z; }
    float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z); if (len < 1e-4f) return;
    if (wenv("WOODY_LAUNCHLOG")) printf("%.3f FIRE launcher %u vis %d listed %d at %.0f %.0f %.0f dir %.3f %.3f %.3f visual %d aim_h %.0f aim %d life %.2f t_xz %.2f target %u | player %.0f %.0f %.0f",
                                        g_now, l->inst->index, l->inst->visible, l->inst->listed, p0.x, p0.y, p0.z, d.x / len, d.y / len, d.z / len, l->t.visual, l->t.aim_h, l->aim, l->t.life, l->t.t_xz,
                                        l->target ? l->target->index : 0u, g_player ? g_player->pos.x : 0, g_player ? g_player->pos.y : 0, g_player ? g_player->pos.z : 0), puts("");
    if (l->anim >= 0 && (uint32_t)l->anim < l->inst->model->nanims && l->anim_dur > 0) {   /* 0x4526fb: the shooting anim lasts exactly param 8 * 0.01 s, also without a bomb */
        float L = l->inst->model->anims[l->anim].duration_s > 0 ? l->inst->model->anims[l->anim].duration_s : 1.0f; inst_play_once(l->inst, l->anim, L / l->anim_dur, g_now); }
    if (l->kind == 0) {                                                            /* the bomb thrower: 0x44d5d0(&L->T, 0, -1, 0), SoundFx 14 on the launcher */
        BombT t = { l->t.radius, l->t.gravity, l->t.speed, l->t.damp_g, l->t.damp_a, l->t.life, l->t.damage };
        Bomb *b = bomb_start(&t, p0, (Vec3){ d.x / len, d.y / len, d.z / len }, 0, -1, 0);
        if (b) { b->launcher = l->inst; audio_fx(14, l->inst, &l->inst->position.x); }
        return;
    }
    shot_spawn(&l->t, p0, (Vec3){ d.x / len, d.y / len, d.z / len }, l->inst, NULL, l->target, shot_sound(l->t.visual));   /* T.target = L->target, T.owner = L */
}
/* projectile of a shooting enemy (0x418820 / the ghost 0x414d10): template 1 with the enemy's speed, damage, xz steering and
 * visual (P+0x60, +0x40, +0x68, +0x74), aim height and vertical steering P+0x70 = 0; the target is the Perso */
void game_enemy_shot(Enemy *owner, Vec3 pos, Vec3 dir, float speed, float damage, float steer, int visual, int sound_fx)
{
    ProjT T = PROJ_T[1]; T.speed = speed; T.damage = damage; T.steer = steer; T.vsteer = 0; T.aim_h = 0; T.visual = visual;
    shot_spawn(&T, pos, dir, owner->inst, owner, g_player ? g_player->inst : NULL, sound_fx);
}
void game_enemy_shot_v(Enemy *owner, Vec3 pos, Vec3 dir, float speed, float damage, float steer, float vsteer, float aim_h, int visual, int sound_fx)
{
    ProjT T = PROJ_T[1]; T.speed = speed; T.damage = damage; T.steer = steer; T.vsteer = vsteer; T.aim_h = aim_h; T.visual = visual;
    shot_spawn(&T, pos, dir, owner->inst, owner, g_player ? g_player->inst : NULL, sound_fx);
}
static float v3len(Vec3 v) { return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); }
static Vec3 v3scale_to(Vec3 v, float s) { float l = v3len(v); return l > 0 ? (Vec3){ v.x / l * s, v.y / l * s, v.z / l * s } : v; }
/* 0x4493c0 without a carried instance: velocity (gravity, damping, seeking with its two clamps), then Move 0x449cc0 */
static void shot_steer(Shot *s, float dt)
{
    const ProjT *T = &s->T; float f = dt * 60.0f;                                   /* 0x4aabbc */
    s->vel.y -= dt * T->gravity * 200.0f; if (s->vel.y < -800.0f) s->vel.y = -800.0f;   /* never "grounded": that is only set for a carried bomb */
    float k = powf(T->damp_a, f); s->vel.x *= k; s->vel.y *= k; s->vel.z *= k;
    if (!s->target) return;                                                        /* 0x4494d9 */
    Vec3 old = s->vel, tp = shot_target_pos(s->target); tp.y += T->aim_h;
    if (s->age < T->t_xz) {                                                        /* 0x44952b: xz toward the target, y of the unit vector kept */
        float sp = v3len(s->vel); Vec3 v = sp > 0 ? (Vec3){ s->vel.x / sp, s->vel.y / sp, s->vel.z / sp } : s->vel;
        float dx = tp.x - s->pos.x, dz = tp.z - s->pos.z, dl = sqrtf(dx * dx + dz * dz); if (dl > 0) { dx /= dl; dz /= dl; }
        float kk = powf(1.0f - T->steer, f);
        v.x = dx * (1 - kk) + v.x * kk; v.z = dz * (1 - kk) + v.z * kk;
        s->vel = v3scale_to(v, sp);
    }
    if (s->age < T->t_y && T->vsteer != 0 && fabsf(s->pos.y - tp.y) > 0) {         /* 0x44969d: a point one second ahead, vsteer per 1/60 s up or down */
        float dy = s->pos.y - tp.y, st = f * T->vsteer; if (dy > 0) st = -st;
        Vec3 q = { s->pos.x + s->vel.x, s->pos.y + s->vel.y + st, s->pos.z + s->vel.z }; int reached = dy > 0 ? q.y < tp.y : q.y > tp.y; if (reached) q.y = tp.y;
        float sp = v3len(s->vel); Vec3 v = v3scale_to((Vec3){ q.x - s->pos.x, q.y - s->pos.y, q.z - s->pos.z }, 1.0f);
        if (reached) { v.y *= 4.0f; v = v3scale_to(v, 1.0f); }                     /* 0x4a94c0 */
        s->vel = (Vec3){ v.x * sp, v.y * sp, v.z * sp };
    }
    if (1.0f + s->vel.y * s->dir0.y < T->lim_y) { float sp = v3len(s->vel); s->vel.y = old.y; s->vel = v3scale_to(s->vel, sp); }   /* 0x4498ec: literally (a speed-scaled y) */
    if (s->vel.x * s->dir0.x + s->vel.z * s->dir0.z < T->lim_xz) { float sp = v3len(s->vel); s->vel.x = old.x; s->vel.z = old.z; s->vel = v3scale_to(s->vel, sp); }   /* 0x4499cf */
}
static void launchers_update(float now, float dt, Player *pl, const GelFile *gel, int player_ok)
{
    for (int i = 0; i < g_nlaunchers; i++) {                                       /* think step 0x452780 = vtbl[3] of 0x4ab148, run only by 0x42b400 for the
                                                                                    * instances of this frame's list world+0x64: a hidden or unlisted launcher never fires */
        Launcher *l = &g_launchers[i]; if (!l->active || !l->inst->visible || !game_enemy_thinks(l->inst)) continue;
        if (floorf((now - l->t0) / l->T) > floorf((now - dt - l->t0) / l->T) && now - l->last >= l->T - 0.2f) {
            l->last = now; launcher_fire(l); if (l->count > 0 && --l->count == 0) l->active = 0;
        }
    }
    for (int i = 0; i < 200; i++) {                                                /* 0x4490f0 / 0x4493c0 */
        Shot *s = &g_shots[i]; if (!s->active) continue;
        if (s->dying > 0) { if ((s->dying -= dt) <= 0) s->active = 0; continue; }
        s->age += dt; int end = s->age >= s->life; Vec3 a = s->pos, b = a;          /* 0x4493ec: out of time, nothing moves */
        if (!end) {
            shot_steer(s, dt);
            s->speed = v3len(s->vel); if (s->speed > 0) s->dir = (Vec3){ s->vel.x / s->speed, s->vel.y / s->speed, s->vel.z / s->speed };
            b = (Vec3){ a.x + s->vel.x * dt, a.y + s->vel.y * dt, a.z + s->vel.z * dt };
        }
        if (!end && player_ok && s->T.hits_all) {                                  /* HitActors 0x44a0a0: 0x433920(old, new, r 5, feet, 69, H) = vtbl[24] 0x44cd60 {pos + (0, H/2, 0), 69, H/2} */
            if (sweep_sphere_cyl(a, b, s->T.radius, pl->pos, 69.0f, player_body_height(pl))) {
                if (player_hit(pl, s->damage, s->dir)) { player_kill(pl, 3); enemy_player_killed(s->enemy); }
                b = (Vec3){ a.x + (b.x - a.x) * g_hit_frac, a.y + (b.y - a.y) * g_hit_frac, a.z + (b.z - a.z) * g_hit_frac }; end = 1;   /* port: the shot ends where the sweep met him */
            }
        }
        if (!end) {                                                                /* the other members of actor list 1: the bomb thrower (type 12) and Boss2 (15) */
            Enemy *h = enemies_bomb_contact(&g_enemies, s->enemy, a, b, s->T.radius);
            if (h) { enemy_hit(h, s->damage, s->dir, b, 0); end = 1; if (wenv("WOODY_FXLOG")) printf("shot %d hits enemy %u", i, h->inst->index), puts(""); }
        }
        if (!end) {                                                                /* Move 0x449cc0 -> Ray 0x4359b0(old, pos, -1), the laser's ray: world polygons (hit kind 1) and
                                                                                    * the press nodes of instances (2), the launcher's own included; kind 3 = the start point
                                                                                    * lies inside a press node (0x4330c0): the projectile is gone */
            const Instance *hi = NULL; Vec3 n, ni; float fi;
            if (inst_point_in_press(gel, &g_ins,NULL, a, &hi)) { end = 1; b = a; if (wenv("WOODY_FXLOG")) printf("shot %d starts inside instance %u (hit kind 3)", i, hi->index), puts(""); }
            else {
                float f = gel_ray_hit(gel, a, b, &n);
                if (inst_ray_press(gel, &g_ins,NULL, a, b, &fi, &ni, &hi) && fi < f) { f = fi; n = ni; } else hi = NULL;
                if (f <= 1.0f) {
                    if (s->T.max_bounce != -1 && s->bounces >= s->T.max_bounce) { b = (Vec3){ a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f }; end = 1; }   /* 0x449dff */
                    else {                                                         /* Bounce 0x449eb0: the end point mirrored in the plane, the speed kept, the point put on the hit */
                        Vec3 d = { b.x - a.x, b.y - a.y, b.z - a.z }; float L = v3len(d), sl = L * f - 0.01f; if (sl < 0) sl = 0;
                        Vec3 h = L > 1e-6f ? (Vec3){ a.x + d.x / L * sl, a.y + d.y / L * sl, a.z + d.z / L * sl } : a;
                        float q = 2.0f * ((b.x - (a.x + d.x * f)) * n.x + (b.y - (a.y + d.y * f)) * n.y + (b.z - (a.z + d.z * f)) * n.z); Vec3 r = { b.x - q * n.x, b.y - q * n.y, b.z - q * n.z };   /* mirrored in the hit plane itself (0x449f6f) */
                        Vec3 u = { r.x - h.x, r.y - h.y, r.z - h.z }; if (v3len(u) > 1e-6f) { s->dir = v3scale_to(u, 1.0f); s->vel = (Vec3){ s->dir.x * s->speed, s->dir.y * s->speed, s->dir.z * s->speed }; }
                        b = h; s->bounces++;
                        if (wenv("WOODY_FXLOG")) printf("shot %d bounces (%d) at %.0f %.0f %.0f", i, s->bounces, h.x, h.y, h.z), puts("");
                    }
                }
                if (hi && end && wenv("WOODY_FXLOG")) printf("shot %d (owner %u) stops on instance %u (model %d) after %.2f s at %.0f %.0f %.0f", i, s->owner ? s->owner->index : 0u, hi->index, (int)(hi->model - g_ins.models), s->age, b.x, b.y, b.z), puts("");
            }
        }
        s->pos = b;
        if (!end) {
            s->hist_acc += dt;                                                     /* 0x47d090: a ribbon point every fx+0x3c */
            if (s->visual <= 2 && s->hist_acc > shot_span(s) / (shot_nseg(s) * s->T.speed)) { shot_hist_push(s); s->hist_acc = 0; }
            if (s->missile) missile_place(s->missile, s->pos, s->dir);             /* 0x4723d0 -> 0x4724e0: the model rides along */
            continue;
        }
        /* 0x46fbca: a missile explodes (0x477060 kind 2, radius 400, no damage of its own) and hands its model back;
         * the fireball explodes the same way (0x4704d7, at fx+0x24: the last position its record saw, the one before
         * this step); the bolt only leaves the flash of 0x46f36d. Either way the trail goes on shrinking for a moment. */
        if (shot_is_missile(s)) { blast_add(b, 400.0f); missile_release(s->missile); s->missile = NULL; } else if (s->visual == 3) blast_add(a, 400.0f); else if (s->visual == 2) flash_add(b, 0);
        s->dying = shot_fade_len(s);
        if (wenv("WOODY_FXLOG")) printf("shot %d (visual %d) ends at %.0f %.0f %.0f age %.2f from %.0f %.0f %.0f", i, s->visual, b.x, b.y, b.z, s->age, s->origin.x, s->origin.y, s->origin.z), puts("");
    }
    for (int i = 0; i < 64; i++) if (g_flashes[i].t > 0) { g_flashes[i].t += dt; if (g_flashes[i].t >= (g_flashes[i].kind == 2 ? 1.0f : 0.4f)) g_flashes[i].t = 0; }
}
/* ---- class 14, the Buzz boss (boss.c, docs/BOSS14.md): the engine side ------------------------------------------
 * mailbox variable, camera shake, the HUD boss bar, the kind-1 explosion and the smoke plumes on the markers of the
 * coupled machine. The plume 0x475f30 (300 particles a second over the way the marker moved, +-15 in x/z, 0.5 s, image 14,
 * rising 50 t, size rand*10 + 20) is from the call sites; its colour and alpha curve are a reconstruction. */
int  game_var_get(uint32_t var) { var &= 0xffffff; return g_vm && var < g_vm->nvars ? g_vm->varval[var] : 0; }
void game_var_set(uint32_t var, int v) { if (g_vm) eko_set_var(g_vm, var, v); }
void game_cam_shake(float t) { g_cam.shake = t; }
void game_msgmask(Instance *in, uint32_t bits, int on) { if (g_vm && in) { if (on) eko_msgmask_set(g_vm, in->id, bits); else eko_msgmask_clear(g_vm, in->id, bits); } }
/* the event half of the generic probe 0x436dc0 (docs/EVENTS.md 3.2), actor = the enemy's or the carried bomb's instance id:
 * on the ground over a world_collision -> In when it is the probe's current one (+0x20), else Press and it becomes current
 * (moving from A straight onto B sends no UnPress(A)); otherwise (terrain, a node without collision, in the air) UnPress of
 * the current one. The non-Perso variants 0x442080 / 0x4420c0 / 0x442100: only COL_B3_BIT0 (and a collision's count) sees them */
void game_col_probe(uint32_t *cur, int on, uint32_t col, const Instance *actor)
{
    if (!g_vm || !actor) return;
    const char *lg = wenv("WOODY_COLLOG");
    if (on && col != 0xffffffffu) {
        if (*cur == col) { eko_col_in(g_vm, col, actor->id); return; }                                     /* 0x436e75 */
        eko_col_press(g_vm, col, actor->id); *cur = col;                                                    /* 0x436e8d */
        if (lg) printf("  COL press 0x%x by instance %u", col, actor->index), puts("");
        return;
    }
    if (*cur != 0xffffffffu) {                                                                              /* 0x436eb7 / 0x436ee5 */
        eko_col_unpress(g_vm, *cur, actor->id);
        if (lg) printf("  COL unpress 0x%x by instance %u", *cur, actor->index), puts("");
        *cur = 0xffffffffu;
    }
}
static struct { int on, cur, max; float t; } g_bossbar;      /* hud+0x48, +0x4c, +0x50 and the bar's slide-in clock */
void game_boss_bar(int on, int cur, int max) { if (on && !g_bossbar.on) g_bossbar.t = 0; g_bossbar.on = on; g_bossbar.cur = cur; g_bossbar.max = max; }   /* 0x4484d0: 0x462470 starts the slide-in */
void game_explosion(Vec3 p) { fx_explode(1, p, (Vec3){ 0, 1, 0 }); blast_add(p, 1400.0f); blast_add(p, 400.0f); }
/* classes 15 / 16 (boss.c, docs/BOSS15_16.md) */
float game_time(void) { return g_now; }
void game_launcher_start(Instance *in)                                         /* 0x4522b0(1, 1.0, 0) = message 1000 [inst, -1]: one shot on the next think step */
{
    Launcher *l = in ? launcher_of(in) : NULL; if (!l) return;
    l->target = NULL; l->count = 1; l->T = 1.0f; l->t0 = g_now + g_dt; l->last = g_now - 1.0f; l->active = 1;
}
void game_bombs_crush(Vec3 c, float r)                                         /* Boss2 0x40e9fd: the bombs in state 2 near a crusher go off */
{
    for (int i = 0; i < g_nbombs; i++) { Bomb *b = &g_bombs[i]; if (!b->in_use || b->state != 2) continue;
        Vec3 d = { b->inst->position.x - c.x, b->inst->position.y - c.y, b->inst->position.z - c.z };
        if (d.x * d.x + d.y * d.y + d.z * d.z < r * r) { if (wenv("WOODY_BOMBLOG")) printf("  BOMB %u crushed", b->inst->index), puts(""); bomb_explode(b); } }
}
void game_bombs_discard(void) { bombs_discard_all(); }
/* the smoke plume 0x475f30(inst, n): an emitter record in the effect pool (life 100000 s, callback 0x475d90) on the
 * typecode-0 marker n of `inst`, and the global byte smoke_on[n] = [0x5e857c + n] = 1. The emitter lives while its byte is
 * 1 (a byte of 0 frees it on its next run: the boss's last hit and Reset, message 1509 mode 4 x = 0). Every frame it takes
 * k = fistp(acc * 300) puffs (acc -= k / 300) at p + (i/k)(prev - p) + (rnd*30 - 15) in x and z, p = the marker now, and
 * `prev` becomes the LAST puff's position (jitter included), so the trail lags behind a moving marker. Puff 0x475cd0:
 * 0.5 s, u = t / 0.5, camera facing image 14 (bank 0), colour (1,1,1), alpha 0.5 - 0.5u, rotation fistp(rnd*512),
 * size rnd*10 + 50u + 20, y + 50u; sprite mode 0x12, flags 7 (own colour, rotation, additive). */
static struct { Instance *link; int n; Vec3 prev; float acc; } g_bplume[8]; static int g_nbplume;
static uint8_t g_smoke_on[3];                                                   /* 0x5e857c..0x5e857e */
static struct { Vec3 pos; float t, size; int rot; } g_bsmoke[1024]; static int g_bsmoke_next;
static int fistp(float v) { return (int)lrintf(v); }                         /* fistp: the FPU's round-to-nearest, unlike ftol 0x499580 */
static void smoke_attach(Instance *link, int n)                                /* 0x475f30 */
{
    if (!link || n < 0 || n > 2 || g_nbplume >= 8) return;
    Vec3 p, d; if (!inst_vector_at(link, 0, (uint32_t)n, &p, &d)) p = link->position;   /* 0x42f6b0 leaves the vector as it was; the port uses the origin */
    g_smoke_on[n] = 1; g_bplume[g_nbplume].link = link; g_bplume[g_nbplume].n = n; g_bplume[g_nbplume].prev = p; g_bplume[g_nbplume].acc = 0; g_nbplume++;
}
void game_boss_smoke(Instance *link, int n, int on)
{
    if (n < 0) { g_smoke_on[0] = g_smoke_on[1] = g_smoke_on[2] = 0; return; }   /* 0x40fe57 / Reset: the three bytes */
    if (!link || n > 2 || !on) return;
    Vec3 p, d; if (!inst_vector_at(link, 0, (uint32_t)n, &p, &d)) p = link->position;
    game_explosion(p); smoke_attach(link, n);                                  /* 0x477060(1, v, 0) + 0x475f30 (BOSS14.md 9.2) */
}
/* message 1509 [a, inst, mode, x] (0x46cf6f; only W1B's outro, object 397 around cinematic 73 with the saucer 399):
 * mode 5 = explosion kind 1 at typecode-0 marker x of inst (0x42f6b0(0, &v, x), 0x477060(1, &v, 0)); mode 4, x == 1 =
 * the three plumes 0x475f30(inst, 0/1/2); mode 4, x != 1 = the three bytes 0 (the plumes die out). a is not read. */
static void game_msg1509(Instance *in, int mode, int x)
{
    if (!in) return;
    if (mode == 5) { Vec3 p, d; if (!inst_vector_at(in, 0, (uint32_t)x, &p, &d)) return; game_explosion(p); }
    else if (mode == 4) { if (x == 1) { smoke_attach(in, 0); smoke_attach(in, 1); smoke_attach(in, 2); } else game_boss_smoke(NULL, -1, 0); }
    if (wenv("WOODY_FXLOG")) printf("  1509 inst %u mode %d x %d (plumes %d on %d%d%d)\n", in->index, mode, x, g_nbplume, g_smoke_on[0], g_smoke_on[1], g_smoke_on[2]);
}
static void boss_fx_draw(float dt)
{
    static const float one[3] = { 1, 1, 1 };
    for (int e = 0; e < g_nbplume; e++) {                                       /* 0x475d90 */
        if (g_smoke_on[g_bplume[e].n] != 1) { g_bplume[e--] = g_bplume[--g_nbplume]; continue; }
        if (dt <= 0) continue;
        g_bplume[e].acc += dt; int k = fistp(g_bplume[e].acc * 300.0f); g_bplume[e].acc -= k * (1.0f / 300.0f);
        Vec3 p, d; if (!inst_vector_at(g_bplume[e].link, 0, (uint32_t)g_bplume[e].n, &p, &d)) p = g_bplume[e].link->position;
        Vec3 D = { g_bplume[e].prev.x - p.x, g_bplume[e].prev.y - p.y, g_bplume[e].prev.z - p.z };
        for (int i = 0; i < k; i++) {
            float f = (float)i / (float)k, jx = fx_rnd() * 30.0f, jz;
            Vec3 q; q.x = f * D.x + jx + p.x - 15.0f; q.y = f * D.y + p.y; jz = fx_rnd() * 30.0f; q.z = f * D.z + jz + p.z - 15.0f;
            int j = g_bsmoke_next++ % 1024; g_bsmoke[j].pos = q; g_bsmoke[j].t = 1e-6f; g_bsmoke[j].size = fx_rnd() * 10.0f; g_bsmoke[j].rot = fistp(fx_rnd() * 512.0f);
            if (i == k - 1) g_bplume[e].prev = q;
        }
    }
    for (int i = 0; i < 1024; i++) if (g_bsmoke[i].t > 0) {                     /* 0x475cd0 */
        if (dt > 0) g_bsmoke[i].t += dt;
        float u = g_bsmoke[i].t / 0.5f; if (u >= 1) { g_bsmoke[i].t = 0; continue; }
        float pos[3] = { g_bsmoke[i].pos.x, g_bsmoke[i].pos.y + 50.0f * u, g_bsmoke[i].pos.z };
        hud_world_spr(14, pos, g_bsmoke[i].size + 50.0f * u + 20.0f, g_bsmoke[i].rot, one, 0.5f - 0.5f * u, 7, NULL, 0);
    }
}

/* death stars 0x477610 (docs/PERSO_DEATH.md 7): five sprites circle over the head of a dying enemy for 2.5 s, one turn, bobbing six times */
static struct { Enemy *e; float age; int img; } g_stars[16];
void game_enemy_stars(Enemy *e)
{
    for (int i = 0; i < 16; i++) if (!g_stars[i].e) { g_stars[i].e = e; g_stars[i].age = 0; g_stars[i].img = (rand() & 1) ? 11 : 10; return; }
}
static void stars_draw(float dt)
{
    static const float grey[3] = { 0.5f, 0.5f, 0.5f }, glow[3] = { 1, 1, 0.5f };   /* 0x4775e7: rgb 0.5 with flags 0x4f (alpha blended, x2 = the plain texture); the glow 0x477562: (1, 1, 0.5) with flags 3 (additive) */
    for (int s = 0; s < 16; s++) {
        Enemy *e = g_stars[s].e; if (!e) continue;
        float u = (g_stars[s].age += dt) / 2.5f;
        if (u >= 1 || e->removed || !e->inst->visible) { g_stars[s].e = NULL; continue; }
        Vec3 p0, d; float len;
        if (inst_vector(e->inst, 0, &p0, &d)) len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z); else { p0 = e->inst->position; len = enemy_height(e); }
        float a = u < 0.7f ? 1.0f : 1.0f - (u - 0.7f) * 3.33f;
        for (int i = 1; i <= 5; i++) {
            int ang = (i * 512 / 5 + (int)(512 * u)) % 512; float f = ang * 6 / 256.0f; int k = (int)f; float w = (k & 1) ? f - k : 1 - (f - k), r = ang * 6.2831853f / 512.0f;
            float pos[3] = { p0.x + 80 * cosf(r), p0.y + len + (4 * w) * (4 * w), p0.z + 80 * sinf(r) };
            if (g_stars[s].img == 10) { float gp[3] = { pos[0], pos[1] + 15, pos[2] }; hud_world_fx(5, gp, 40, 0, glow, 0.8f * a); }
            hud_world_fx(g_stars[s].img, pos, 40, (float)(int)((w - 0.5f) * 56) / 512.0f, grey, a);
        }
    }
}
/* speech bubble 0x478980 / callback 0x4786f0 (docs/PERSO_DEATH.md 4.1): a comic balloon beside and above an instance,
 * "?!" when Woody falls to his death (kind 0), a curse after a hard landing (1), "zzz" while he sleeps (4, lives as long
 * as *live != 0), and whatever the script asks for with message 1500. The side is chosen once: an instance on the right
 * half of the screen (camera-space x >= 0, 0x4789e3) gets the balloon on its left, mirrored, so the tail points at it. */
typedef struct { Instance *inst; float age, dur, offy, offx; int img[4], n, side; const int *live; } Bubble;
static Bubble g_bubbles[8];
void game_bubble(Instance *inst, int kind, float dur, float offy, float offx, const int *live)
{
    static const int k_img[5][4] = { { 0x2d }, { 0x33, 0x34 }, { 0x2e }, { 0x32 }, { 0x2f, 0x30, 0x31, 0 } }, k_n[5] = { 1, 2, 1, 1, 4 };   /* 0x478a8c */
    if (!inst || kind < 0 || kind > 4) return;                            /* other kinds leave the record without images */
    for (int i = 0; i < 8; i++) if (!g_bubbles[i].inst) {
        g_bubbles[i].inst = inst; g_bubbles[i].age = 0; g_bubbles[i].dur = dur; g_bubbles[i].offy = offy; g_bubbles[i].offx = offx;
        memcpy(g_bubbles[i].img, k_img[kind], sizeof g_bubbles[i].img); g_bubbles[i].n = k_n[kind]; g_bubbles[i].side = -1; g_bubbles[i].live = live;
        if (wenv("WOODY_BUBLOG")) printf("  BUBBLE kind %d dur %.2f off %.0f/%.0f%s\n", kind, dur, offy, offx, live ? " (live)" : "");
        return;
    }
}
static void bubbles_draw(const FreeCamera *cam, float dt)
{
    for (int i = 0; i < 8; i++) {
        Bubble *b = &g_bubbles[i]; if (!b->inst) continue;
        float u = (b->age += dt) / b->dur;
        if (b->live ? *b->live == 0 : u >= 1) { b->inst = NULL; continue; }
        Vec3 T = ins_anim_centre(b->inst);                               /* inst+0x60: the animated root, it falls with him */
        if (b->side < 0) { Vec3 r = cam_right(cam); b->side = (T.x - cam->pos.x) * r.x + (T.y - cam->pos.y) * r.y + (T.z - cam->pos.z) * r.z >= 0; }
        float s = b->live ? (b->age < 0.5f ? 2 * b->age : 1) : u < 0.04f ? 25 * u : u > 0.96f ? 25 * (1 - u) : 1;   /* pops in and out */
        float dx = cam->pos.x - T.x, dz = cam->pos.z - T.z, l = sqrtf(dx * dx + dz * dz);
        if (l > 0) { dx /= l; dz /= l; }
        float size = 90 * s + 30, h = size * 0.5f, X = b->side ? -(b->offx + h) : b->offx + h, Y = b->offy + h;   /* R = (d.z, 0, -d.x) (0x46d320), U = +y */
        float pos[3] = { T.x + dz * X, T.y + Y, T.z - dx * X };
        hud_world_bubble(0x2c, pos, size, b->side);
        int img = b->img[(int)(u * 8) % b->n];
        if (img) hud_world_bubble(img, pos, 55 * s + 10, 0);
    }
}
/* ---- pickup effects (docs/BONUS.md 2.4, 0x4793d0) --------------------------------------------------------------
 * One emitter record per pickup. Neither emitter draws: both spawn the same particle (0x4791f0), a camera facing
 * additive quad of bank 0 image 4 whose size swells 0 -> 30 -> 0 over its life. One pool as in the original
 * ([0x5e823c]+0xdb8, 2000 records, no free list: a dead record is swapped with the last one). */
static const float k_fx_shape[12][3] = {                                     /* 0x4b7990: the 8 corners of a cube, then a tetrahedron */
    { 1, 1, 1}, { 1, 1,-1}, { 1,-1, 1}, { 1,-1,-1}, {-1, 1, 1}, {-1, 1,-1}, {-1,-1, 1}, {-1,-1,-1},
    { 0, 0.5f, 0}, {-0.494f,-0.5f, 0.855f}, { 1,-0.5f, 0}, {-0.494f,-0.5f,-0.855f},
};
typedef struct { float age, life, acc; Vec3 pos; int kind, shape; Vec3 dir; float D, h, R, acc2; Vec3 n; float acc3, size; int rot, foot; AmbFx amb; } FxRec;   /* kind FX_AMB: a class-90 particle (ambient.c, amb = its fields); kinds 12..25: the particles of docs/PARTICLES.md (enum FxKind below); the special attack (docs/PERSO_SPECIAL.md 3): 7 = its emitter (0x47a8d0), 8 = a streak (0x47a790), 9 = a fire ring (0x47a4c0);
    the hit star 0x4750e0: 10 = the flash (0x475040), 11 = a spark (0x474e00, dir = its spoke, R = star size, shape = spin) */   /* kind 0 = shape burst (0x478f70), 1 = sparkle box (0x4792d0), 2 = the particle (0x4791f0);
    the water splash (docs/SPLASH.md): 3 = its emitter (0x478360), 4 = a drop (0x477fa0), 5 = the ripple where a drop lands (0x478290), 6 = a ring (0x4781b0) */
static FxRec g_fx[2000]; static int g_nfx;
enum FxKind {                                                                 /* docs/PARTICLES.md, one record callback each */
    FX_STEP_EMIT = 12, FX_STEP_PUFF, FX_STEP_PRINT,                          /* 0x47e460, 0x47e370, 0x47c9a0: footsteps (2) */
    FX_RING,                                                                 /* 0x475fb0: one cloud of the smoke ring 0x476140 (3) */
    FX_PECK_EMIT, FX_PECK_CHIP, FX_PECK_FLASH, FX_PECK_HOLE,                 /* 0x4798f0, 0x479670, 0x479760, 0x479800: the beak impact 0x479c80 (4) */
    FX_DEBRIS_EMIT, FX_DEBRIS,                                               /* 0x476cd0, 0x4764f0: the dust burst of an explosion (5.1) */
    FX_BURN_EMIT, FX_BURN, FX_TRAIL_SMOKE, FX_TRAIL_SPARK,                   /* 0x476b50, 0x4767f0, 0x476f00, 0x476fb0: the burning debris of explosion kind 1 (5.2) */
    FX_SKELETON,                                                             /* 0x477980: the skeleton flash of Kill 2/9 (6, docs/PERSO_DEATH.md 4.2) */
    FX_BOARD_PUFF                                                            /* 0x475380: a puff of the race board's spray (docs/RACE.md 2.2) */,
    FX_FLAME,                                                                /* 0x47cd00: one flame of a torch of message 1508 (9) */
    FX_AMB                                                                   /* class 90 (docs/AMBIENT.md): butterfly 0x47d440, mote 0x47dca0, drop 0x47de10, ripple 0x47df80 */
};
static void fx_particle(FxRec *e, float u, float dt);
static FxRec *fx_new(float life, Vec3 pos, int kind)
{
    if (g_nfx >= 2000) return NULL;                                          /* 0x4793e7: a full pool silently drops the effect */
    FxRec *r = &g_fx[g_nfx++]; memset(r, 0, sizeof *r); r->life = life; r->pos = pos; r->kind = kind; return r;
}
AmbFx *game_fx_amb_new(void)                                                 /* 0x47e050 / 0x47e160 / 0x47e230 / 0x47e310: the same pool, the same full test */
{
    FxRec *r = fx_new(1.0f, (Vec3){ 0, 0, 0 }, FX_AMB);
    return r ? &r->amb : NULL;
}
static float fx_rnd(void) { return (float)rand() / (float)RAND_MAX; }        /* 0x43ff40: rand() / 32767, so [0,1] inclusive */
static void fx_rotmat(int a0, int a1, int a2, float M[9])                    /* 0x46d220; the angles are in 1/512 turn */
{
    const float k = 6.2831853f / 512.0f;
    float S0 = sinf(a0 * k), C0 = cosf(a0 * k), S1 = sinf(a1 * k), C1 = cosf(a1 * k), S2 = sinf(a2 * k), C2 = cosf(a2 * k);
    M[0] = C1 * C2;  M[1] = S0 * S1 * C2 + C0 * S2;  M[2] = S0 * S2 - C0 * S1 * C2;
    M[3] = -C1 * S2; M[4] = C0 * C2 - S0 * S1 * S2;  M[5] = S0 * C2 + C0 * S1 * S2;
    M[6] = S1;       M[7] = -S0 * C1;                M[8] = C0 * C1;
}
static struct { int kind; Vec3 pos; } g_pick[8]; static int g_npick;        /* pickups waiting to be projected: 0x448510 needs the view matrix, which the frame loop owns */
/* ---- unique items, type 36 (docs/BONUS.md 2.3 / 2.5): the ctor 0x44f67e numbers them in the order the level script
 * makes them (1200 [slot, 36]; counter [0x5e54f0], back to 0 with the level). Collecting one (0x44f700) sets the byte
 * rec+0x05+n of this level in the save block of the Perso's character (0x450760(cfg+0x380, level, n)); the update
 * 0x44f770 tests that byte (0x450730) every frame and takes the item out of the world (0x407850) while it is set, so
 * an item taken once never comes back in that save, not even after a reload of the level. */
static Instance *g_uniq[64]; static int g_nuniq;
static uint8_t *uniq_flag(const Instance *in)
{
    int n = 0; while (n < g_nuniq && g_uniq[n] != in) n++;
    if (n >= g_nuniq || n >= 32 || g_level < 0 || g_level >= 29) return NULL;   /* rec+0x05 holds 32; the original does not check (0x450760) */
    return &g_save.chr[g_char].rec[g_level].uniq[n];
}
static void uniq_update(void) { for (int i = 0; i < g_nuniq; i++) { const uint8_t *f = uniq_flag(g_uniq[i]); if (f && *f) g_uniq[i]->visible = 0; } }

void game_pickup_fx(int n, Vec3 pos)                                         /* 0x4793d0: n = 0 life, 1 charge, 2 W, 3 unique, 4 race/invincible */
{
    FxRec *e;
    if (n == 0 || n == 1) { pos.y += 50.0f; if ((e = fx_new(2.0f, pos, 0)) != NULL) e->shape = n == 0 ? 1 : 0; }   /* tetrahedron resp. cube */
    else if (n >= 2 && n <= 4) fx_new(1.0f, pos, 1);
}
/* the water splash 0x478660 (docs/SPLASH.md): Kill(7) with (pos + (0,110,0), speed, 50) and message 1505 with (inst, 1000, f/100).
 * It draws nothing itself: for 0.3 s it throws 500 drops a second outward along a half-sine arc, and every 0.2 s a ring. */
void game_splash(Vec3 c, float speed, float radius)
{
    FxRec *e = fx_new(1.0f, c, 3); if (!e) return;
    e->h = speed * 0.001f; e->acc = 0; e->acc2 = 0.2f; e->R = fx_rnd() * 50.0f + radius;   /* accK starts at 0.2: the first ring comes in the first frame */
}
static float sin512(int i) { return sinf(6.2831853f * (i & 511) / 512.0f); }   /* -cos512[(i + 128) & 511] */
static float cos512(int i) { return cosf(6.2831853f * (i & 511) / 512.0f); }
void game_special_fx(void) { FxRec *e = fx_new(2.5f, (Vec3){ 0, 0, 0 }, 7); if (e) e->shape = 0; }   /* 0x47ab90: no position, the children read the Perso's */
static Vec3 g_fx_eye, g_fx_fwd = { 0, 0, 1 };                                /* the camera of the last drawn frame (0x4750e0 builds the spark plane on it) and its look direction */
void game_hit_star(Vec3 pt)                                                  /* 0x4750e0 */
{
    fx_new(0.1f, pt, 10);
    Vec3 n = { g_fx_eye.x - pt.x, g_fx_eye.y - pt.y, g_fx_eye.z - pt.z }; float l = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
    n = l > 1e-3f ? (Vec3){ n.x / l, n.y / l, n.z / l } : (Vec3){ 0, 0, 1 };
    Vec3 a = fabsf(n.y) < 0.9f ? (Vec3){ 0, 1, 0 } : (Vec3){ 1, 0, 0 };        /* 0x46d320: two unit vectors across the view direction */
    Vec3 U = { a.y * n.z - a.z * n.y, a.z * n.x - a.x * n.z, a.x * n.y - a.y * n.x }; l = sqrtf(U.x * U.x + U.y * U.y + U.z * U.z); U = (Vec3){ U.x / l, U.y / l, U.z / l };
    Vec3 V = { n.y * U.z - n.z * U.y, n.z * U.x - n.x * U.z, n.x * U.y - n.y * U.x };
    for (int j = 0; j < 8; j++) {                                            /* 8 spokes 45 degrees apart, +-17.6 degrees */
        FxRec *s = fx_new(0.4f, pt, 11); if (!s) continue;
        int a1 = (int)(fx_rnd() * 50.0f + 64 * j - 25), a2 = (int)(fx_rnd() * 50.0f + 64 * j - 25);
        Vec3 d = { U.x * cos512(a1) + V.x * sin512(a2), U.y * cos512(a1) + V.y * sin512(a2), U.z * cos512(a1) + V.z * sin512(a2) };
        l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z); s->dir = l > 1e-5f ? (Vec3){ d.x / l, d.y / l, d.z / l } : U;
        s->shape = fx_rnd() * 2.0f > 1.0f; s->R = fx_rnd() * 20.0f - 5.0f + 25.0f;   /* spin; size 20..40 (half diagonal) */
    }
}
/* message 1508 [inst] (0x46ceae, docs/PARTICLES.md 9): a torch. A 20-byte node {inst, n, timers, points, next} goes on the
 * list 0x5e8638 with the points of the instance's type-0 markers, taken once at the message (0x47cdf0); 0x47cea0 runs the
 * list every frame just before the pool driver (0x46d0ba) and 0x47cec0 frees it with the level. */
#define MAX_TORCH 32
static struct { Instance *in; int n; float t[4]; Vec3 p[4]; } g_torch[MAX_TORCH]; static int g_ntorch;
static void torch_add(Instance *in)
{
    if (!in || g_ntorch >= MAX_TORCH) return;
    if (!in->node_world) ins_pose(in, in->anim, in->anim_time);                  /* 0x42f6b0 poses it (vtbl[2](1)) before reading the marker */
    int n = 0; Vec3 p, d;
    while (n < 4 && inst_vector_at(in, 0, (uint32_t)n, &p, &d)) g_torch[g_ntorch].p[n++] = p;   /* 0x47ce0f: count, then read (only P0 is used) */
    g_torch[g_ntorch].in = in; g_torch[g_ntorch].n = n; memset(g_torch[g_ntorch].t, 0, sizeof g_torch[g_ntorch].t); g_ntorch++;
    if (wenv("WOODY_FXLOG")) printf("torch: inst %u, %d marker%s at %.0f %.0f %.0f", in->index, n, n == 1 ? "" : "s", n ? g_torch[g_ntorch - 1].p[0].x : 0, n ? g_torch[g_ntorch - 1].p[0].y : 0, n ? g_torch[g_ntorch - 1].p[0].z : 0), puts("");
}
static void torch_update(float dt)                                           /* 0x47cf10 per node */
{
    for (int i = 0; i < g_ntorch; i++) {
        if (!g_torch[i].in->drawn) continue;                                 /* inst+0x58 == this frame: its clock ran, i.e. it was drawn */
        for (int k = 0; k < g_torch[i].n; k++) {
            float *t = &g_torch[i].t[k]; *t += dt;
            int n = (int)(*t * 15.0f); *t -= (float)n * 0.0666667f;           /* 15 flames a second (0x4a9864, 0x4abd8c) */
            while (n-- > 0) {
                FxRec *f = fx_new(0, g_torch[i].p[k], FX_FLAME); if (!f) continue;
                float dx = fx_rnd() * 20.0f - 10.0f, dz = fx_rnd() * 20.0f - 10.0f;   /* 0x47cfc3 */
                f->pos.x += dx; f->pos.z += dz;
                float k2 = 1.0f - sqrtf(dx * dx + dz * dz) * 0.1f;           /* 1 at the centre, 0 at 10 out, down to -0.41 in the corners */
                f->R = fx_rnd() * (fx_rnd() * k2 * 20.0f) + 40.0f;           /* +0x14: the size */
                f->life = fx_rnd() * k2 * 0.5f + 2.5f;                        /* +4 */
            }
        }
    }
}
int game_enemy_thinks(const Instance *inst)                                  /* Think 0x42b400 runs for the instances of this frame's list world+0x64 (rnd_instance_list) */
{
    return !g_rnd || inst->listed;
}
uint32_t game_instance_list(Instance *const **list)                          /* world+0x64 / +0x60 in list order; 0 = no list (then *list = NULL) */
{
    if (!g_rnd || !g_rnd->list_on) { *list = NULL; return 0; }
    *list = g_rnd->list; return g_rnd->nlist;
}
static Vec3 drop_pt(const FxRec *e, float wx, float wy)                      /* a drop at fraction wx along its path; the height uses wy (fistp rounds) */
{
    return (Vec3){ e->pos.x + e->dir.x * e->D * wx, e->pos.y + sin512((int)lrintf(255.0f * wy)) * e->h * 100.0f, e->pos.z + e->dir.z * e->D * wx };
}
static void fx_update(float dt, const float *eye)
{
    static const float grey05[3] = { 0.5f, 0.5f, 0.5f }, blue[3] = { 0.65f, 0.65f, 0.8f }, up[3] = { 0, 1, 0 }, one[3] = { 1, 1, 1 };   /* additive: rgb x alpha, no x2 (docs/SPLASH.md 7) */
    if (eye) g_fx_eye = (Vec3){ eye[0], eye[1], eye[2] };
    Vec3 feet = g_player ? g_player->pos : (Vec3){ 0, 0, 0 };
    for (int i = 0; i < g_nfx; i++) {                                        /* 0x470c70 re-reads the bound, so a particle born this frame also draws this frame */
        FxRec *e = &g_fx[i];
        if (e->kind == FX_AMB) { if (ambient_fx_run(&e->amb, dt, eye)) continue; g_fx[i] = g_fx[--g_nfx]; i--; continue; }   /* the callback moves, draws and ages the record itself */
        float u = (e->age += dt) / e->life;
        if (e->kind == 4 && u >= 1.0f) { Vec3 q = drop_pt(e, u, u); fx_new(0.6f, q, 5); e = &g_fx[i]; }   /* 0x478100: a drop that lands leaves a ripple */
        if (u < 1.0f) {
            if (e->kind == 3) {                                              /* 0x478360: the emitter */
                e->acc += dt; e->acc2 += dt;
                if (u < 0.3f) {
                    int n = (int)(e->acc * 500.0f); e->acc -= n * 0.002f;
                    while (n-- > 0) {
                        FxRec *d = fx_new((fx_rnd() + 1.0f) * 0.4f, e->pos, 4); if (!d) continue;
                        int a = (int)(fx_rnd() * 511.0f); float c = cosf(6.2831853f * a / 512.0f), s = sin512(a);
                        d->pos.x = (fx_rnd() * 20.0f + e->R) * c + e->pos.x;     /* separate jitter per axis, not purely radial */
                        d->pos.z = (fx_rnd() * 20.0f + e->R) * s + e->pos.z;
                        d->dir = e->R > 0 ? (Vec3){ c, 0, s } : (Vec3){ 0, 0, 0 };
                        fx_rnd(); fx_rnd();                                      /* +0x24, +0x20: never read */
                        d->h = e->h; d->D = fx_rnd() * 100.0f + 150.0f;          /* 150..250 outward */
                    }
                }
                int n = (int)(e->acc2 * 5.0f); e->acc2 -= n * 0.2f;
                while (n-- > 0) { FxRec *k = fx_new(0.7f, e->pos, 6); if (k) { k->R = e->R; fx_rnd(); } }   /* the rotation rnd*512 does nothing: the ring image is round */
            } else if (e->kind == 4) {                                       /* 0x477fa0: an additive streak, tail alpha 0, head 0.08 further on */
                Vec3 a = drop_pt(e, u, u), b = drop_pt(e, u + 0.08f, u + 0.1f);
                if (eye) hud_world_streak(57, &a.x, &b.x, eye, 4.0f, grey05, 0.0f, 0.65f);
            } else if (e->kind == 5) {                                       /* 0x478290: image 3, byte-identical to image 58 */
                hud_world_fx_plane(0x3a, &e->pos.x, up, u * 25.0f + 5.0f, blue, (1.0f - u) * 0.3f);
            } else if (e->kind == 6) {                                       /* 0x4781b0 */
                hud_world_fx_plane(0x3a, &e->pos.x, up, 2.0f * e->R + 600.0f * u, blue, (1.0f - u) * 0.3f);
            } else if (e->kind == 7) {                                       /* 0x47a8d0: 250 streaks a second for 1.25 s, fire rings at u 0.7 / 0.8 / 0.9 */
                e->acc += dt;
                if (u < 0.5f) {
                    int n = (int)(e->acc * 250.0f); e->acc -= n * 0.004f;
                    while (n-- > 0) {
                        FxRec *s = fx_new(0.5f, feet, 8); if (!s) continue;
                        int a = (int)(fx_rnd() * 512.0f), b = (int)(fx_rnd() * 512.0f);
                        s->dir = (Vec3){ 400.0f * cos512(a) * cos512(b), 400.0f * sin512(a), 400.0f * cos512(a) * sin512(b) };   /* a point on a sphere of 400 */
                    }
                }
                int w = u >= 0.9f ? 4 : u >= 0.8f ? 2 : u >= 0.7f ? 1 : 0;
                if (w && !(e->shape & w)) { fx_new(0.5f, feet, 9); e->shape |= w; }   /* centred on the feet NOW; the ring does not follow him */
            } else if (e->kind == 8) {                                       /* 0x47a790: from the sphere into Perso+0x60 + 100 = feet + 143, re-read every frame */
                Vec3 C = { feet.x, feet.y + 143.0f, feet.z }; float k = 1.0f - u * u;
                Vec3 a = { C.x + e->dir.x, C.y + e->dir.y, C.z + e->dir.z }, b = { C.x + k * e->dir.x, C.y + k * e->dir.y, C.z + k * e->dir.z };
                if (eye) hud_world_streak(0, &a.x, &b.x, eye, 2.0f, one, 0.0f, u <= 0.7f ? 1.0f : 1.0f - (u - 0.7f) * 3.33333f);
            } else if (e->kind == 9) {                                       /* 0x47a4c0: a flat band 800 wide from radius 0 to 4000 in 0.5 s, submitted twice */
                float r0 = 4000.0f * u, r1 = 4000.0f * (u + 0.2f), o = 2.0f * (1.0f - u);
                const float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} }, rgb[4][3] = { {0,0,0}, {0.5f*o,0.5f*o,0.4f*o}, {0.5f*o,0.5f*o,0.4f*o}, {0,0,0} };
                for (int i = 0; i < 16; i++) {
                    int a0 = 32 * i, a1 = a0 + 32;
                    float v[4][3] = { { e->pos.x + r0 * cos512(a0), e->pos.y, e->pos.z + r0 * sin512(a0) }, { e->pos.x + r1 * cos512(a0), e->pos.y, e->pos.z + r1 * sin512(a0) },
                                      { e->pos.x + r1 * cos512(a1), e->pos.y, e->pos.z + r1 * sin512(a1) }, { e->pos.x + r0 * cos512(a1), e->pos.y, e->pos.z + r0 * sin512(a1) } };
                    hud_world_quad(33, v, uv, rgb);
                }
            } else if (e->kind == 10) {                                      /* 0x475040: the flash of a hit */
                hud_world_fx(9, &e->pos.x, 250.0f * u, 0, one, 0.4f);
            } else if (e->kind == 11) {                                      /* 0x474e00: a speed line out to a spinning star */
                int i = (int)(u * 128.0f); float r = 150.0f * sin512(i);
                Vec3 h = { e->pos.x + e->dir.x * r, e->pos.y + e->dir.y * r, e->pos.z + e->dir.z * r };
                if (eye) hud_world_streak(7, &e->pos.x, &h.x, eye, 10.0f, one, 0.0f, 0.1f);
                hud_world_fx(8, &h.x, e->R, (float)(e->shape ? (int)(256.0f * u) : (int)(511.0f - 256.0f * u)) / 512.0f, one, 1.0f - u);
            } else if (e->kind >= FX_STEP_EMIT) {                                   /* docs/PARTICLES.md */
                fx_particle(e, u, dt);
            } else if (e->kind == 2) {                                              /* 0x4791f0: the only thing that draws. The fade in and out is the size, not the alpha */
                float size = 30.0f * sinf(3.14159265f * (int)(255.0f * u) / 256.0f);
                hud_world_fx(4, &e->pos.x, size, (float)(int)(45.0f * u) / 512.0f, grey05, 1.0f);   /* rgb 0.5, alpha 1, flags 7 = additive (0x479249..0x479295): byte a*c*128 = 64 under MODULATE2X = texture x 0.5 (SPLASH.md 7) */
            } else if (e->kind == 0) {                                       /* 0x478f70: a rotating cage of spark sources that shrinks onto the point */
                float M[9]; fx_rotmat((int)(u * 255.5f), (int)(u * 408.8f), (int)(u * 511.0f), M);
                int first = e->shape ? 8 : 0, cnt = e->shape ? 4 : 8, reps;
                e->acc += dt * 60.0f; reps = (int)e->acc; e->acc -= reps;     /* the original emits one set per FRAME; normalised to its 60 Hz so the density does not follow our frame rate */
                for (int r = 0; r < reps; r++) for (int k = 0; k < cnt; k++) {
                    const float *S = k_fx_shape[first + k], s = 1.0f - u;
                    float X = s * S[0] * 40.0f, Y = s * S[1] * 40.0f, Z = s * S[2] * 40.0f;
                    Vec3 p = { e->pos.x + M[0] * X + M[3] * Y + M[6] * Z, e->pos.y + M[1] * X + M[4] * Y + M[7] * Z, e->pos.z + M[2] * X + M[5] * Y + M[8] * Z };
                    if (!fx_new(0.4f, p, 2)) break;
                }
            } else {                                                         /* 0x4792d0: 50 sparks a second in a box over the bonus, each 0.2 s */
                float acc = e->acc + dt; int n = (int)(acc * 50.0f); e->acc = acc - n * 0.02f;
                while (n-- > 0) {
                    Vec3 p = { e->pos.x + fx_rnd() * 60.0f - 30.0f, e->pos.y + (fx_rnd() + 1.0f) * 25.0f, e->pos.z + fx_rnd() * 60.0f - 30.0f };
                    if (!fx_new(0.2f, p, 2)) break;
                }
            }
            continue;
        }
        if (e->kind == FX_SKELETON && g_player && g_player->inst) g_player->inst->fade = g_player->inst->fade_target = e->R;   /* 0x477e06: 0x44e7f0(P, old +0x6c, 0) */
        g_fx[i] = g_fx[--g_nfx]; i--;                                        /* 0x470cf4: swap with the last and look at this slot again */
    }
}

/* ---- the particle effects of docs/PARTICLES.md: footsteps, the smoke ring, the beak impact, explosion debris ------
 * All of them are records in the one effect pool (g_fx = [0x5e823c]+0xdb8, fx_new / fx_update) with their own
 * callback, drawn through the sprite primitive 0x470f10 with the original's flags (hud_world_spr). The numbers
 * below are the exe's (docs/PARTICLES.md cites every constant). */
static Vec3 v3cross(Vec3 a, Vec3 b) { return (Vec3){ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static void fx_axes(Vec3 d, Vec3 *X, Vec3 *Y)                                /* 0x46d320: rows X, Y of the frame whose Z is d */
{
    if (fabsf(d.x) < 0.001f && fabsf(d.z) < 0.001f) {                        /* straight up or down (0x46d375) */
        Vec3 v = { 0, d.z, -d.y }; float l = sqrtf(v.y * v.y + v.z * v.z); if (l > 0) { v.y /= l; v.z /= l; }
        *Y = v; *X = v3cross(v, d);
    } else {                                                                 /* 0x46d417 */
        Vec3 a = { d.z, 0, -d.x }; float l = sqrtf(a.x * a.x + a.z * a.z); if (l > 0) { a.x /= l; a.z /= l; }
        *X = a; *Y = v3cross(d, a);
    }
}
static Vec3 fx_plane_u(Vec3 n)                                               /* 0x471ee0, column 0: the first axis of the plane with normal n */
{
    if (fabsf(n.x) < 0.001f && 1.0f - fabsf(n.y) < 0.001f && fabsf(n.z) < 0.001f) {
        float l = sqrtf(n.y * n.y + n.z * n.z); if (l <= 0) l = 1;
        Vec3 v = { 0, -n.z / l, n.y / l }; return v3cross(v, n);
    }
    float l = sqrtf(n.x * n.x + n.z * n.z); if (l <= 0) l = 1;
    return (Vec3){ n.z / l, 0, -n.x / l };
}
/* 0x47cba0(pos, ground normal, walk direction, foot, kind) from the walk cycle 0x463f40 (docs/FOOTSTEPS.md 1). Kind bit 0
 * leaves a print in the ground for 15 s (kind 3 = dust, sand, snow), kind bit 1 raises the dust puffs (kinds 2 and 3). Foot
 * 0 sits 15 to the left (n x d), foot 1 15 to the right. The step SOUND is not from here (docs/SOUND.md 3). */
void game_footstep(Vec3 pos, Vec3 normal, Vec3 dir, int foot, int kind)
{
    Vec3 s = v3cross(normal, dir); float k = foot ? -15.0f : 15.0f;          /* 0x4abd88 / 0x4a9864 */
    if (kind & 1) {
        FxRec *r = fx_new(15.0f, (Vec3){ pos.x + s.x * k, pos.y + 0.1f + s.y * k, pos.z + s.z * k }, FX_STEP_PRINT);   /* +0.1 = 0x4a9008 */
        if (r) { r->n = normal; r->dir = dir; r->foot = foot; }
    }
    if (kind & 2) { FxRec *r = fx_new(0.2f, pos, FX_STEP_EMIT); if (r) { r->n = normal; r->dir = dir; r->foot = foot; } }   /* 0x47e660 */
    if (wenv("WOODY_FXLOG")) printf("footstep foot %d kind %d at %.0f %.0f %.0f", foot, kind, pos.x, pos.y, pos.z), puts("");
}
/* 0x476140(pos, normal, kind, t0, life): a ring of clouds that spreads out in the plane across `normal`. Table 0x4b7c60
 * per kind: radius, image, cloud size, alpha, alpha blended; kinds 1 and 2 have no caller in the exe. t0 is the age the
 * clouds start at (at most 0.9 life), so a ring born at t0 = 0.25 of 1.5 s is already a sixth of the way out. */
static const struct { float R; int img; float size, alpha; int blend; } k_ring[4] = {
    { 200, 14, 80, 0.5f, 1 }, { 50, 16, 60, 0.05f, 1 }, { 50, 16, 60, 0.025f, 1 }, { 100, 16, 60, 0.1f, 1 } };
void game_smoke_ring(Vec3 pos, Vec3 normal, int kind, float t0, float life)
{
    if (kind < 0 || kind > 3) return;
    float n = k_ring[kind].R * 6.2831853f / (k_ring[kind].size * 0.3f);     /* clouds 0.3 of their size apart round the ring */
    int step = (int)lrintf(512.0f / n), a = 0;                               /* fistp: rounded */
    Vec3 X, Y; fx_axes(normal, &X, &Y);
    if (t0 > life * 0.9f) t0 = life * 0.9f;
    for (int i = 0; (float)i < n; i++, a += step) {                          /* a full pool skips the cloud but not its place in the ring */
        FxRec *r = fx_new(life, pos, FX_RING); if (!r) continue;
        float c = cos512(a), s = sin512(a), q = k_ring[kind].size * 0.25f;
        r->age = t0; r->dir = (Vec3){ X.x * c + Y.x * s, X.y * c + Y.y * s, X.z * c + Y.z * s };
        r->size = fx_rnd() * q * 2.0f - q; r->rot = (i * 5000) & 511; r->shape = kind;
    }
    if (wenv("WOODY_FXLOG")) printf("smoke ring kind %d (%d clouds) at %.0f %.0f %.0f", kind, (int)ceilf(n), pos.x, pos.y, pos.z), puts("");
}
void game_land_dust(Vec3 pos, Vec3 normal) { game_smoke_ring(pos, normal, 3, 0.25f, 1.5f); }   /* 0x464486 */
static void fx_explode(int kind, Vec3 pos, Vec3 n)
{
    if (kind == 1) fx_new(0.2f, pos, FX_BURN_EMIT);                          /* 0x4770bf */
    if (kind == 0 || kind == 1) fx_new(0.2f, pos, FX_DEBRIS_EMIT);           /* 0x47710b / 0x4772d6 */
    if (kind == 0) game_smoke_ring(pos, n, 0, 0.25f, 1.5f);                  /* 0x47733b */
    if (wenv("WOODY_FXLOG")) printf("explosion kind %d at %.0f %.0f %.0f", kind, pos.x, pos.y, pos.z), puts("");
}
/* 0x479c80(kind, point, normal): kind 1 (every hit of the attack probe 0x4575b0) is one yellow flash of 0.05 s; kind 0 (the
 * climb, every 0.3 s) is an emitter of 0.2 s that throws splinters, keeps the flash lit and leaves one hole in the wall. */
void game_peck_fx(int kind, Vec3 pos, const Vec3 *n)
{
    if (kind == 1) fx_new(0.05f, pos, FX_PECK_FLASH);
    else if (kind == 0) { FxRec *r = fx_new(0.2f, pos, FX_PECK_EMIT); if (r) r->n = n ? *n : (Vec3){ 0, 1, 0 }; }
    if (wenv("WOODY_FXLOG")) printf("peck kind %d at %.0f %.0f %.0f", kind, pos.x, pos.y, pos.z), puts("");
}
/* 0x477e40, Kill 2 and 9 (the laser and the lightning, docs/PERSO_DEATH.md 4.2; also the race kill 2 at 0x44c59d): the
 * position argument is never read. The record keeps the Perso's fade (+0x10 = old +0x6c) to give it back at the end, and
 * he turns to face the camera (0x459ff0(M, -view row 2)). */
void game_race_bonus_reset(void)                                             /* 0x44f8a0: every type-37 instance is re-celled (visible again) */
{
    int n = 0;
    for (uint32_t mi = 0; mi < g_ins.nmodels; mi++) for (uint32_t k = 0; k < g_ins.models[mi].ninstances; k++) {
        Instance *ii = &g_ins.models[mi].instances[k]; if (ii->type != 37) continue;
        if (!ii->visible) n++; ii->visible = 1;
    }
    if (n) printf("  RACE %d race bonuses back\n", n);
}
/* the board spray emitter 0x475440 (docs/RACE.md 2.2), ticked by the effect driver 0x46d040 only in the frames the ride marked it
 * (+0xc, cleared after). Per type-9 marker (P0 -> P1) of the board: a trail direction d (P1 - P0 the first time, then the
 * previous position - P0), a camera-facing glow (image 32, 40, alpha 1 - 0.2 sin(pi ph0), turning with ph0), a glow in the plane
 * across d (image 32, 35, ph1), three flame quads round d (image 31, mode 0x13 = 2:1 along d, 45 +- 5, alpha 0.8, 120 degrees
 * apart, spinning at ph2) and 200 puffs a second (0x475380: image 14, 0.2 s, size 15..30, alpha 0.3 (1 - u)) spread over the
 * way back to the previous position. Sizes from the tables 0x4abcc8/cc/d0 at index 3 (45/40/35); boost (mode 3) doubles
 * the glows and makes the flames 1.3x. All additive. */
static void board_frame(Vec3 d, Vec3 *X, Vec3 *Y)                          /* 0x46d320: X = normalize(d.z, 0, -d.x), Y = d x X */
{
    if (fabsf(d.x) < 0.001f && fabsf(d.z) < 0.001f) {
        float l = sqrtf(d.z * d.z + d.y * d.y); *Y = l > 0 ? (Vec3){ 0, d.z / l, -d.y / l } : (Vec3){ 0, 0, 1 };
        *X = (Vec3){ Y->y * d.z - Y->z * d.y, Y->z * d.x - Y->x * d.z, Y->x * d.y - Y->y * d.x };
    } else {
        float l = sqrtf(d.x * d.x + d.z * d.z); *X = (Vec3){ d.z / l, 0, -d.x / l };
        *Y = (Vec3){ d.y * X->z - d.z * X->y, d.z * X->x - d.x * X->z, d.x * X->y - d.y * X->x };
    }
}
/* the glows and flames of one type-9 marker of 0x475440 (0x4756e9..0x475b74), shared by the race board and the rocket: P0 the
 * marker point, d the unit trail direction, size index i into 0x4abcc8 (A = flame [i], B = glow 1 [i + 1], C = glow 2 [i + 2]),
 * state 1 (start-up ramp, scale s), 2 (on) or 3 (boost), ph = the three phases +0x10/+0x14/+0x18. */
static void exhaust_glow_flames(Vec3 P0, Vec3 d, int size_idx, int mode_state, float s, const float ph[3])
{
    static const float tA[10] = { 50, 70, 35, 45, 40, 35, 100, 70, 60, 100 }, one[3] = { 1, 1, 1 };   /* 0x4abcc8 + 4 i: A = [i], B = [i + 1], C = [i + 2] */
    float A = tA[size_idx], B = tA[size_idx + 1], C = tA[size_idx + 2];
    Vec3 X, Y; board_frame(d, &X, &Y);
    float p[3] = { P0.x, P0.y, P0.z }, nrm[3] = { d.x, d.y, d.z };
    float sz = mode_state == 1 ? s * B : mode_state == 3 ? 2 * B : B;
    hud_world_spr(32, p, sz, 512 - (int)(ph[0] * 512.0f), one, 1.0f - 0.2f * sin512((int)(ph[0] * 256.0f)), 7, NULL, 0);   /* camera facing */
    sz = mode_state == 1 ? s * C : mode_state == 3 ? 2 * C : C;
    hud_world_spr(32, p, sz, (int)(ph[1] * 512.0f), one, 1.0f - 0.2f * sin512((int)(ph[1] * 255.0f)), 6, nrm, 0);    /* across the trail */
    float r = fx_rnd() * 10.0f - 5.0f; int mode = mode_state == 3 ? 0x12 : 0x13;
    sz = (mode_state == 1 ? s * A : mode_state == 3 ? 1.3f * A : A) + r;     /* 0x4758b8: the ramp scales the table size, the +-5 comes on top */
    float half = sz * cos512(mode == 0x12 ? 64 : 37) - sz * 0.015625f;  /* [0x5e823c]+0x800[mode]: the flame starts at P0 and trails back along d */
    float q[3] = { P0.x + d.x * half, P0.y + d.y * half, P0.z + d.z * half };
    for (int j = 0; j < 0xff; j += 0x55) {
        int a1 = (int)(ph[2] * 512.0f + (float)j) & 511;                     /* 0x4759cd: ftol(f3 * 512 + k) */
        float basis[6] = { d.x, d.y, d.z, X.x * cos512(a1) + Y.x * sin512(a1), X.y * cos512(a1) + Y.y * sin512(a1), X.z * cos512(a1) + Y.z * sin512(a1) };   /* R = d, F round d */
        hud_world_spr_mode(mode, 31, q, sz, 0, one, 0.8f, 0x62, basis, 2);
    }
}
static void board_fx_draw(float dt)
{
    if (!g_player || !g_player->bfx.inst) return;
    BoardFx *e = &g_player->bfx;
    if (!e->active) return;
    e->active = 0;                                                           /* 0x46d0b0 */
    static const float tA[10] = { 50, 70, 35, 45, 40, 35, 100, 70, 60, 100 };   /* 0x4abcc8: A = the flame size, also the puffs' start offset */
    float A = tA[e->size_idx];
    const float rates[3] = { 0.05f, 0.15f, 3.0f };                          /* 0x4aab4c, 0x4aa1c8, 0x4a988c */
    for (int k = 0; k < 3; k++) { e->ph[k] += dt * rates[k]; if (e->ph[k] >= 1.0f) e->ph[k] -= 1.0f; }
    if (e->mode) e->acc += dt;
    int n = (int)(e->acc * 200.0f); e->acc -= n * 0.005f;                   /* 0x4aa164 */
    if (e->mode == 1) { e->t1c += dt; if (e->t1c >= 1.0f) { e->mode = 2; e->t1c = 0; } }
    if (e->mode == 0) return;
    float s = 0;                                                             /* mode 1 (not used by the race): three 0.15 s ramps at 0, 0.3, 0.85 */
    if (e->mode == 1) { float t = e->t1c; s = t > 0 && t < 0.15f ? t * 6.6666665f : t > 0.3f && t < 0.45f ? (t - 0.3f) * 6.6666665f : t > 0.85f && t < 1.0f ? (t - 0.85f) * 6.6666665f : 0; }
    for (int i = 0; i < e->n; i++) {
        Vec3 P0, D; if (!inst_vector_at(e->inst, 9, (uint32_t)i, &P0, &D)) continue;
        Vec3 d = e->has_prev[i] ? (Vec3){ e->prev[i].x - P0.x, e->prev[i].y - P0.y, e->prev[i].z - P0.z } : D;
        e->has_prev[i] = 1;
        float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z); if (len > 0) { d.x /= len; d.y /= len; d.z /= len; }
        exhaust_glow_flames(P0, d, e->size_idx, e->mode, s, e->ph);
        for (int k = n - 1; k >= 0; k--) {                                   /* the puffs, spread from P0 + 45 d back towards the previous position */
            float f = (float)k / (float)n, dist = f * len + A;
            FxRec *pf = fx_new(0.2f, (Vec3){ P0.x + d.x * dist, P0.y + d.y * dist, P0.z + d.z * dist }, FX_BOARD_PUFF); if (!pf) continue;
            pf->age = k * dt / n; pf->rot = (int)(fx_rnd() * 512.0f);
            e->prev[i] = e->size_idx == 9 || e->size_idx == 6 ? (Vec3){ P0.x + D.x, P0.y + D.y, P0.z + D.z } : pf->pos;   /* 0x475c2c */
        }
    }
}
void game_skeleton(void)
{
    Player *p = g_player; if (!p || !p->inst) return;
    FxRec *r = fx_new(1.5f, p->pos, FX_SKELETON); if (!r) return;
    r->R = p->inst->fade; r->shape = g_char == 0; r->dir = (Vec3){ p->pos.x, p->pos.y + 96.0f, p->pos.z };
    if (fabsf(g_fx_fwd.x) + fabsf(g_fx_fwd.z) > 1e-4f) p->yaw = atan2f(-g_fx_fwd.x, -g_fx_fwd.z);
    if (wenv("WOODY_FXLOG")) printf("skeleton flash at %.0f %.0f %.0f", p->pos.x, p->pos.y, p->pos.z), puts("");
}
static void fx_particle(FxRec *e, float u, float dt)
{
    static const float one[3] = { 1, 1, 1 }, half[3] = { 0.5f, 0.5f, 0.5f }, hole[3] = { 0.8f, 0.8f, 0 }, grey8[3] = { 0.8f, 0.8f, 0.8f };
    switch (e->kind) {
    case FX_STEP_EMIT: {                                                     /* 0x47e460: 50 puffs a second beside the foot */
        Vec3 A = v3cross(e->n, e->dir);
        e->acc += dt; int n = (int)(e->acc * 50.0f); e->acc -= n * 0.02f;
        while (n-- > 0) {
            FxRec *p = fx_new(0.8f, e->pos, FX_STEP_PUFF); if (!p) continue;
            p->age = fx_rnd() * 0.2f;
            float s = e->foot == 0 ? fx_rnd() * 15.0f + 5.0f : -5.0f - fx_rnd() * 15.0f;
            p->pos = (Vec3){ e->pos.x + A.x * s, e->pos.y + A.y * s, e->pos.z + A.z * s };
            p->dir = (Vec3){ -e->dir.x, -e->dir.y, -e->dir.z };             /* they drift back where he came from */
            p->rot = (int)(fx_rnd() * 512.0f);
        }
        break; }
    case FX_STEP_PUFF: {                                                     /* 0x47e370: 100 back and 30 up over 0.8 s, faint and additive */
        float pos[3] = { e->pos.x + e->dir.x * u * 100.0f, e->pos.y + e->dir.y * u * 100.0f + u * 30.0f, e->pos.z + e->dir.z * u * 100.0f };
        hud_world_spr(14, pos, u * 15.0f + 20.0f, e->rot, one, (1.0f - u) * 0.1f, 7, NULL, 0);
        break; }
    case FX_STEP_PRINT: {                                                    /* 0x47c9a0: an oval pressed in (68, additive) with its rim shadow (69) */
        Vec3 R = v3cross(e->dir, e->n), F = v3cross(e->n, R);               /* S+0x23c, S+0x248: +v along the walk */
        float B[6] = { R.x, R.y, R.z, F.x, F.y, F.z }, size = g_char == 0 ? 40.0f : 20.0f;   /* 0x40c350: Perso subtype 1 (Woody) 40, the others 20 */
        hud_world_spr(68, &e->pos.x, size, 0, half, (1.0f - u) * 0.1f, 0x22, B, 0);
        hud_world_spr(69, &e->pos.x, size, 0, half, (1.0f - u) * 0.3f, 0x6a, B, 1);
        break; }
    case FX_RING: {                                                          /* 0x475fb0: out along its spoke on a quarter sine, fading as it goes */
        float S = sin512((int)(u * 128.0f));
        float R = k_ring[e->shape].R * S, pos[3] = { e->pos.x + e->dir.x * R, e->pos.y + e->dir.y * R, e->pos.z + e->dir.z * R };
        hud_world_spr(k_ring[e->shape].img, pos, (u + 1.0f) * k_ring[e->shape].size * 0.5f + e->size, e->rot, one,
                      (1.0f - S) * k_ring[e->shape].alpha, k_ring[e->shape].blend ? 0xf : 7, NULL, 0);
        break; }
    case FX_PECK_EMIT: {                                                     /* 0x4798f0: 33 splinters, 50 flashes and 8 holes a second */
        e->acc += dt; e->acc2 += dt; e->acc3 += dt;
        int n = (int)(e->acc * 33.0f); e->acc -= n * 0.030303f;
        while (n-- > 0) {
            FxRec *c = fx_new((fx_rnd() + 1.0f) * 0.3f, e->pos, FX_PECK_CHIP); if (!c) continue;
            int a = (int)(fx_rnd() * 511.0f);
            c->pos = (Vec3){ e->pos.x + cos512(a) * 10.0f, e->pos.y, e->pos.z + sin512(a) * 10.0f };
            c->dir = (Vec3){ cos512(a), 0, sin512(a) };                      /* level and outward, whatever the wall */
            c->rot = (int)(fx_rnd() * 511.0f);
        }
        n = (int)(e->acc2 * 50.0f); e->acc2 -= n * 0.02f;
        while (n-- > 0) fx_new(0.05f, e->pos, FX_PECK_FLASH);
        n = (int)(e->acc3 * 8.0f); e->acc3 -= n * 0.125f;                    /* 0.2 s at 8 a second: one hole per peck */
        while (n-- > 0) {
            float hold = fx_rnd() * 0.3f + 5.0f, v = fx_rnd() * 20.0f - 10.0f; Vec3 U = fx_plane_u(e->n);
            FxRec *h = fx_new(hold + 4.0f, (Vec3){ e->pos.x + U.x * v, e->pos.y + U.y * v, e->pos.z + U.z * v }, FX_PECK_HOLE); if (!h) continue;
            h->D = hold; h->n = e->n; h->rot = (int)(fx_rnd() * 511.0f);
        }
        break; }
    case FX_PECK_CHIP: {                                                     /* 0x479670: a splinter on an arc 80 out and 100 high */
        float pos[3] = { e->pos.x + u * e->dir.x * 80.0f, e->pos.y + sin512((int)(u * 255.0f)) * 100.0f, e->pos.z + u * e->dir.z * 80.0f };
        /* the original never sets the rotation here and draws with whatever S+0x224 the previous sprite left; the port
         * takes the chip's own rolled angle (+0x20) */
        hud_world_spr(25, pos, fx_rnd() * 5.0f + 10.0f, e->rot, one, 1.0f, 0xf, NULL, 0);
        break; }
    case FX_PECK_FLASH:                                                      /* 0x479760 */
        hud_world_spr(18, &e->pos.x, 80.0f, 0, one, 0.8f, 3, NULL, 0);
        break;
    case FX_BOARD_PUFF:                                                      /* 0x475380: the race board's spray, image 14, flags 7 (additive) */
        hud_world_spr(14, &e->pos.x, (u + 1.0f) * 15.0f, e->rot, one, 0.3f - 0.3f * u, 7, NULL, 0);
        break;
    case FX_PECK_HOLE: {                                                     /* 0x479800: 5 to 5.3 s in the wall, then 4 s to fade */
        float a = e->age < e->D ? 1.0f : 1.0f - (e->age - e->D) * 0.25f;
        hud_world_spr(26, &e->pos.x, 15.0f, e->rot, hole, a, 0xe, &e->n.x, 0);
        break; }
    case FX_DEBRIS_EMIT: {                                                   /* 0x476cd0: 400 clouds a second, 0.7 s each */
        e->acc += dt; int n = (int)(e->acc * 400.0f); e->acc -= n * 0.0025f;
        while (n-- > 0) {
            FxRec *d = fx_new(0.7f, e->pos, FX_DEBRIS); if (!d) continue;
            d->size = fx_rnd() * 50.0f + 80.0f;
            d->foot = (int)(fx_rnd() * 2.0f) != 0 ? 16 : 17;                 /* the image */
            d->rot = (int)(fx_rnd() * 511.0f);
            int a = (int)(fx_rnd() * 255.0f), b = (int)(fx_rnd() * 511.0f); float r = fx_rnd() * 150.0f + 20.0f;
            if (u < 0.8f) { d->D = 600.0f; d->dir = (Vec3){ cos512(a), sin512(a), cos512(a) * sin512(b) }; }   /* the upper half, not normalised */
            else { d->D = 40.0f; d->dir = (Vec3){ 0, 1, 0 }; }             /* the last fifth only rises */
            d->pos = (Vec3){ e->pos.x + cos512(a) * r, e->pos.y + sin512(a) * r, e->pos.z + cos512(a) * sin512(b) * r };
        }
        break; }
    case FX_DEBRIS: {                                                        /* 0x4764f0: D * u * cos(u pi/4) along its direction */
        float s = cos512((int)(u * 64.0f + 128.0f) + 128) * -e->D * u;
        float pos[3] = { e->pos.x + e->dir.x * s, e->pos.y + e->dir.y * s, e->pos.z + e->dir.z * s };
        hud_world_spr(e->foot, pos, e->size, e->rot, one, (1.0f - u) * 0.2f, 0xf, NULL, 0);
        break; }
    case FX_BURN_EMIT: {                                                     /* 0x476b50: 60 burning pieces a second */
        e->acc += dt; int n = (int)(e->acc * 60.0f); e->acc -= n * 0.0166667f;
        while (n-- > 0) {
            FxRec *b = fx_new(2.0f, e->pos, FX_BURN); if (!b) continue;
            Vec3 d = { fx_rnd() * 2.0f - 1.0f, 0, 0 }; d.y = fx_rnd() * 2.0f - 0.5f; d.z = fx_rnd() * 2.0f - 1.0f;
            float l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z); if (l > 0) { d.x /= l; d.y /= l; d.z /= l; }
            b->dir = d; b->pos = (Vec3){ e->pos.x + d.x * 300.0f, e->pos.y + d.y * 300.0f, e->pos.z + d.z * 300.0f };
        }
        break; }
    case FX_BURN: {                                                          /* 0x4767f0: 1200 u/s, bent 0.2 down every 0.1 s, a smoke puff and a spark every 0.025 s */
        e->acc += dt;                                                        /* +0x20: time not yet laid out; e->h = +0x28, the time along this 0.1 s leg */
        if (e->acc > 0.025f) {
            do {
                e->h += 0.025f;
                Vec3 p = { e->pos.x + e->dir.x * e->h * 1200.0f, e->pos.y + e->dir.y * e->h * 1200.0f, e->pos.z + e->dir.z * e->h * 1200.0f };
                FxRec *k = fx_new(0.2f, p, FX_TRAIL_SMOKE);
                if (k) { k->size = fx_rnd() * 20.0f + 40.0f; k->R = 0.3f; k->rot = (int)(fx_rnd() * 512.0f); }
                k = fx_new(0.15f, p, FX_TRAIL_SPARK);
                if (k) { k->size = fx_rnd() * 20.0f + 60.0f; k->R = 1.0f; k->rot = (int)(fx_rnd() * 512.0f); }
                if (e->h >= 0.1f) {                                          /* the next leg starts at the end of this one */
                    e->pos.x += e->dir.x * 120.0f; e->pos.y += e->dir.y * 120.0f; e->pos.z += e->dir.z * 120.0f;
                    e->dir.y -= 0.2f; float l = sqrtf(e->dir.x * e->dir.x + e->dir.y * e->dir.y + e->dir.z * e->dir.z);
                    if (l > 0) { e->dir.x /= l; e->dir.y /= l; e->dir.z /= l; }
                    e->h = 0;
                }
                e->acc -= 0.025f;
            } while (e->acc > 0.025f);
        }
        float t = (e->h + e->acc) * 1200.0f, pos[3] = { e->pos.x + e->dir.x * t, e->pos.y + e->dir.y * t, e->pos.z + e->dir.z * t };
        hud_world_spr(12, pos, 40.0f, 0, grey8, 1.0f, 3, NULL, 0);           /* the burning head */
        break; }
    case FX_TRAIL_SMOKE:                                                     /* 0x476f00: white smoke (image 15), alpha blended */
        hud_world_spr(15, &e->pos.x, e->size, e->rot, one, (1.0f - u) * e->R, 0xf, NULL, 0);
        break;
    case FX_TRAIL_SPARK:                                                     /* 0x476fb0: the fire in it (image 13), additive */
        hud_world_spr(13, &e->pos.x, e->size, e->rot, one, (1.0f - u) * e->R, 7, NULL, 0);
        break;
    case FX_SKELETON: {                                                      /* 0x477980: the model and a sprite skeleton take turns, in eighths of 1.5 s */
        static const float off[18][3] = {                                    /* 0x4b7d10: head, arm, arm, ribcage, leg, leg; rows 0 (pose B) and 2 (pose A) */
            { -5, 180, 0 }, { -60, 100, 0 }, { 60, 150, 0 }, { 0, 96, 0 }, { -40, 41, 0 }, { 40, 46, 0 },
            { -5, 180, 0 }, { -60, 150, 0 }, { 60, 150, 0 }, { 0, 96, 0 }, { -40, 41, 0 }, { 40, 46, 0 },
            { 10, 180, 0 }, { -60, 150, 0 }, { 60, 100, 0 }, { 0, 96, 0 }, { -40, 41, 0 }, { 40, 46, 0 } };
        static const int mir[18] = { 0, 1, 2, 0, 0, 2, 0, 0, 2, 0, 0, 2, 2, 0, 3, 0, 0, 2 };   /* 0x4b7cb0: UV sets per bone, rows 0 and 12 */
        static const int img[2][12] = { { 0x2a, 0x22, 0x22, 0x23, 0x24, 0x24, 0x2b, 0x26, 0x26, 0x27, 0x28, 0x28 },   /* 0x4b7e60: Knothead, Splinter */
                                        { 0x25, 0x22, 0x22, 0x23, 0x24, 0x24, 0x29, 0x26, 0x26, 0x27, 0x28, 0x28 } }; /* 0x4b7e30: Woody (subtype 1) */
        static const float white255[3] = { 255, 255, 255 };
        Player *pl = g_player; if (!pl || !pl->inst) break;
        int ph = (u < 0.125f || (u > 0.25f && u < 0.375f)) ? 1 : ((u > 0.5f && u < 0.625f) || (u > 0.75f && u < 0.875f)) ? 2 : 0;
        pl->inst->fade = pl->inst->fade_target = ph ? 1.0f : 0.0f;          /* 0x44e7f0(P, 1 or 0, 0) with +0x100 = 100/s: at once */
        if (ph) {
            Vec3 P = pl->pos, d = { g_fx_eye.x - P.x, g_fx_eye.y - P.y, g_fx_eye.z - P.z }, X, Y;   /* towards the camera, 3D */
            float l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z); if (l > 0) { d.x /= l; d.y /= l; d.z /= l; }
            fx_axes(d, &X, &Y);
            float jit = fx_rnd() * 20.0f - 10.0f;                            /* one per frame for all six glows */
            int row = ph == 1 ? 12 : 0, head = ph == 1 ? 40 : 472;           /* A: head tilted 40/512 one way, B: the other */
            for (int i = 0; i < 6; i++) {
                const float *o = off[row + i];
                float pos[3] = { P.x + X.x * o[0] + Y.x * o[1] + d.x * o[2], P.y + X.y * o[0] + Y.y * o[1] + d.y * o[2], P.z + X.z * o[0] + Y.z * o[1] + d.z * o[2] };
                float size = i == 0 ? 120.0f : i == 3 ? 75.0f : 55.0f; int mode = i == 3 ? 0x1a : 0x12, rot = i == 0 ? head : 0;
                hud_world_spr_mode(mode, img[e->shape][i], pos, size, rot, NULL, 1.0f, 0x4d, NULL, mir[row + i]);          /* the bone, alpha blended */
                hud_world_spr_mode(mode, img[e->shape][6 + i], pos, size + jit, rot, NULL, 1.0f, 0x45, NULL, mir[row + i]); /* its flickering glow, additive */
            }
        }
        /* 0x477d9d..0x477dc6: 0x498790(kind 0, S+0x208, white, rnd * 100 + 200) every frame, never drawn by the original (LIGHTING.md 7).
         * S+0x208 is the position of the shared sprite object, so in a skeleton phase the last bone's glow (the right leg) and
         * in a model phase whatever effect drew a sprite last - this frame or an earlier one */
        { float lp[3]; hud_last_sprite_pos(lp); rnd_light_add(0, (Vec3){ lp[0], lp[1], lp[2] }, white255, fx_rnd() * 100.0f + 200.0f); }
        break; }
    case FX_FLAME: {                                                         /* 0x47cd00: rises 40 a second, shrinks to nothing, white -> red, additive */
        e->pos.y += dt * 40.0f;                                              /* +0xc += dt * 0x4ab294 */
        float pos[3] = { e->pos.x + fx_rnd() * u, e->pos.y, e->pos.z + fx_rnd() * u }, rgb[3] = { 0.5f, 0.5f - u * 0.5f, 0.5f - u * 0.5f };
        hud_world_spr(12, pos, (1.0f - u) * e->R, 0, rgb, 1.0f, 3, NULL, 0);  /* image 0x1000c, mode 0x12, flags 3: camera facing, own colour */
        break; }
    }
}

/* the visuals of a projectile: the bolt of visual 2 (0x46f8a0) and the missile of visual 0/1 (0x4700e0). Both are a
 * head sprite with a ribbon behind it, and the missile has the model and its exhaust on top of that (0x46fb30).
 * The fireball of visual 3 (0x470420) has no ribbon on screen: two dim image-12 sprites spinning against each other
 * and a trail of image-13 sparks, 75 a second, left behind where it flew (docs/PROJECTILES.md 5.5).
 * The ribbon is sampled from the path the projectile really flew (its points are pushed every fx+0x3c seconds), so the
 * trail of a homing or bouncing shot curves and kinks with it. */
/* 0x470420 while the projectile lives: the head and the sparks. t = the record's age (fx+0), the same clock as s->age. */
static void fireball_draw(Shot *s, float dt)
{
    static const float grey[3] = { 0.5f, 0.5f, 0.5f };
    int r = (int)(s->age * 511.0f) % 511;                                          /* 0x47080d: (int)(t * 511) % 511, the second one 511 minus that */
    hud_world_fx(12, &s->pos.x, 140.0f, r / 512.0f, grey, 0.7f);                   /* flags 5: additive, so 0.5 * 0.7 of the texture */
    hud_world_fx(12, &s->pos.x, 70.0f, (511 - r) / 512.0f, grey, 0.7f);
    if (dt <= 0) { s->fb_prev = s->pos; return; }
    /* 0x470863: acc += dt, n = (int)(acc * 100), acc -= n * 0.0133 -> 75 sparks a second on average, spread over the way
     * flown since the previous frame, back from the head along the current direction, each axis +-10 at random */
    s->fb_acc += dt; int n = (int)(s->fb_acc * 100.0f); s->fb_acc -= n * 0.0133f;
    float dx = s->fb_prev.x - s->pos.x, dy = s->fb_prev.y - s->pos.y, dz = s->fb_prev.z - s->pos.z, dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (n > 32) n = 32;
    for (int k = 0; k < n; k++) {
        float f = (float)k / n; Spark *p = &g_sparks[g_spark_next++ % 256];
        p->pos.x = s->pos.x - f * s->dir.x * dist + (float)msvc_rand(NULL) / 32767.0f * 20.0f - 10.0f;
        p->pos.y = s->pos.y - f * s->dir.y * dist + (float)msvc_rand(NULL) / 32767.0f * 20.0f - 10.0f;
        p->pos.z = s->pos.z - f * s->dir.z * dist + (float)msvc_rand(NULL) / 32767.0f * 20.0f - 10.0f;
        p->rot = (float)(int)((float)msvc_rand(NULL) / 32767.0f * 512.0f) / 512.0f; p->t = 1e-4f;
    }
    s->fb_prev = s->pos;
}
static void launchers_draw(const float *eye, float dt)
{
    static const float white[3] = { 1, 1, 1 };
    for (int i = 0; i < 200; i++) {
        Shot *s = &g_shots[i]; if (!s->active || s->visual >= 4) continue;
        if (s->visual == 3) { if (s->dying <= 0) fireball_draw(s, dt); continue; }
        int mis = shot_is_missile(s), nseg = mis ? 20 : 10;                         /* 500 in 20 resp. 400 in 10 (0x4a9998 / 0x4a964c) */
        float span = mis ? 500.0f : 400.0f, hw = mis ? 7.0f : 30.0f;
        float t = fmodf(s->age, 2.0f), k = s->dying > 0 ? s->dying / shot_fade_len(s) : 1.0f;
        float seg = s->T.speed > 0 ? span / (s->T.speed * nseg) * k : 0;           /* fx+0x10 in seconds of flight; it shrinks by dt * 3 / nseg once the projectile is gone */
        for (int j = 0; j < nseg && seg > 0.0001f; j++) {                          /* sampled along the path actually flown (0x46d0d0); before the muzzle it stays on the muzzle */
            float u0 = (float)j / nseg, u1 = (float)(j + 1) / nseg, c0 = cosf(u0 * 1.5707963f), c1 = cosf(u1 * 1.5707963f);
            float ca[3], cb[3];
            if (mis) { for (int q = 0; q < 3; q++) { ca[q] = (1 - u0) * c0; cb[q] = (1 - u1) * c1; } }   /* rgb = 1 - u, alpha = cos(u*pi/2) */
            else { ca[0] = 0.25f * (1 - u0) * c0; ca[1] = 0.4f * (1 - u0) * c0; ca[2] = 0.45f * c0; cb[0] = 0.25f * (1 - u1) * c1; cb[1] = 0.4f * (1 - u1) * c1; cb[2] = 0.45f * c1; }
            Vec3 a = shot_hist_at(s, s->age - seg * j), b = shot_hist_at(s, s->age - seg * (j + 1));
            hud_world_ribbon(&a.x, &b.x, eye, hw, ca, cb);
        }
        if (s->dying > 0) continue;
        if (mis) {                                                                  /* head 0x46fbfa: on the nose, 55 in front of the projectile point */
            static const float flame[3] = { 1.0f, 0.58f, 0 };
            float hp[3] = { s->pos.x + s->dir.x * 55.0f, s->pos.y + s->dir.y * 55.0f, s->pos.z + s->dir.z * 55.0f };
            hud_world_fx(4, hp, 40.0f, t * 255.5f / 512.0f, flame, 1.0f - (t - floorf(t)));
            if (s->missile) missile_exhaust(s->missile, dt);
        } else {
            hud_world_fx(6, &s->pos.x, 50.0f, t * 255.5f / 512.0f, white, 1.0f);
            hud_world_fx(6, &s->pos.x, 80.0f, (1.0f - t * 0.5f) * 511.0f / 512.0f, white, 0.5f);
        }
    }
    for (int i = 0; i < 256; i++) if (g_sparks[i].t > 0) {                          /* 0x470370: image 13, size 50, white, alpha 1 - u, 0.4 s, where it was left */
        Spark *p = &g_sparks[i]; float u = p->t / 0.4f;
        hud_world_fx(13, &p->pos.x, 50.0f, p->rot, white, 1.0f - u); if ((p->t += dt) >= 0.4f) p->t = 0; }
    for (int i = 0; i < 64; i++) if (g_flashes[i].t > 0) {                          /* muzzle / end flash, 0.4 s */
        float t = g_flashes[i].t, u = t / 0.4f, size = 200.0f * cosf(u * 1.5707963f);
        if (g_flashes[i].kind) { hud_world_fx(32, &g_flashes[i].pos.x, size, (1.0f - t * 0.5f), white, 0.5f - 0.5f * u); continue; }   /* 0x46fa40: one image-32 flash at the muzzle */
        if (g_flashes[i].kind == 2) { hud_world_fx(32, &g_flashes[i].pos.x, 80.0f, t, white, 1.0f - t); continue; }   /* 0x4702b0: 1 s, size 80, alpha 1 - u, one turn in that second */
        hud_world_fx(6, &g_flashes[i].pos.x, size, t * 256.0f / 512.0f, white, 1.0f - u);
        hud_world_fx(4, &g_flashes[i].pos.x, size, (1.0f - t * 0.5f) * 512.0f / 512.0f, white, 0.5f - 0.5f * u);
    }
}
/* ---- rideable rocket, class 20 (docs/ROCKET.md): not steered. Message 40 seats the player, the rocket turns 2 s towards point 0 of
 * its own trajectory, ignites 0.3 s, flies a straight line (2000 u/s^2 up to vmax, no collision at all) and explodes after fly_time
 * seconds (blast 600: Kill(6) for a rider who stayed on); the last second it blinks red. 0.5 s later it is back at its start and fades
 * in over 1 s. Class 21 is the bomb cannon of W2x (ROCKET.md 7, BOMB.md 7): the same machine, but at the end of the ignition it
 * fires a class-40 bomb along its own marker (no gravity, speed vmax, fuse fly_time) and the rider sits on that bomb; no
 * exhaust, no flight of its own, no blink, and after the flight time it turns back to its start in 2 s (state 8). */
typedef struct { Instance *inst; int type, state, exhaust, has_prev; float t, speed, fly_time, vmax, ex_t, f1, f2, f3, puff_acc; Vec3 start_pos, prev_p1; Quat start_q, q0, q1; Bomb *bomb; } Rocket;
static Rocket g_rockets[8]; static int g_nrockets;
static Rocket *rocket_of(const Instance *in) { for (int i = 0; i < g_nrockets; i++) if (g_rockets[i].inst == in) return &g_rockets[i]; return NULL; }
static void rocket_place(Rocket *r) { Instance *in = r->inst; mat4_from_trs(&in->world, in->position, in->quat, in->scale); ins_pose(in, in->anim, in->anim_time); }
static void rocket_reset(Rocket *r)                                                /* vtbl[17] 0x452ae0 */
{
    Instance *in = r->inst; if (r->state == 5 || r->state == 6) audio_fx_stop(11, in, 1); if (r->state == 3) audio_fx_stop(15, in, 1);
    in->position = r->start_pos; in->quat = r->start_q; r->state = 0; in->noncollide = 0; in->fade_rate = 1.0f; in->fade_target = 0; in->tint_red = 0;
    /* type 21: bom->vtbl[17]() on +0x168. The original resets whatever bomb that pointer holds now, even one a launcher has taken
     * from the pool since (BOMB.md 7); the port only resets it while it is still the ridden one */
    if (r->bomb) { if (r->bomb->ridden) bomb_reset(r->bomb); r->bomb = NULL; }
    r->has_prev = 0;                                                               /* 0x452baa: only the exhaust's per-marker "has previous" bytes +0x2c[]; its
                                                                                    * state (+8) stays, so the next ride burns at full size from mounting on until
                                                                                    * the ignition sets it back to 1 (ROCKET.md 10.6) */
    rocket_place(r);
}
static Quat quat_from_axes(Vec3 X, Vec3 Y, Vec3 Z)                                 /* images of the model axes = columns of the rotation */
{
    float m00 = X.x, m10 = X.y, m20 = X.z, m01 = Y.x, m11 = Y.y, m21 = Y.z, m02 = Z.x, m12 = Z.y, m22 = Z.z, t = m00 + m11 + m22; Quat q;
    if (t > 0) { float s = sqrtf(t + 1) * 2; q = (Quat){ (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, s * 0.25f }; }
    else if (m00 > m11 && m00 > m22) { float s = sqrtf(1 + m00 - m11 - m22) * 2; q = (Quat){ s * 0.25f, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s }; }
    else if (m11 > m22) { float s = sqrtf(1 + m11 - m00 - m22) * 2; q = (Quat){ (m01 + m10) / s, s * 0.25f, (m12 + m21) / s, (m02 - m20) / s }; }
    else { float s = sqrtf(1 + m22 - m00 - m11) * 2; q = (Quat){ (m02 + m20) / s, (m12 + m21) / s, s * 0.25f, (m10 - m01) / s }; }
    return q;
}
static Vec3 rocket_aim(const Rocket *r, Vec3 from)
{
    Vec3 d = { 0, 0, 1 }; if (r->inst->traj.npoints) { Vec3 p = r->inst->traj.points[0]; d = (Vec3){ p.x - from.x, p.y - from.y, p.z - from.z }; }
    float l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z); if (l > 1e-6f) { d.x /= l; d.y /= l; d.z /= l; } return d;
}
static void rocket_fly(Rocket *r, float dt)                                        /* 0x452cc0 */
{
    Vec3 d = rocket_aim(r, r->start_pos); r->speed += dt * 2000.0f; if (r->speed > r->vmax) r->speed = r->vmax;
    Instance *in = r->inst; in->position.x += d.x * r->speed * dt; in->position.y += d.y * r->speed * dt; in->position.z += d.z * r->speed * dt;
}
static void rockets_update(float dt, Player *pl, int have_player)                  /* think step 0x452e10 */
{
    for (int i = 0; i < g_nrockets; i++) {
        Rocket *r = &g_rockets[i]; Instance *in = r->inst;
        if (in->fade != in->fade_target) { float st = in->fade_rate * dt; in->fade = in->fade < in->fade_target ? (in->fade + st > in->fade_target ? in->fade_target : in->fade + st) : (in->fade - st < in->fade_target ? in->fade_target : in->fade - st); }
        if (!r->state) continue;
        in->tint_red = 0;
        switch (r->state) {
        case 1: r->t += dt; if (r->t >= 0.83f) r->state = 2; break;                /* Woody climbs on */
        case 2: { r->speed = 0; r->t = 0;                                          /* turn target 0x452ebf: model -Y = flight direction, model +Z = up */
            Vec3 f = rocket_aim(r, in->position), Y = { -f.x, -f.y, -f.z }; float k = Y.y;
            Vec3 Z = { -Y.x * k, 1 - Y.y * k, -Y.z * k }; float l = sqrtf(Z.x * Z.x + Z.y * Z.y + Z.z * Z.z); if (l < 1e-5f) { Z = (Vec3){ 0, 0, 1 }; l = 1; } Z.x /= l; Z.y /= l; Z.z /= l;
            Vec3 X = { Y.y * Z.z - Y.z * Z.y, Y.z * Z.x - Y.x * Z.z, Y.x * Z.y - Y.y * Z.x };
            r->q0 = in->quat; r->q1 = quat_from_axes(X, Y, Z); r->state = 3; audio_fx(15, in, &in->position.x); break; }
        case 3: r->t += dt; if (r->t > 2.0f) r->t = 2.0f; in->quat = q_slerp(r->q0, r->q1, r->t / 2.0f);
            if (r->t >= 2.0f) { r->t = 0; r->state = 4; audio_fx_stop(15, in, 1); audio_fx(16, in, &in->position.x); if (r->type == 20) { r->exhaust = 1; r->ex_t = 0; } } break;
        case 4: r->t += dt; if (r->t < 0.3f) break; r->t = 0; r->state = 5;
            if (r->type == 20) { audio_fx(10, in, &in->position.x); audio_fx(11, in, &in->position.x); break; }
            { Vec3 m0, md; if (!inst_vector(in, 0, &m0, &md)) { m0 = in->position; md = rocket_aim(r, in->position); }   /* 0x453279: template 0 without gravity */
              float l = sqrtf(md.x * md.x + md.y * md.y + md.z * md.z); if (l > 1e-6f) { md.x /= l; md.y /= l; md.z /= l; }
              BombT t = BOMB_T0; t.gravity = 0; t.speed = r->vmax; t.damp_a = 1.0f; t.life = r->fly_time;
              r->bomb = bomb_start(&t, m0, md, 0, -1, 1); if (r->bomb) r->bomb->ridden = 1;   /* the original crashes without a free bomb */
              audio_fx(14, in, &in->position.x); }
            break;
        case 5: if (r->type == 20) rocket_fly(r, dt); r->t += dt; if (r->t >= r->fly_time - 1.0f) { r->state = 6; r->t = 0; } break;
        case 6: r->t += dt;
            if (r->type == 21) { if (r->t >= 1.0f) { r->t = 0; r->state = 7; } break; }
            rocket_fly(r, dt); in->tint_red = !((int)(r->t * 20.0f) & 1);   /* 0x4537d0: 10 Hz red / normal */
            if (r->t >= 1.0f) { audio_fx_stop(11, in, 1); audio_fx(6, in, &in->position.x); r->t = 0; r->state = 7;
                game_explosion(in->position); } break;                                                          /* 0x453538: explosion kind 1 (debris, dust, two flash records) */
        case 7: if (r->type == 21) { r->q0 = in->quat; r->q1 = r->start_q; r->t = 0; r->state = 8; break; }   /* 0x453613: turn back to the start rotation */
                                                                                   /* blast 600 on actor list 1 (0x453560, ROCKET.md 4.3): first the player (0x44d040) */
            if (have_player && !pl->dead_kind) { Vec3 d = { pl->inst->position.x - in->position.x, pl->inst->position.y - in->position.y, pl->inst->position.z - in->position.z };
                if (d.x * d.x + d.y * d.y + d.z * d.z < 600.0f * 600.0f && !wenv("WOODY_GOD")) { float l = sqrtf(d.x * d.x + d.z * d.z); Vec3 away = l > 1e-3f ? (Vec3){ d.x / l, 0, d.z / l } : (Vec3){ 0, 0, 1 };
                    player_hit(pl, 0, away); pad_rumble(1.0f, 0.5f); player_kill(pl, 6);   /* rumble 0x44d1b0(+0x1d4, +0x1d0) = P+0xc4, P+0xc0 */ printf("  ROCKET %u blast kills the player", in->index), puts(""); } }
            enemies_actor_blast(&g_enemies, in->position, 600.0f);                 /* the rest of list 1: the thrower (0x4119b0) and Boss2 (0x40e800) */
            in->fade = in->fade_target = 1.0f; r->t = 0; r->state = 9; break;
        case 8: r->t += dt; if (r->t > 2.0f) r->t = 2.0f; in->quat = q_slerp(r->q0, r->q1, r->t / 2.0f);
            if (r->t >= 2.0f) { r->t = 0; r->state = 0; rocket_place(r); } break;           /* no Reset: +0x168 and the 0x40 flag stay until the next 40 */
        case 9: r->t += dt; if (r->t >= 0.5f) rocket_reset(r); break;
        }
        if (r->exhaust == 1 && (r->ex_t += dt) >= 1.0f) r->exhaust = 2;
        if (r->state) rocket_place(r);
        if (wenv("WOODY_ROCKETLOG") && r->state) { const float *m = in->world.m;   /* the model axes in world space = the rows +0x28 of the original (tools/wverify.py --probe rocket) */
            printf("rocket %u state %d t %.3f pos %.1f %.1f %.1f rows %.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f", in->index, r->state, r->t, in->position.x, in->position.y, in->position.z,
                   m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]), puts(""); }
        if (wenv("WOODY_FXLOG") && r->state >= 5 && r->state <= 7) printf("rocket %u state %d t %.2f pos %.0f %.0f %.0f speed %.0f", in->index, r->state, r->t, in->position.x, in->position.y, in->position.z, r->speed), puts("");
    }
    if (have_player && pl->ride) { Rocket *r = rocket_of(pl->ride); Vec3 d;
        if (r) { pl->ride_state = r->state; pl->ride_q = r->inst->quat;                   /* seat 0x452bd0: marker 0; type 21 in states 5..7 the bomb, 8/0 leave it where it was */
            if (r->type == 21 && r->state >= 5 && r->state <= 7) { if (r->bomb) pl->ride_seat = r->bomb->inst->position; }
            else if (r->type == 21 && (r->state == 8 || r->state == 0)) { pl->ride_state = 9; }   /* port: nothing to sit on any more, the rider gets off (ROCKET.md 0.4) */
            else if (!inst_vector(r->inst, 0, &pl->ride_seat, &d)) pl->ride_seat = r->inst->position; } }
}
/* exhaust 0x475440 on the typecode-9 marker (table 0x4abcc8 index 6: flame 100, glows 70 / 60; start-up sputters) and its
 * smoke; the two flashes of explosion kind 1 (R 1400 and 400) are records like any other, see fx_smoke_draw.
 * Glows and flames as on the race board (exhaust_glow_flames): a camera-facing glow, a glow in the plane across d and three
 * 2:1 flame quads crossed on d at 120 degrees, spinning with f3. d is P1 - P0 of the marker the first frame, after that
 * the stored point minus this frame's P0 (0x47566e); for size index 6 and 9 the stored point is P1 (0x475c2c), written only
 * inside the puff loop (0x475c58), so in flight the flame leans back along the way the rocket came.
 * The list driver (0x46d0a4, before the pool driver) runs the exhaust in every frame the think step marked it (+0xc = 1 at
 * 0x452e67: every rocket state but 0, not while paused, 0x452e3b), whatever the rocket's visibility: it burns on at the blast
 * point in states 7 and 9. The smoke is the race board's (board_fx_draw): 200 puffs a second (0x4aa164, 0x4abd08 = 0.005)
 * from 100 (the size-6 entry of 0x4abcc8) behind the nozzle back along the trail, pool records FX_BOARD_PUFF (0x475380). */
static void rockets_draw(float dt)
{
    for (int i = 0; i < g_nrockets; i++) {
        Rocket *r = &g_rockets[i]; Vec3 m, d; if (!r->state || dt <= 0 || !inst_vector(r->inst, 9, &m, &d)) continue;
        r->f1 += dt * 0.05f; r->f2 += dt * 0.15f; r->f3 += dt * 3.0f;             /* wrapped at 1 (0x475469..0x4754dc) */
        if (r->f1 >= 1.0f) r->f1 -= 1.0f;
        if (r->f2 >= 1.0f) r->f2 -= 1.0f;
        if (r->f3 >= 1.0f) r->f3 -= 1.0f;
        if (r->exhaust) r->puff_acc += dt;                                        /* 0x4754e3: in every exhaust state but 0 */
        int n = (int)(r->puff_acc * 200.0f); r->puff_acc -= n * 0.005f;
        if (!r->exhaust) continue;
        float s = 1.0f, ta = r->ex_t;
        if (r->exhaust == 1) s = ta < 0.15f ? ta * 6.667f : (ta > 0.3f && ta < 0.45f) ? (ta - 0.3f) * 6.667f : (ta > 0.85f && ta < 1.0f) ? (ta - 0.85f) * 6.667f : 0;
        Vec3 t = r->has_prev ? (Vec3){ r->prev_p1.x - m.x, r->prev_p1.y - m.y, r->prev_p1.z - m.z } : d;
        r->has_prev = 1;                                                         /* 0x475669 */
        float len = sqrtf(t.x * t.x + t.y * t.y + t.z * t.z); if (len > 0) { t.x /= len; t.y /= len; t.z /= len; }
        if (s > 0) {                                                             /* state 1 draws only inside its three ramps */
            const float ph[3] = { r->f1, r->f2, r->f3 };
            exhaust_glow_flames(m, t, 6, r->exhaust == 1 ? 1 : 2, s, ph);
        }
        for (int k = n - 1; k >= 0; k--) {                                       /* 0x475b7a: also between the ramps */
            float dist = (float)k / (float)n * len + 100.0f;
            FxRec *pf = fx_new(0.2f, (Vec3){ m.x + t.x * dist, m.y + t.y * dist, m.z + t.z * dist }, FX_BOARD_PUFF); if (!pf) continue;
            pf->age = k * dt / n;                                                /* 0x475bbf */
            r->prev_p1 = (Vec3){ m.x + d.x, m.y + d.y, m.z + d.z };              /* 0x475c58 */
            pf->rot = (int)(fx_rnd() * 512.0f);                                  /* 0x475c79 */
        }
    }
}
/* ---- environment instances, class 90: all three modes (butterflies, motes, rain) are in ambient.c (docs/AMBIENT.md); their
 * particles are FX_AMB records of the effect pool above. */
/* the per-frame instance list the sound Update gets (world+0x64, 0x401ee7): rnd_instance_list, built at the start of the frame
 * (0x42a980 -> 0x42a840, with the frustum / race-distance test of stationary instances, docs/INSTANCE.md 4.1) */
static int snd_owner_active(const void *owner) { const Instance *in = owner; return in->visible && in->listed; }
static uint32_t g_text_var; static int g_hud_ext;                 /* 1080: close flag variable; 1172: extended HUD this frame (app+0x70) */
static int g_cam_hold;                                            /* CamMgr+0x290, messages 1649 / 1650: written, never read (0x41fa40 / 0x41fa50) */
static void snd_msg(const EkoMsg *m, Instance *in)
{
#define AI(i) ((i) < (int)m->nargs ? (float)(int32_t)m->args[i] : 0.0f)
    uint32_t s2 = m->nargs ? m->args[0] : 0, s3 = m->nargs > 1 ? m->args[1] : 0;   /* sample ref of the 2D / 3D forms */
    const float *pos = in ? &in->position.x : NULL;
    int q = (m->id >= 1603 && m->id <= 1605) || (m->id >= 1611 && m->id <= 1615) || m->id == 1617 || (m->id >= 1624 && m->id <= 1626) || (m->id >= 1636 && m->id <= 1638);   /* queued variants: vt[0x2c]/[0x30], 3D queue = 1 (0x468466, 0x468587) */
    switch (m->id) {
    case 1600: case 1603: audio_play_q(s2, NULL, q, 0, AI(1), 1.0f, NULL, 0, 0); break;
    case 1601: case 1604: audio_play_q(s2, NULL, q, 0, AI(1), AI(2) * 0.01f, NULL, 0, 0); break;
    case 1602: case 1605: audio_play_q(s2, NULL, q, 0, AI(1), AI(2) * -0.01f, NULL, 0, 0); break;
    case 1606: case 1611: audio_play_q(s2, NULL, q, 1, AI(1), 1.0f, NULL, 0, 0); break;
    case 1607: case 1612: audio_play_q(s2, NULL, q, 1, AI(1), AI(2) * 0.01f, NULL, 0, 0); break;
    case 1609: case 1614: audio_play_q(s2, NULL, q, 1, AI(1), AI(2) * -0.01f, NULL, 0, 0); break;
    case 1608: case 1610: case 1613: case 1615: audio_play_q(s2, NULL, q, 1, AI(1), AI(2) * 0.01f, NULL, 0, AI(3) * 0.01f); break;
    case 1616: case 1617: audio_play_q(s3, in, q, 0, AI(2), 1.0f, NULL, 0, 0); break;
    case 1618: case 1619: audio_play_q(s3, in, 1, 0, AI(2), -AI(3), NULL, 0, 0); break;   /* 0x46820a: -arg3 without the 0.01, so arg3 = seconds */
    case 1652: audio_stop2d(s2, AI(1) * 0.01f, (int)AI(2)); break;
    case 1655: audio_music((int)AI(0)); break;
    case 1646: case 1656: audio_music_stop(AI(0) * 0.01f); break;
    case 1657: audio_next_fade_in(AI(0) * 0.01f); break;
    case 1649: case 1650:                                                           /* 0x46864f / 0x468660: no arguments, [0x4c737c] = CamMgr: 0x41fa40 sets +0x290 = 1,
                                                                                     * 0x41fa50 clears it (also on every camera mode switch, 0x41f50c..0x41f5dc). Nothing
                                                                                     * reads CamMgr+0x290 (docs/SOUND.md 8.5), and no script sends either: a flag, no effect */
        g_cam_hold = m->id == 1649; if (wenv("WOODY_SNDLOG")) printf("  SND %d: camera flag CamMgr+0x290 = %d (unread)\n", m->id, g_cam_hold); break;
    default: break;
    }
    if (!in) return;
    switch (m->id) {                                                                /* 3D: key (instance, sample); default dmin 2 m */
    case 1620: audio_play(s3, in, 1, AI(2), 1.0f, pos, 2.0f, 0); break;
    case 1621: audio_play(s3, in, 1, AI(2), AI(3) * 0.01f, pos, 2.0f, 0); break;
    case 1622: case 1624: audio_play_q(s3, in, q, 0, AI(2), 1.0f, pos, 2.0f, 0); break;
    case 1623: case 1625: audio_play_q(s3, in, q, 0, AI(2), AI(3) * 0.01f, pos, 2.0f, 0); break;
    case 1626: case 1627: audio_play_q(s3, in, q, 0, AI(2), AI(3) * -0.01f, pos, 2.0f, 0); break;
    case 1628: audio_stop3d(s3, in, AI(2) * 0.01f); break;
    case 1629: audio_play(s3, in, 1, AI(2), AI(3) * -0.01f, pos, 2.0f, AI(4) * 0.01f); break;
    case 1630: audio_play(s3, in, 1, AI(2), 1.0f, pos, AI(3) * 0.01f, 0); break;
    case 1631: audio_play(s3, in, 1, AI(2), AI(3) * 0.01f, pos, AI(4) * 0.01f, 0); break;
    case 1632: audio_play(s3, in, 1, AI(2), AI(3) * -0.01f, pos, AI(5) * 0.01f, AI(4) * 0.01f); break;
    case 1633: case 1636: audio_play_q(s3, in, q, 0, AI(2), 1.0f, pos, AI(3) * 0.01f, 0); break;
    case 1634: case 1637: audio_play_q(s3, in, q, 0, AI(2), AI(3) * 0.01f, pos, AI(4) * 0.01f, 0); break;
    case 1635: case 1638: audio_play_q(s3, in, q, 0, AI(2), AI(3) * -0.01f, pos, AI(4) * 0.01f, 0); break;
    default: break;
    }
#undef AI
}

/* animation events of type 4 on the root node = sounds (0x42f5e0 -> 0x43a8f0 -> 0x4695f0, docs/SOUND.md 3):
 * {4, t, ref, probLo, probHi, vol, pitch%, dmin cm, 0}; one random draw per call picks among the variants; the Perso plays 2D.
 * 0x42f5e0 is the tail of an instance's vtbl[3] update, which only runs for the instances of this frame's list world+0x64;
 * it always stores frame / animation / time (+0xc4/+0xc8/+0xcc) but only collects events when +0xc4 was the previous frame,
 * so an instance that comes back into the list starts from where it is, without a backlog. Scanning every instance of the
 * level instead started a stream of far-away one-shots (W3B: 17 owners of ref 0x4b ~100 m off) that filled the voice pool. */
static unsigned g_snd_frame = 1;
static void anim_sounds(Instance *ii)
{
    const Model *mo = ii->model;
    if (!ii->visible || !mo->nnodes || ii->anim < 0 || (uint32_t)ii->anim >= mo->nanims) return;
    if (!(g_player && g_player->inst == ii) && !game_enemy_thinks(ii)) return;         /* not in the list: no update, no scan (the Perso's update always runs) */
    int fire = ii->snd_frame == g_snd_frame - 1; ii->snd_frame = g_snd_frame;
    const InsNode *n = &mo->nodes[0]; if (!n->event_refs || !n->pool || !n->event_refs[ii->anim].cnt) { ii->snd_anim = ii->anim; return; }
    const InsAnim *a = &mo->anims[ii->anim]; float dur = a->duration_s > 0 ? a->duration_s : 1.0f;
    float ph = fmodf(ii->anim_time / dur, 1.0f); if (ph < 0) ph += 1.0f; float tf = ph * (float)a->nframes;
    float t0 = ii->snd_anim == ii->anim ? ii->snd_tf : 0.0f;                         /* 0x43a8b4: a new animation plays its events of [0, tNow) (Woody's results arrival, anim 74, speaks at t = 0) */
    ii->snd_anim = ii->anim; ii->snd_tf = tf;
    if (!fire || tf == t0) return;
    float r = -1;
    const uint32_t *e = (const uint32_t *)(n->pool + ((size_t)n->a + n->b + n->event_refs[ii->anim].off) * 4);
    for (uint32_t i = 0; i < n->event_refs[ii->anim].cnt; i++) {
        uint32_t type = e[0], size = type == 3 ? 15 : type == 4 ? 9 : type == 5 ? 6 : 0; if (!size) return;
        if (type == 4) {
            float t, lo, hi, vol, dmin; int32_t pitch;
            memcpy(&t, &e[1], 4); memcpy(&lo, &e[3], 4); memcpy(&hi, &e[4], 4); memcpy(&vol, &e[5], 4); memcpy(&pitch, &e[6], 4); memcpy(&dmin, &e[7], 4);
            int hit = tf > t0 ? (t >= t0 && t < tf) : (t >= t0 || t < tf);
            if (hit) {
                if (r < 0) { r = (float)msvc_rand(NULL) / 32767.0f * 100.0f - 1.0f; if (r < 0) r = 0; }
                if (r >= lo && r < hi) {
                    int perso = g_player && g_player->inst == ii;
                    audio_play(e[2], ii, 0, vol, (float)pitch * 0.01f, perso ? NULL : &ii->position.x, dmin * 0.01f, 0);
                }
            }
        }
        e += size;
    }
}
static void on_msg(EkoVM *vm, const EkoMsg *m, void *user)
{
    (void)user;
    Instance *in = m->nargs ? slot_instance(m->args[0]) : NULL;
    if (m->id >= 500 && m->id <= 800 && m->nargs && slot_camera(m->args[0])) cam_msg(m, slot_camera(m->args[0]));
    if (m->id == 1200 && in && m->nargs > 1 && m->args[1] == 36 && g_nuniq < 64) g_uniq[g_nuniq++] = in;   /* 0x44f67e: the sequence number n */
    if (m->id == 1200 && in) rnd_note_link(in);   /* 0x403e7a: the new class object is linked in front of its sector chain (the list order, INSTANCE.md 4.1) */
    switch (m->id) {
    /* The black outline (SetFlags bit 0x20) comes from the level script alone: every level sends message 45 with 0x21
     * right after this message to each actor the original draws with a rim, the bosses included. The exceptions are
     * authentic: Woody in Blackbox/Credits/Lang, the W2B end boss (type 12) and the W3B ghosts (type 13). The port used
     * to set the bit here on every actor class because Buzz came out without a rim, but that was the outline distance
     * being measured from the .ins position instead of the animated root inst+0x60 (issue #35, ins_anim_centre). */
    case 1200: if (in && m->nargs > 1) { in->type = (int)m->args[1]; if (g_player && (in->type == 1 || in->type == 2 || in->type == 3 || in->type == 18 || in->type == 19) && g_player->inst != in) { g_player->inst->scripted = 1; player_bind(g_player, in); in->scripted = 0; printf("player: instance %u (type %d) at %.0f %.0f %.0f\n", in->index, in->type, in->position.x, in->position.y, in->position.z); } if ((in->type >= 4 && in->type <= 9) || (in->type >= 12 && in->type <= 16)) enemies_add(&g_enemies, in, in->type, g_player); if (in->type == 34 && g_player) { g_player->bonus_total++; } if (in->type == 37 && g_player) { g_player->race_total++; } if ((in->type == 20 || in->type == 21) && !rocket_of(in) && g_nrockets < 8) { Rocket *rk = &g_rockets[g_nrockets++]; memset(rk, 0, sizeof *rk); rk->inst = in; rk->type = in->type; rk->start_pos = in->position; rk->start_q = in->quat; rk->fly_time = 10.0f; rk->vmax = 1000.0f; in->scripted = 0; }   /* 0x452890; 21 = the bomb cannon */ if (in->type == 40 && !bomb_of(in) && g_nbombs < 16) { Bomb *b = &g_bombs[g_nbombs++]; memset(b, 0, sizeof *b); b->inst = in; b->var = -1; }   /* ctor 0x44d250: into the pool, parked visible where the .ins has it */ if ((in->type == 120 || in->type == 121) && g_nchests < 32) { int k = 0; while (k < g_nchests && g_chests[k] != in) k++; if (k == g_nchests) g_chests[g_nchests++] = in; }   /* ctor 0x451650, list 0x5e581c */ if (in->type == 41) missile_add(in);   /* 0x403b5d: into the missile pool, hidden (0x472530) */ if (in->type == 60) water_add(in);   /* the water volume (water.c, docs/WATER.md) */ if (in->type == 80) storm_add(in);   /* the lightning rod (storm.c, docs/STORM.md) */ if (in->type == 110) in->visible = 0;   /* 0x489210 (vtable[3]) puts these where the world-select carousel wants them every frame, so the original never draws them at their .ins position; only page 3 shows them (carousel_frame) */ if (in->type == 42 && !launcher_of(in) && g_nlaunchers < MAX_LAUNCHERS) { Launcher *l = &g_launchers[g_nlaunchers++]; memset(l, 0, sizeof *l); l->inst = in; l->kind = 1; l->t = PROJ_T[1]; l->T = 1.0f; l->anim = -1; }   /* ctor 0x452140 -> 0x452330(1): template 1 */ if (in->type >= 50 && in->type <= 52 && !laser_of(in) && g_nlasers < 64) { Laser *z = &g_lasers[g_nlasers++]; memset(z, 0, sizeof *z); z->inst = in; z->type = in->type; z->len = 400.0f; z->phase = (float)in->id; for (int k = 0; k < 8; k++) laser_fx_init(&z->fx[k]); } if (wenv("WOODY_TYPELOG")) printf("  TYPE %d inst %u model %d visible %d fade %.2f pos %.0f %.0f %.0f", in->type, in->index, (int)(in->model - g_ins.models), in->visible, in->fade, in->position.x, in->position.y, in->position.z), puts(""); if (wenv("WOODY_VECLOG") && (in->type >= 1 && in->type <= 3)) for (uint32_t q = 0; q < g_ins.nslots; q++) { Vec3 vp, vd; Instance *w = g_ins.slots[q]; if (w && inst_vector(w, 5, &vp, &vd)) printf("  slot %u inst %u: vector5 at %.0f %.0f %.0f dir %.0f %.0f %.0f", q, w->index, vp.x, vp.y, vp.z, vd.x, vd.y, vd.z), puts(""); }   /* door / switch markers */ } break;   /* SetTypeInstance; [0x5e54e4] = Woody bonus total */
    /* sent by shipped scripts, nothing to do in the port (docs/MESSAGES.md): 51 = laser rec+0x18 (0x451059, no reader in the exe),
     * 63 = class 17 +0x114 = v + Reset (0x40c5e0; v = 1 = no typecode-9 smoke emitter, which the ctor 0x40c3f8 already set),
     * 1010 = debug print 0x462c60 */
    case 51: case 63: case 1010: break;
    /* 58 = carousel registration (0x451960 -> 0x45e6f0, docs/MENU_LOAD.md 4.1; the port takes the fixed House slots 105..114).
     * n = 3, the BlackBox figure, also gets 0x436ca0(1.0, {1,-1,-1,-1}): anim 1 once at speed 3, long over when page 3 shows it,
     * so it holds anim 1's last frame (live: speed 0, apos 10, slots 1 -1 -1 -1). Without it the figure stood in frame 0 of
     * anim 0 - the House fly-in, 640 units off its pedestal and tilted - until it first came to the front. */
    case 58: if (in && m->nargs > 1 && m->args[1] == 3) inst_play_once(in, 1, 3.0f, g_now); break;
    case 1509: if (m->nargs > 3) game_msg1509(slot_instance(m->args[1]), (int)m->args[2], (int)m->args[3]); break;   /* 0x46cf6f: arg 1 is the instance, arg 0 is not read */
    case 1505: if (in && m->nargs > 1) game_splash(in->position, 1000.0f, (float)(int32_t)m->args[1] * 0.01f); break;   /* splash 0x46cdfd -> 0x478660 (docs/SPLASH.md 1) */
    case 1507: if (in) game_hit_star(in->position); break;                          /* 0x46ce85 -> 0x4750e0(&inst+0xc): the hit star at the instance (docs/PARTICLES.md 9) */
    case 1508: torch_add(in); break;                                                /* 0x46ceae: torch flames on the type-0 markers, list 0x5e8638 (docs/PARTICLES.md 9) */
    case 34: if (in && m->nargs > 1 && g_rnd) rnd_link((Renderer *)g_rnd, in, slot_instance(m->args[1])); break;   /* 0x42dc21: hide args[1] while the camera is in inst's volume (docs/INSTANCE.md 10.1) */
    case 33: break;                                                                 /* only ever sent to class 60, whose handler 0x474a40 drops it (like the base 0x42d5e0); docs/WATER.md 1.1 */
    case 1506: if (in && m->nargs > 4) water_param(in, (int32_t)m->args[1], (int32_t)m->args[2], (int32_t)m->args[3], (int32_t)m->args[4]); break;   /* SetWaterVolumeParameter 0x46ce38 */
    case 1500: if (in && m->nargs > 4) game_bubble(in, (int32_t)m->args[1], (int32_t)m->args[2] * 0.01f, (float)(int32_t)m->args[3], (float)(int32_t)m->args[4], NULL); break;   /* speech bubble 0x46ccc0: [inst, kind, duration cs, offY, offX] (K2R, S2R) */
    case 1501: case 1504: case 1502: case 1503: case 1511: {                        /* environment instance (class 90), handler 0x46cca0 */
        ambient_msg(in, (int)m->id, m->args, (int)m->nargs);                        /* mode, 1502 colour + life + count, 1503 rain force, 1504 count, 1511 off (ambient.c, docs/AMBIENT.md) */
        break; }
    case 59: case 60:                                                               /* class 14 (the Buzz boss, 0x410070): 59 couples an instance, 60 names its mailbox var */
        if (in && (in->type == 14 || (m->id == 60 && (in->type == 15 || in->type == 16))) && m->nargs > 1) enemies_boss_msg(&g_enemies, in, (int)m->id, m->args[1], m->id == 59 ? slot_instance(m->args[1]) : NULL);
        break;
    case 61: case 62:                                                               /* classes 15 / 16 (docs/BOSS15_16.md): 61 links 8 instances, 62 a group number and 7 */
        if (in && (in->type == 15 || in->type == 16)) { Instance *li[8] = { 0 }; int k0 = m->id == 62 ? 2 : 1, n = 0;
            for (uint32_t k = (uint32_t)k0; k < m->nargs && n < 8; k++) li[n++] = slot_instance(m->args[k]);
            enemies_boss_links(&g_enemies, in, (int)m->id, m->id == 62 && m->nargs > 1 ? (int)m->args[1] : 0, li, n); }
        break;
    case 15: case 16: case 17: case 18: case 19:                                    /* texture overrides (docs/INSTANCE.md 2): 16/18 frames - the level-select doors turn their
                                                                                     * red cross into a green tick with it -, 15/17 UV scroll (no level sends those); 0x42db50
                                                                                     * has no scripted test */
        if (in) inst_msg(in, m->id, m->args, m->nargs, g_now);
        break;
    case 1: case 2: case 3: case 4: case 5: case 6: case 12: case 13:               /* base class: animation, show/hide, path, fade (instance.c) */
    case 45:                                                                        /* SetFlags (0x42ddb4) is a plain store on every instance, the player included: bit
                                                                                     * 0x20 is what gives a model its black outline (docs/MODEL_RENDER.md 11) */
        if (in && m->id == 6 && m->nargs > 1 && !m->args[1]) enemies_msg6_off(&g_enemies, in);   /* Enemy::HandleMsg 0x41abfd: UnPress 0x41ac11 before 0x407850 */
        if (in && inst_msg(in, m->id, m->args, m->nargs, g_now) && g_nretry < 32) g_retry[g_nretry++] = *m;   /* 12/13 while an animation runs: offered again
                                                                                     * every frame until it ended (retry list 0x401250, docs/INSTANCE.md 5) */
        break;
    case 42: case 43: case 44: case 46: case 56: case 57:
        if (in && in->scripted && inst_msg(in, m->id, m->args, m->nargs, g_now) && g_nretry < 32) g_retry[g_nretry++] = *m;
        break;
    case 40: if (in && rocket_of(in) && g_player) { Rocket *rk = rocket_of(in);      /* 0x452a50: only at rest, and only when the Perso accepts (state 0, on the ground) */
            if (rk->state == 0 && player_mount(g_player, in)) { rocket_reset(rk); rk->t = 0; rk->state = 1; in->noncollide = 1; Vec3 d; if (!inst_vector(in, 0, &g_player->ride_seat, &d)) g_player->ride_seat = in->position; g_player->ride_q = in->quat; } } break;
    case 29: if (in && rocket_of(in)) rocket_reset(rocket_of(in)); if (in && bomb_of(in)) bomb_reset(bomb_of(in)); if (in && (in->type == 120 || in->type == 121)) chest_reset(in); break;   /* 0x451820: bomb and chest Reset */
    case 1090: if (in && m->nargs > 2) {                                           /* the bomb dispenser 0x444e00 (docs/BOMB_CARRY.md 2): template 0, fuse arg * 0.01 s */
        BombT t = BOMB_T0; t.life = (float)(int32_t)m->args[2] * 0.01f; Vec3 p0, d; float l = 0;
        if (game_inst_vector(in, 0, &p0, &d)) { l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z); t.speed = 100.0f; }   /* 0x42f6b0 poses the dispenser first (vtbl[2](1)) */ else { p0 = in->position; d = (Vec3){ 0, -1, 0 }; t.speed = 0; }
        if (l > 1e-4f) { d.x /= l; d.y /= l; d.z /= l; }
        bomb_start(&t, p0, d, 1, (int32_t)(m->args[1] & 0xffffff), 0);            /* no free bomb: the variable is set at once */
    } break;
    case 55: if (in && rocket_of(in) && m->nargs > 2) { Rocket *rk = rocket_of(in); if (m->args[1] == 1) rk->fly_time = (float)(int32_t)m->args[2] * 0.01f; else if (m->args[1] == 2) rk->vmax = (float)(int32_t)m->args[2]; } break;
    case 7: if (in) { int k = 0; for (int i = 0; i < g_nretry; i++) if (slot_instance(g_retry[i].args[0]) != in) g_retry[k++] = g_retry[i]; g_nretry = k; } break;
    case 10:                                                                        /* Collect (docs/BONUS.md): the level script saw the player enter the bonus volume */
        /* no "already taken" test (0x44f350): a removed bonus simply gets no more volume events, and the Jackpot pays its
         * prize by sending 10 up to five times to the same hidden life bonus */
        if (in && g_player && player_collect(g_player, in->type, m->nargs > 1 ? (int)m->args[1] : 0)) {
            int t = in->type;
            Vec3 fp = in->position;
            if (t == 34 && in->node_world) { fp.x = in->node_world[0].m[12]; fp.y = in->node_world[0].m[13]; fp.z = in->node_world[0].m[14]; }   /* 0x44f630: the W sits on its animated volume node, not on inst.pos */
            int fx = t == 30 ? 0 : t == 35 ? 1 : t == 34 ? 2 : t == 36 ? 3 : (t == 37 || t == 38) ? 4 : -1;
            int kind = t == 30 ? 1 : t == 36 ? 2 : t == 35 ? 3 : t == 34 ? 4 : t == 37 ? 5 : 0;       /* 0x448510; type 38 has no HUD animation */
            in->visible = 0;                                                        /* 0x407850: cell = -1 */
            if (t == 36) { uint8_t *f = uniq_flag(in); if (f) { *f = 1; printf("  unique item %d of level %d taken (character %d)\n", (int)(f - g_save.chr[g_char].rec[g_level].uniq), g_level, g_char); } }   /* 0x44f755: 0x450760 */
            if (fx >= 0) game_pickup_fx(fx, fp);
            if (kind && g_npick < 8) { g_pick[g_npick].kind = kind; g_pick[g_npick].pos = fp; g_npick++; }   /* projected and started in the frame loop, where the camera is */
        }
        break;
    case 1020:                                                                      /* 0x44516a: Perso->vt[38](1) = Kill(1) 0x44c110, sent by the pit / water volumes; + 0x459030 unless CamMgr+0x138 == 5 (the side view).
                                                                                     * 0x459030 runs even when Kill refuses (Boss2 beaten, App closing): only the side-view test 0x44517c gates it */
        if (g_player) {
            player_kill(g_player, 1);
            if (g_cam.mode != 0x20) { g_cam.fix_pos = g_cam.pos; g_cam.fix_target = g_player->inst; g_cam.fix_f = g_cam.look_off.y; g_cam.cut = 1; cam_set_mode(2); g_cam.death_cam = 1; }
        }
        break;
    /* game flow (docs/GAMEFLOW.md) */
    case 1081: if (m->nargs) request_level((int)m->args[0], 1.5f); break;                                   /* GotoLevel: 0x404b60(1.5, level, 1, 0) */
    case 1083:                                                                                              /* EndLevel 0x404be0 */
        if (g_level == 1 || g_level == 11 || g_level == 18 || g_level == 25) { request_level(0, 0.5f); break; }    /* from a hub: to the title */
        if (g_level >= 0 && g_level < 29) g_save.chr[g_char].rec[g_level].done = 1;
        results_capture();                                                                                  /* memcpy(app+0x74, perso+0x710, 20): the results screen runs in the hub, after the switch */
        request_level(g_char == 0 ? 1 : g_char == 1 ? 11 : 18, 0.5f); break;
    case 1082: if (m->nargs > 1) eko_set_var(vm, m->args[1], level_is_enable((int)m->args[0])); break;
    case 1085: if (m->nargs > 1) eko_set_var(vm, m->args[1], m->args[0] < 29 ? g_save.chr[g_char].rec[m->args[0]].done : 0); break;   /* LevelIsDone 0x4509e0 */
    case 1030:                                                                                              /* SaveAuto: checkpoint 0x445129 -> 0x44aa10 */
        if (in && g_player) {                                                                               /* +0x318 = inst.pos; +0x324 = xz of marker typecode 0 (P1 - P0, 0x42f6b0), else the current facing */
            Vec3 p0, dir; g_player->spawn_pos = in->position; g_player->has_ckpt = 1;   /* +0x330 */
            g_player->race_bonus_ckpt = g_player->race_bonus;                                               /* +0x4e0 = +0x264 (0x44aaef) */
            g_player->spawn_yaw = inst_vector(in, 0, &p0, &dir) && dir.x * dir.x + dir.z * dir.z > 1e-6f ? atan2f(dir.x, dir.z) : g_player->yaw;
        } else if (g_player && m->nargs && (m->args[0] & 0xffffff) == 0) {                                 /* 0x44514d: SaveAuto(0) -> the (silent, 0x462c60) warning "SaveAuto(this)", then */
            g_player->spawn_pos = g_player->pos; g_player->spawn_yaw = g_player->yaw;                       /* 0x44a920(NULL, 0): +0x318 = +0x1f4, +0x324 = the Mover's facing; no +0x330 / +0x4e0 */
        }
        break;
    case 1142: g_prop = in; break;
    case 1121:                                                                                              /* StartBoostSurf 0x444a37 -> 0x456000: along marker typecode 0 of inst, speed a, a*0.01 s */
        if (in && g_player && m->nargs > 2) { Vec3 p0, dir; if (!inst_vector(in, 0, &p0, &dir)) { puts("  Pas de Vecteur dans l'instance du message StartBoostSurf"); break; }
            player_boost(g_player, p0, dir, (float)(int32_t)m->args[1], (float)(int32_t)m->args[2] * 0.01f); }
        break;
    case 1120:                                                                                              /* SetRaceInfo 0x455dc0(board, camera polyline): the board rides under the Perso (docs/RACE.md 1) */
        if (in && g_player && m->nargs > 1) {
            Camera *pc = slot_camera(m->args[1]);
            if (!pc || !pc->traj.npoints) { puts("  Message SetRaceInfo : on doit envoyer une camera avec une polyline en 2eme argument"); break; }   /* 0x444b67 */
            g_player->board = in; g_player->race_path = &pc->traj; in->scripted = 0; in->anim_speed = 0;
            g_player->board_lanim = -1;                                                                     /* B = new AnimCtl(board): not reset, nothing requested */
            memset(&g_player->bfx, 0, sizeof g_player->bfx); g_player->bfx.inst = in; g_player->bfx.mode = 2; g_player->bfx.size_idx = 3;   /* the spray emitter (0x34 B) */
            { Vec3 p0, d; while (g_player->bfx.n < 4 && inst_vector_at(in, 9, (uint32_t)g_player->bfx.n, &p0, &d)) g_player->bfx.n++; }   /* its type-9 markers (0x455e58) */
            printf("  RACE board = instance %u, path = camera %u (%u points), %d spray markers\n", in->index, pc->index, pc->traj.npoints, g_player->bfx.n);
            if (g_rnd && g_player->race_char && !wenv("WOODY_NORACEVIS")) rnd_set_race((Renderer *)g_rnd, &pc->traj);   /* 0x455f10: region list -> renderer+0xc0, read by 0x42a980 for subtypes 4/5 only (0x401c36) */
        }
        break;
    case 1040:                                                                                              /* scripted Perso action 0x44dda0: 17 = walk into the door, 18 = come out of it (docs/PERSO_DEATH.md 2) */
        if (g_player && m->nargs > 1) {
            plane_release(); Vec3 p0 = { 0, 0, 0 }, dir = { 0, 0, 0 }; int have = in && inst_vector(in, 5, &p0, &dir);   /* 0x445250: 0x42f6b0(inst, 5, buf, 0), no fallback; on a miss the
                                                                                                             * original still passes buf (stale stack), the port keeps him where he is */
            int act = (int)m->args[1];
            /* 0x44de36: actions 17/18 (jump table 0x44dfdc / byte table 0x44dfe8: action - 10 = 7, 8 -> case 1) first take him out
             * of every volume, 0x443ff0 (refused like the rest in state 2); 10..16, 19 and 72..78 skip it (case 0) */
            if ((act == 17 || act == 18) && !g_player->dead_kind) player_leave_all(g_player, vm);
            player_script_action(g_player, act, have, p0, dir);
            /* 0x44df67 is the tail of 0x44dda0 itself, not a state-change hook (correcting docs/CAMERA_SCRIPT.md 4.3):
             * if the animation this action just started carries a camera track, that track becomes the camera. Woody's
             * animations 17 and 18 both have one, with the eye 371 units off his own axis - that is the sideways shot
             * of him walking into the door. 0x44df92 clears CamMgr+0x618 bit 1: no letterbox, unlike a cinematic. The
             * Perso+0x558 test at 0x44df73 is always true (set 24 instructions earlier) and is not ported. */
            script_action_camera();
        }
        break;
    case 1043:                                                                                              /* 0x4451e4: 0x44dda0(act, vector 5 of inst, inst2) - 1040 with an instance carried along (+0x554).
                                                                                                             * No shipped script sends it (the 28 code files: 354 x 1040, all with action 17 / 18, no 1041 / 1043) */
        if (g_player && m->nargs > 2) {
            plane_release(); Vec3 p0 = { 0, 0, 0 }, dir = { 0, 0, 0 }; int have = in && inst_vector(in, 5, &p0, &dir);
            int act = (int)m->args[1];
            if ((act == 17 || act == 18) && !g_player->dead_kind) player_leave_all(g_player, vm);
            player_script_action(g_player, act, have, p0, dir);
            if (g_player->script_act == act) g_player->script_carry = slot_instance(m->args[2]);
            script_action_camera();
        }
        break;
    case 1041:                                                                                              /* 0x445443: 0x44e040(act, &inst.pos): turn to inst, a zero-length action (dead: no script sends it) */
        if (in && g_player && m->nargs > 1) player_face_action(g_player, (int)m->args[1], in->position);
        break;
    /* 0x4451a1 / 0x4451c2: Perso state 7 (docs/PERSO_STATE7.md), carried by the type-0 vector marker of inst; 1045 ignores its argument.
     * No shipped level script sends either (every SEND in the 28 code files has an immediate id); WOODY_MSGAT can */
    case 1044: if (g_player) player_follow(g_player, in); break;
    case 1045: if (g_player) player_follow_end(g_player); break;
    case 30: if (g_player && in == g_player->inst && m->nargs > 1) player_lock(g_player, (float)(int32_t)m->args[1] * 0.01f); break;   /* Perso LockMove 0x44cde9 [_, cs] (docs/PERSO_LOOK.md 6) */
    case 26:                                                                                                /* Perso teleport 0x44ce11 [_, inst, mode]: 1 = position, 2 = position + direction of the vector marker */
        if (g_player && m->nargs > 2) {
            Instance *to = slot_instance(m->args[1]); int mode = (int)m->args[2];
            if (to && (mode == 1 || mode == 2)) {
                Vec3 p0, dir = { 0, 0, 0 }; int have = mode == 2 && (inst_vector(to, 5, &p0, &dir) || inst_vector(to, 0, &p0, &dir));
                plane_release(); eko_actor_leave_all(vm, g_player->inst->id); player_teleport(g_player, to->position, have, dir);
                /* 0x458f90 sits outside the state-5 test and cuts HERE, in script order: 0x41f9f0(2) + SetMode(0, 0). It has to happen
                 * inside the tick and not a frame later, because the rest of the tick usually puts the camera somewhere else and that has
                 * to win: an area gate in a hub sends 580 + 520 straight behind it (the fixed camera that watches the gate open,
                 * docs/CAMERA_SCRIPT.md 1.3, object 258 in WWS) and a door sends 1040 / action 18 (the camera track of the animation).
                 * Only the mode switch happens now: the follow camera seats itself (cam_init = 0) in the next camera update, which runs
                 * after this tick, so it still uses the position and facing the door action gives him. */
                cam_hard_reset();
                printf("  TELEPORT to inst %u (%.0f %.0f %.0f)%s", to->index, to->position.x, to->position.y, to->position.z, g_player->script_act ? " (refused: scripted action running, only the camera cuts)" : ""), puts("");
            }
        }
        break;
    case 1140:                                                                                              /* hub: the player arrives at the door he came out of and the results screen runs (0x453d90) */
        if (in && g_player && m->nargs > 1) results_begin(vm, in, m->args[1]);
        break;
    case 1042:                                                                                              /* peck switch: is the player at `inst` and pointing the same way as its marker? */
        if (m->nargs > 3) {
            /* 0x445269 (docs/OBJECTS.md 1.2), the engine half of every peck switch and every door in the game.
             * It answers only for a Perso who has his own controls (state 0) and stands on the ground, it measures
             * the xz distance to the START of the instance's own vector marker (typecode 0, else 5) - not to the
             * instance - and it does not test "looks at the switch" but "faces the same way as that marker".
             * On a yes it brakes the charge run that releasing the attack button started in the Perso update of this
             * same frame (0x44542f -> 0x458e40): that brake, animation 0x12, IS the peck the player sees at a switch.
             * Without it he keeps the 700 u/s of the charge run and storms into the thing he meant to peck. */
            int ok = 0, atk = g_player ? g_player->atk : 0; float d = 0, c = 0; int have = 0;
            if (in && g_player && player_state_free(g_player) && g_player->on_ground) {
                Vec3 p0, dir; have = inst_vector(in, 0, &p0, &dir) || inst_vector(in, 5, &p0, &dir);
                if (!have) { p0 = in->position; dir.x = p0.x - g_player->pos.x; dir.y = 0; dir.z = p0.z - g_player->pos.z; }   /* the original reads an uninitialised vector here; aim at the instance instead */
                float dx = p0.x - g_player->pos.x, dz = p0.z - g_player->pos.z, dl = sqrtf(dir.x * dir.x + dir.z * dir.z);
                d = sqrtf(dx * dx + dz * dz);
                c = dl < 1e-3f ? 1.0f : (sinf(g_player->yaw) * dir.x + cosf(g_player->yaw) * dir.z) / dl;   /* Mover direction . marker direction, both flattened */
                ok = d <= (float)(int)m->args[1] && c > cosf((float)(int)m->args[2] * 3.14159265f / 180.0f);   /* 0x445341: `dist` is raw, not x0.01 */
                if (ok) player_brake_charge(g_player);
            }
            if (wenv("WOODY_SWLOG") && in)
                printf("  1042 inst %u marker %d dist %.0f/%d angle %.0f/%d deg state %s atk %d -> %d", in->index, have, d, (int)m->args[1],
                       acosf(c < -1 ? -1 : c > 1 ? 1 : c) * 180.0f / 3.14159265f, (int)m->args[2],
                       !g_player ? "-" : !player_state_free(g_player) ? "busy" : !g_player->on_ground ? "air" : "free", atk, ok), puts("");
            eko_set_var(vm, m->args[3], ok);
        }
        break;
    case 1048: case 1049: case 1050:                                                                        /* key tests on actions 0, 1, 6 */
        if (m->nargs > 1) { int k = m->id - 1048, mode = (int)m->args[1], now = g_act_now[k], prev = g_act_prev[k];
                            int v = mode == 0 ? now : mode == 1 ? (now && !prev) : (!now && prev);            /* 0x467400 held, 0x467420 just pressed, 0x467440 just released */
                            if (wenv("WOODY_SWLOG") && v) printf("  %u action %d mode %d -> 1", m->id, k == 2 ? 6 : k, mode), puts("");
                            eko_set_var(vm, m->args[0], v); }
        break;
    case 1141: g_pose = in; break;
    case 1160: if (m->nargs) { g_intro_var = m->args[0]; g_have_intro = 1; g_intro_obj = m->nargs > 1 ? (int)(m->args[1] & 0xffffff) : 0; } break;
    case 1084: if (m->nargs) eko_set_var(vm, m->args[0], g_prev_level); break;                              /* GetPrevLevel: the hub script picks the spawn point with it */
    case 1180: request_level(26, 0.0f); break;                               /* 0x4448b9: 0x404b60(0, 0x1a, 0, 0x20), arg ignored; a cut, no fade-out (docs/CREDITS.md) */
    case 1150: case 1151: if (m->nargs) { fade_start((int)m->args[0] * 0.01f, m->id == 1151); g_sfade.script = 1; } break;   /* a script fade-out does not stay black when it ends: the House intro cuts to its second scene behind 1152 */
    case 1131: if (in && m->nargs > 1) { g_cin.main_inst = in; g_cin.anim = (int)m->args[1]; } break;
    case 1132: if (in && m->nargs > 1 && g_cin.nactors < 32) { g_cin.actor[g_cin.nactors].inst = in; g_cin.actor[g_cin.nactors++].anim = (int)m->args[1]; } break;
    case 1130:                                                                     /* (vector instance, rtc sound track, var) */
        if (in && g_cin.main_inst && g_cin.state == 0 && m->nargs > 2) {
            const Model *mo = g_cin.main_inst->model; g_cin.vec = in; g_cin.var = m->args[2];
            g_cin.remain = ((uint32_t)g_cin.anim < mo->nanims ? mo->anims[g_cin.anim].duration_s : 0) / 3.0f;   /* duration / 12288 */
            g_cin.state = 1; g_cin.timer = 0.5f; fade_start(0.4f, 1); g_cin.rtc = (int)m->args[1]; audio_music_pause(1, 0.45f);
        }
        break;
    case 1152: g_black_frame = 1; break;
    case 1000: case 1001: case 1002: case 1003: case 1004: if (in) {                                         /* launcher, 0x444870 (docs/PROJECTILES.md) */
        Launcher *l = launcher_of(in); if (!l) break; int a1 = m->nargs > 1 ? (int)m->args[1] : 0, a2 = m->nargs > 2 ? (int)m->args[2] : 0, a3 = m->nargs > 3 ? (int)m->args[3] : 0;
        if (m->id == 1001) { l->kind = a1; l->t = PROJ_T[a1 >= 0 && a1 <= 3 ? a1 : 1]; l->aim = 0; l->active = 0; l->anim = -1; l->anim_dur = 0; l->target = NULL; l->count = -1; l->T = 0; }   /* 0x452330: Reset 0x452260 + template a1 (0 = the thrown bomb: no visual, its life is the fuse) */
        else if (m->id == 1002) {                                                  /* 0x452360, jump table 0x4524fc (docs/PROJECTILES.md 3) */
            ProjT *t = &l->t; float v = (float)a2;
            switch (a1) {
            case 0: t->speed = v; break;            case 1: t->gravity = v; break;        case 2: t->life = v * 0.01f; break;   /* +0x184 too: no reader */
            case 3: t->max_bounce = a2; break;       case 4: t->damage = v; break;         case 5: t->aim_h = v; break;
            case 7: l->anim = a2; break;             case 8: l->anim_dur = v * 0.01f; break; case 9: t->radius = v; break;
            case 10: t->damp_g = v * 0.01f; break;   case 11: t->damp_a = v * 0.01f; break; case 12: t->steer = v * 0.001f; break;
            case 13: t->vsteer = v * 0.1f; break;    case 14: t->t_xz = v * 0.01f; break;  case 15: t->t_y = v * 0.01f; break;
            case 16: t->lim_y = v * 0.01f; break;    case 17: t->lim_xz = v * 0.01f; break;
            case 18: if (a2 >= 0 && a2 <= 3) { static const int vis[4] = { 2, 3, 4, 0 }; t->visual = vis[a2]; } break;   /* table 0x45254c */
            case 19: l->aim = a2 != 0; break;        default: break;                       /* 6: +0x188, no reader */
            } }
        else if (m->id == 1004) l->active = 0;
        else {
            int tgt = m->id == 1000 ? a1 : a1, cnt = m->id == 1000 ? 1 : a2; float T = m->id == 1000 ? 1.0f : a3 * 0.01f; if (T < 0.2f) T = 0.2f;
            l->target = tgt == -1 ? NULL : slot_instance((uint32_t)tgt); l->count = cnt; l->T = T; l->t0 = g_now + g_dt; l->last = g_now - T; l->active = 1;   /* 0x4522e9: t0 = now + dt (World+0x30 / +0x38): started in a frame, the
                                                                                    * first shot comes on the next think step; started by an init script (dt 0, verified live: W1A launcher 26
                                                                                    * first fires at 3 s, not at 0) only one interval later */
        }
    } break;
    case 11: if (in && m->nargs > 1) enemies_msg11(&g_enemies, in, (int)m->args[1], m->nargs > 2 ? (int)m->args[2] : 0); break;   /* Enemy::HandleMsg 0x41a740 */
    case 1201: case 1202: if (in) enemies_msg1201(&g_enemies, in, m->args[0], m->id == 1201); break;   /* 0x403440: type-word bit 0x400 (no level sends them) */
    case 50: case 52: case 53: if (in) { Laser *z = laser_of(in); if (z && m->nargs > 1) { if (m->id == 50) z->on = m->args[1] == 1; else if (m->id == 52) z->len = (float)(int)m->args[1]; else z->target = slot_instance(m->args[1]); } } break;
    case 1080: if (m->nargs > 3) { hud_text_open((int)m->args[0], (int)m->args[1], &m->args[3], (int)m->nargs - 3); g_text_var = m->args[2]; printf("  TEXT box at vm t=%d: strings %u %u %u\n", vm->time, m->args[3] & 0xffff, m->nargs > 4 ? m->args[4] & 0xffff : 0, m->nargs > 5 ? m->args[5] & 0xffff : 0); } break;   /* text box 0x456ed0: stays until the script sets var != 0 */
    case 1172: g_hud_ext = 1; break;
    case 54: if (in && in->type == 80 && m->nargs > 2) storm_zone_param(in, (int)m->args[1], (int)m->args[2]); break;   /* class 80 handler 0x451b50: 1 = radius, 2 = height */
    case 1100: if (m->nargs) storm_start((float)(int32_t)m->args[0] * 0.01f); break;   /* 0x444b79 -> 0x451ba0: the thunderstorm, interval x100 */
    case 1101: storm_stop(); break;                                                    /* 0x444b93 -> 0x451bd0 */
    /* the WWS Jackpot machine (script object 349): 1173 arms it while Woody has a $, 1171 pays one per spin and the three
     * Buzz faces take lives with 1170 (0x444904 / 0x444942 / 0x444934, confirmed on the switch table 0x44569c); the prizes
     * are plain Collects (message 10) on the type-30 life bonus 352. The original copies both counters into the save
     * block at once (0x44c7a0 / 0x44c840); the port's frame loop does that for every level. */
    case 1173: if (m->nargs) eko_set_var(vm, m->args[0], g_player && g_player->unique_items > 0); break;
    case 1171: if (g_player && --g_player->unique_items < 0) g_player->unique_items = 0; break;
    case 1170: if (g_player && --g_player->lives < 1) g_player->lives = 1; break;
    case 1088: if (in && g_player) cam_side_start(in, m->nargs > 1 ? (int)m->args[1] : 0); break;
    case 1110: if (m->nargs > 1) { static const int fld[9] = { -1, 3, 2, 4, 1, 0, 6, 5, 7 }; int n = (int)m->args[0];   /* n -> sv_par index */
                   if (n == 9) memcpy(g_cam.sv_par, k_sv_defaults, sizeof k_sv_defaults); else if (n >= 1 && n <= 8) g_cam.sv_par[fld[n]] = (float)(int)m->args[1]; } break;                                                              /* credits */                       /* 0x44516a: Perso->vt[38](1), sent by the pit / water volumes */
    default: if (m->id >= 1600 && m->id <= 1657) snd_msg(m, in); else if (!(m->id >= 500 && m->id <= 800 && m->nargs && slot_camera(m->args[0]))) msg_unknown(m, in ? "instance" : "game"); break;
    }
    if (g_log_msgs) {
        printf("  SEND %u [", m->id);
        for (uint32_t i = 0; i < m->nargs; i++) printf("%s%s%x", i ? ", " : "", m->args[i] >= 0x1000000 ? "0x" : "", m->args[i] >= 0x1000000 ? m->args[i] : m->args[i]);
        printf("]\n");
    }
}
static void on_warn(EkoVM *vm, const char *s, void *user) { (void)vm; (void)user; printf("VM warning: %s\n", s); }
static uint32_t g_seed = 1;
static uint32_t msvc_rand(void *user) { (void)user; g_seed = g_seed * 214013u + 2531011u; return (g_seed >> 16) & 0x7fff; }

/* one loaded level: everything that is torn down and rebuilt on a level change (the window and GL context stay) */
typedef struct {
    char name[32]; TexFile tex; GelFile gel; LitFile lit; int have_lit; VisFile vis; int have_vis; void *code; EkoVM vm; Renderer rnd;
    Player player; int have_player; double t0;
} Level;
static void *read_all(const char *path, size_t *sz);
static void level_free(Level *L)
{
    g_nlasers = 0; g_nlaunchers = 0; g_nmissiles = 0; memset(g_shots, 0, sizeof g_shots); memset(g_flashes, 0, sizeof g_flashes); memset(g_sparks, 0, sizeof g_sparks); hud_text_reset(); audio_stop_all(); audio_bank_free(1); audio_rtc(-1);                            /* vt[0x8c] StopAll on leaving a level (0x4049e0); the voices read instance memory */
    if (L->have_player) player_free(&L->player);
    car_forget(); g_nuniq = 0;                                                    /* 0x44f6c6: [0x5e54f0] = 0 */
    memset(g_stars, 0, sizeof g_stars); memset(g_bubbles, 0, sizeof g_bubbles); g_nrockets = 0; g_nbombs = 0; g_nchests = 0; memset(g_bombfx, 0, sizeof g_bombfx); water_reset(NULL); storm_reset(); g_nfx = 0; g_ntorch = 0; g_npick = 0; hud_anim_reset(); memset(g_puffs, 0, sizeof g_puffs); memset(g_blasts, 0, sizeof g_blasts); g_player = NULL; g_prop = NULL; g_pose = NULL; g_have_intro = 0; memset(&g_res, 0, sizeof g_res); g_enemies.n = 0; g_enemies.total = g_enemies.killed = 0; memset(&g_bossbar, 0, sizeof g_bossbar); memset(g_bplume, 0, sizeof g_bplume); g_nbplume = 0; memset(g_smoke_on, 0, sizeof g_smoke_on); memset(g_bsmoke, 0, sizeof g_bsmoke); player_set_carried(NULL, NULL); g_nretry = 0; memset(&g_cam, 0, sizeof g_cam); g_cam.mode = 1; memset(&g_sfade, 0, sizeof g_sfade); g_black_frame = 0; memset(&g_cin, 0, sizeof g_cin);
    ambient_reset();                                                               /* class 90 (ambient.c); its particles went with g_nfx = 0 */
    bb_free();                                                                     /* 0x4049a0: the BlackBox object goes with the level */
    rnd_free(&L->rnd); eko_free(&L->vm); free(L->code); ins_free(&g_ins); if (L->have_lit) lit_free(&L->lit); if (L->have_vis) vis_free(&L->vis); gel_free(&L->gel); tex_free(&L->tex);
    memset(L, 0, sizeof *L);
}
static int level_load(Level *L, const char *dir, const char *lvl)
{
    char path[512]; memset(L, 0, sizeof *L); snprintf(L->name, sizeof L->name, "%s", lvl);
    g_now = 0; g_clock = 0; g_dt = 0;                  /* the init scripts start animations / launchers against the new level's clock, not the previous level's */
    if (char_of_level(g_level) >= 0) g_char = char_of_level(g_level);
    snprintf(path, sizeof path, "%s/%s/%s.tex", dir, lvl, lvl); if (tex_load(&L->tex, path)) return -1;
    tp_scope(lvl);                                     /* --dumptex writes this level's textures to mods\dump\<lvl> (texpack.c) */
    snprintf(path, sizeof path, "%s/%s/%s.gel", dir, lvl, lvl); if (gel_load(&L->gel, path)) { tex_free(&L->tex); return -1; }
    snprintf(path, sizeof path, "%s/%s/%s.ins", dir, lvl, lvl); if (ins_load(&g_ins, path)) { gel_free(&L->gel); tex_free(&L->tex); return -1; }
    snprintf(path, sizeof path, "%s/%s/code", dir, lvl);
    size_t codesz; L->code = read_all(path, &codesz);
    if (!L->code || eko_load(&L->vm, L->code, codesz)) { fprintf(stderr, "cannot load %s\n", path); free(L->code); ins_free(&g_ins); gel_free(&L->gel); tex_free(&L->tex); return -1; }
    L->vm.on_msg = on_msg; L->vm.on_warn = on_warn; L->vm.rand_fn = msvc_rand; g_vm = &L->vm;
    printf("%s: %u polys, %u verts, %u textures, %u models, %u slots, %u script objects\n", lvl, L->gel.npolys, L->gel.nverts, L->tex.ngroups, g_ins.nmodels, g_ins.nslots, L->vm.nobj);
    snprintf(path, sizeof path, "%s/%s/%s.lit", dir, lvl, lvl); L->have_lit = lit_load(&L->lit, path) == 0;
    snprintf(path, sizeof path, "%s/%s/%s.vis", dir, lvl, lvl); L->have_vis = vis_load(&L->vis, path, L->gel.nsectors) == 0;   /* 0x408260: what each sector can see */
    if (wenv("WOODY_CELLLOG")) printf("  gel: %u cells, %u sectors, %u kd nodes | lit: %u lights, %u sector light lists\n", L->gel.ncells, L->gel.nsectors, L->gel.nkd, L->lit.nlights, L->lit.nsectors);
    rnd_init(&L->rnd, &L->tex, &L->gel, &g_ins, L->have_lit ? &L->lit : NULL, L->have_vis ? &L->vis : NULL);
    water_reset(&L->tex); L->rnd.post_models = water_draw;   /* class 60 (water.c) */
    L->rnd.spr_count = hud_wq_count; L->rnd.spr_quad = hud_wq_quad; L->rnd.spr_draw = hud_wq_draw; L->rnd.spr_done = hud_wq_done;   /* the world sprites (hud.c) */
    L->rnd.on_drawn = storm_rod_drawn;                       /* class 80 vt[26] 0x452010: the rod colour, per drawn rod (storm.c) */
    snprintf(path, sizeof path, "%s/%s/%s.col", dir, lvl, lvl); rnd_load_col(&L->rnd, path);   /* 0x4271e0: the objects of every kd leaf (0x42aa0b) */
    L->have_player = player_init(&L->player, &g_ins, &L->gel, &L->tex) == 0;
    g_player = L->have_player ? &L->player : NULL; g_rnd = &L->rnd; g_gel = &L->gel;
    for (uint32_t mi = 0; mi < g_ins.nmodels; mi++) for (uint32_t k = 0; k < g_ins.models[mi].ninstances; k++) inst_init(&g_ins.models[mi].instances[k]);
    if (L->have_player) { L->player.inst->scripted = 0; L->player.enemies = &g_enemies; }
{ static const char *chr[3] = { "Woody", "Knothead", "Splinter" }; static int bank0 = -1;
      if (bank0 != g_char) { snprintf(path, sizeof path, "%s/../Common/%s.rck", dir, chr[g_char]); printf("sound bank 0: %d sounds\n", audio_bank_load(0, path)); bank0 = g_char; }
      snprintf(path, sizeof path, "%s/%s/%s.rck", dir, lvl, lvl); printf("sound bank 1: %d sounds\n", audio_bank_load(1, path));
      { char common[512]; snprintf(common, sizeof common, "%s/../Common/%s.rck", dir, chr[g_char]); if (hud_load(common, path)) printf("hud: no font / images\n"); }
      { uint32_t sky[5]; if (hud_sky_images(sky)) rnd_set_sky(&L->rnd, sky); }
      M.title_music = 0; if (g_level == 0) menu_title_page0(); else if (g_level == 26) menu_enter(0x20); else menu_off(); }   /* 0x4041b0 app+0x54 = 0; 0x4017c9 -> 0x404e30: page 0 + track 0 "Menu"; level 0x1a: both 0x404b60 callers (1180, the BlackBox end 0x401d35) ask for state 0 + page 0x20, the credits (docs/CREDITS.md) */
    printf("VM init...\n"); eko_init(&L->vm);
    printf("init done: %d messages\n", L->vm.nmsgs);
    for (int i = 0; i < L->vm.nmsgs; i++) on_msg(&L->vm, &L->vm.msgs[i], NULL);   /* docs/VM.md 2: the exe queues the messages and the game loop only takes the queue after the tick,
                                                                                  * so a variable one of them writes (1082 LevelIsEnable for the level-select doors) wakes its
                                                                                  * object in the first tick instead of in an init whose wake lists are cleared at the end */
    eko_msg_reset(&L->vm);
    if (L->have_player) { SaveChar *sc = &g_save.chr[g_char]; L->player.lives = sc->lives; L->player.health = sc->health > 0 ? sc->health : 1.0f;
                          L->player.unique_items = sc->unique; L->player.special_charges = sc->charges;
                          if (wenv("WOODY_LIVES")) L->player.lives = atoi(wenv("WOODY_LIVES"));   /* testing: 1 = the next death is game over */
                          player_race_start(&L->player);     /* 0x44ab20 -> 0x456150 SurfEnter with the board of 1120 (docs/RACE.md 3.1) */
                          player_ground_snap(&L->player); }   /* 0x44a6a0 / 0x44a759; 0x44a7ee: he starts standing on the floor, not falling onto it */
    if (g_level == 25 && bb_init(dir)) printf("BlackBox: not available, the level runs as a plain set\n");   /* 0x4042c1: level 0x19 -> new 0x484420, App state 3 */
    L->t0 = win_time();
    return 0;
}

/* ---------------------------------------------------------------- the logo films (docs/HNM.md) */
/* App state 2 = 0x401500: the films of table 0x4b3960 one after the other, 0x445d00(i) starts one (path = game dir + "\Logo\..."),
 * 0x445de0 asks the player whether it still runs; action 9 held (0x467400(9): Esc in the shipped Woody.cfg) stops the running one
 * (0x445da0) and the next one starts in the same frame, so holding Esc skips them all. After the third: House + title (0x404e30).
 * PORT EXTRA: one press of Esc, Enter, Space or the jump / attack key ends the running film and the next one starts, so each press
 * skips one film; Esc counts per press too, holding it no longer runs through all three (woodyre.cfg logos=0: no films at all).
 * Test hooks: WOODY_LOGOSHOT="file.ppm T" = screenshot T s after the first film started; WOODY_LOGOESC="T ..." = Esc at those times. */
static int logo_skip_pressed(const Window *w, int *kp)                    /* Esc / Enter / Space went down, or the jump / attack action; kp = the keys of the last call */
{
    int r = in_pressed(4) || in_pressed(6) || in_pressed(9) || in_pressed(12);
    static const int sk[3] = { VK_ESCAPE, VK_RETURN, VK_SPACE };
    for (int i = 0; i < 3; i++) if (w->keys[sk[i]] && !kp[sk[i]]) r = 1;
    memcpy(kp, w->keys, sizeof w->keys); return r;
}
static void logos_play(Window *w, const char *dir)
{
    static const char *k_logo[3] = { "Cryo", "Eko", "Universal" };                 /* \Logo\Cryo.hnm, \Logo\Eko.hnm, \Logo\Universal.hnm */
    char shot[260] = ""; double shot_at = -1, esc[8]; int nesc = 0, esc_i = 0;
    { const char *e = wenv("WOODY_LOGOSHOT"); if (e && sscanf(e, "%259s %lf", shot, &shot_at) != 2) shot_at = -1; }
    { const char *e = wenv("WOODY_LOGOESC"); while (e && *e && nesc < 8) { char *q; double v = strtod(e, &q); if (q == e) break; esc[nesc++] = v; e = q; } }
    double T0 = win_time(); int kp[256]; memcpy(kp, w->keys, sizeof kp);
    for (int k = 0; k < 3 && !w->quit; k++) {
        char path[600]; snprintf(path, sizeof path, "%s/../Logo/%s.hnm", dir, k_logo[k]);
        HnmFile h; if (hnm_open(&h, path)) { printf("logo: %s missing\n", path); continue; }
        int snd = 0, stop = 0, r = 0; double t0 = win_time();
        while (!stop && !w->quit && (r = hnm_next(&h)) > 0) {
            if (h.npcm && g_setup.film) { if (!snd && h.has_sound) snd = !audio_pcm_open(h.rate, h.channels); if (snd) audio_pcm_push(h.pcm, h.npcm); }   /* the first block holds 32 frames of sound; film sound only with
                * Setup's "Cinematic" switch (0x426a57: [0x5e81bc] = cfg +0x70, else the player gets no DirectSound); no volume option applies,
                * the core never calls SetVolume on its buffer (docs/SETUP.md 3.3) */
            if (h.frame == 1) t0 = win_time();
            double due = t0 + (h.frame - 1) * h.frame_time;                                                   /* the clock of the sound: one superchunk = one frame of it */
            for (;;) {
                win_poll(w); double now = win_time(); in_frame(w, 0, now, now);
                if (esc_i < nesc && now - T0 >= esc[esc_i]) { esc_i++; stop = 1; }
                if (logo_skip_pressed(w, kp)) stop = 1;                                  /* port extra: a press (not holding, as 0x445da0 on action 9 does) ends this film */
                if (stop || w->quit || now >= due) break;
                Sleep(1);
            }
            if (stop || w->quit) break;
            rnd_film_frame(w, h.cur, h.width, h.height);
            if (shot_at >= 0 && win_time() - T0 >= shot_at) { rnd_screenshot(w, shot); printf("logo shot %s at %.2f s: film %d frame %d\n", shot, win_time() - T0, k, h.frame - 1); shot_at = -1; }
            win_swap(w);
        }
        for (double end = t0 + h.frame * h.frame_time; !stop && !w->quit && win_time() < end; Sleep(1)) { win_poll(w); double now = win_time(); in_frame(w, 0, now, now); if (logo_skip_pressed(w, kp)) stop = 1; }   /* the last frame's time */
        if (snd) audio_pcm_close();
        printf("logo %d (%s): %d of %d frames%s%s\n", k, k_logo[k], h.frame, h.frames, stop ? ", skipped" : "", r < 0 ? ", bad data" : "");
        hnm_close(&h);
    }
    rnd_film_frame(w, NULL, 0, 0);
}

static void *read_all(const char *path, size_t *sz) { FILE *f = fopen(path, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); void *b = malloc((size_t)n); if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; } fclose(f); *sz = (size_t)n; return b; }

/* a crash (PORT EXTRA): the log is buffered, so without this its last lines - the ones a bug report needs - never reach
 * woodyre.log. Writes where it happened, flushes, says so in the windowed build and lets the system end the process. */
#ifdef _WIN32
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e)
{
    void *a = e->ExceptionRecord->ExceptionAddress; HMODULE m = NULL; char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
        GetModuleFileNameA(m, name, sizeof name); const char *s = strrchr(name, '\\'); if (s) memmove(name, s + 1, strlen(s));
    }
    printf("crash: exception 0x%08lx at %p (%s+0x%llx)\n", (unsigned long)e->ExceptionRecord->ExceptionCode, a, name,
           m ? (unsigned long long)((char *)a - (char *)m) : 0ull);
    fflush(stdout);
#ifdef WOODY_GUI
    MessageBoxA(NULL, "WoodyRE has crashed. woodyre.log (next to woodyre.cfg) says where; please attach it to a bug report.", "WoodyRE", MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
#endif
    return EXCEPTION_CONTINUE_SEARCH;
}
#elif !defined __ANDROID__ && !defined __SWITCH__                 /* Android keeps its own handlers (the tombstone in logcat) */
#include <signal.h>
static void crash_signal(int sig) { printf("crash: signal %d\n", sig); fflush(stdout); signal(sig, SIG_DFL); raise(sig); }
#endif

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_filter);
#elif !defined __ANDROID__ && !defined __SWITCH__
    signal(SIGSEGV, crash_signal); signal(SIGBUS, crash_signal); signal(SIGFPE, crash_signal); signal(SIGILL, crash_signal); signal(SIGABRT, crash_signal);
#endif
    if (wenv("WOODY_UNBUF")) setvbuf(stdout, NULL, _IONBF, 0);                  /* debugging a crash: every line reaches the log */
    /* [data dir] [level] [options]: the data dir is a path (it has a / \ or :, or exists); without one data_find() looks for the
     * game files and asks for the CD at the first start (datasetup.h). No level: boot to the title (House, level 0) */
    int ai = 1; const char *dir = NULL, *lvl = "House";
    if (argc > ai && argv[ai][0] != '-' && (strpbrk(argv[ai], "/\\:") || plat_exists(argv[ai]))) dir = argv[ai++];
    if (argc > ai && argv[ai][0] != '-') lvl = argv[ai++];
    if (!dir && !(dir = data_find())) return 1;
#ifdef WOODY_GUI
    if (!wenv("WOODY_CONSOLE")) freopen("woodyre.log", "w", stdout);              /* the windowed release build: the log beside woodyre.cfg */
#endif
    int verify = 0;                                                               /* --verify: check the data against the supported CDs and quit */
    const char *shot_path = NULL; double shot_after = 0;                          /* --shot file.ppm seconds: screenshot then quit */
    int have_cam = 0; float cam_args[5] = {0, 0, 0, 0, 0};                          /* --cam x y z yaw pitch (degrees) */
    double jump_at = -1; float max_y = -1e30f, start_y = 0;                       /* --jump T: hold jump from T s for 1 s (testing), reports the apex */
    double jump_len = 1.0, jump2_at = -1, jump2_len = wenv("WOODY_J2LEN") ? atof(wenv("WOODY_J2LEN")) : 0.15;                                         /* --jump2 LEN T2: first press lasts LEN s, second press (0.15 s) at T2 */
    double peck_at = -1, peck_len = 0.1, duck_at = -1, duck_len = 0.1, special_at = -1;   /* --special T: release the special attack key (RCtrl / E) at T s */   /* --peck / --duck T LEN: hold the attack / duck key (X) from T s for LEN s (testing) */
    int have_pos = 0; float pos_args[3] = {0, 0, 0};                               /* --pos x y z: start the player there (testing) */
    int have_yaw = 0; float yaw_arg = 0;
    double enter_at = -1;                                                         /* --enter T: press Enter on the title after T s (testing) */
    int pick_type = 0, pre_bonus = -1; float pre_health = -1; double pick_at = 0;   /* --pickup TYPE T, --bonus N, --health N (testing) */
    int door_inst = -1, door_act = 17; double door_at = -1;                        /* --door INST ACT T (testing) */
    int new_game = 0, logo = -1; const char *next_name = NULL; double next_at = 0;   /* --nologo / --logo: the logo films off / on even for a scripted run */                              /* --next LVL T: change to level LVL after T s (testing) */
    double walk_for = 0, walk_at = wenv("WOODY_WALKAT") ? atof(wenv("WOODY_WALKAT")) : 0; int fly = 0;                                             /* --walk T: hold forward for T s (testing); --fly: start in free camera */
    int res_w = 0, res_h = 0, full_arg = -1, wide_arg = -1;                          /* --res WxH, --windowed / --fullscreen, --aspect 4:3|wide (port extras) */
    for (int i = ai; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i + 2 < argc) { shot_path = argv[i + 1]; shot_after = atof(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--cam") && i + 5 < argc) { for (int k = 0; k < 5; k++) cam_args[k] = (float)atof(argv[i + 1 + k]); have_cam = 1; i += 5; fly = 1; }
        else if (!strcmp(argv[i], "--walk") && i + 1 < argc) { walk_for = atof(argv[i + 1]); i += 1; }
        else if (!strcmp(argv[i], "--jump") && i + 1 < argc) { jump_at = atof(argv[i + 1]); i += 1; }
        else if (!strcmp(argv[i], "--jump2") && i + 2 < argc) { jump_len = atof(argv[i + 1]); jump2_at = atof(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--peck") && i + 2 < argc) { peck_at = atof(argv[i + 1]); peck_len = atof(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--duck") && i + 2 < argc) { duck_at = atof(argv[i + 1]); duck_len = atof(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--special") && i + 1 < argc) { special_at = atof(argv[i + 1]); i += 1; }
        else if (!strcmp(argv[i], "--pos") && i + 3 < argc) { for (int k = 0; k < 3; k++) pos_args[k] = (float)atof(argv[i + 1 + k]); have_pos = 1; i += 3; }
        else if (!strcmp(argv[i], "--fly")) fly = 1;
        else if (!strcmp(argv[i], "--enter") && i + 1 < argc) { enter_at = atof(argv[i + 1]); i += 1; }
        else if (!strcmp(argv[i], "--pickup") && i + 2 < argc) { pick_type = atoi(argv[i + 1]); pick_at = atof(argv[i + 2]); i += 2; }   /* --pickup TYPE T: collect a bonus of that type in front of the camera (testing) */
        else if (!strcmp(argv[i], "--door") && i + 3 < argc) { door_inst = atoi(argv[i + 1]); door_act = atoi(argv[i + 2]); door_at = atof(argv[i + 3]); i += 3; }   /* --door INST ACT T: send the script's own message 1040 (17 = walk into the door, 18 = come out) at T (testing) */
        else if (!strcmp(argv[i], "--bonus") && i + 1 < argc) { pre_bonus = atoi(argv[i + 1]); i += 1; }                                 /* --bonus N: start with N W's in the counter (testing) */
        else if (!strcmp(argv[i], "--health") && i + 1 < argc) { pre_health = (float)atof(argv[i + 1]); i += 1; }                        /* --health N: hearts before the pickup (testing) */
        else if (!strcmp(argv[i], "--yaw") && i + 1 < argc) { have_yaw = 1; yaw_arg = (float)atof(argv[i + 1]) * 3.14159265f / 180; i += 1; }   /* with --pos: facing in degrees */
        else if (!strcmp(argv[i], "--unlock")) g_unlock_all = 1;                       /* every level door open */
        else if (!strcmp(argv[i], "--newgame")) new_game = 1;                          /* ignore woodyre.sav */
        else if (!strcmp(argv[i], "--nologo")) logo = 0;
        else if (!strcmp(argv[i], "--logo")) logo = 1;
        else if (!strcmp(argv[i], "--prev") && i + 1 < argc) { g_prev_level = level_index(argv[i + 1]); i += 1; }   /* --prev LVL: pretend we came from LVL (hub spawn point) */
        else if (!strcmp(argv[i], "--stats") && i + 5 < argc) {                        /* --stats TOTAL_A GOT_A TOTAL_B GOT_B SECONDS: the five level statistics the results screen shows (testing) */
            g_stats.have = 1; g_stats.stats[0] = atoi(argv[i + 1]); g_stats.stats[2] = atoi(argv[i + 2]);
            g_stats.stats[1] = atoi(argv[i + 3]); g_stats.stats[3] = atoi(argv[i + 4]); g_stats.time = (float)atof(argv[i + 5]); i += 5; }
        else if (!strcmp(argv[i], "--next") && i + 2 < argc) { next_name = argv[i + 1]; next_at = atof(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--res") && i + 1 < argc) { if (sscanf(argv[i + 1], "%dx%d", &res_w, &res_h) != 2 || res_w < 320 || res_h < 240) res_w = res_h = 0; i += 1; }   /* port extras (docs/DISPLAY.md 5) */
        else if (!strcmp(argv[i], "--windowed")) full_arg = 0;
        else if (!strcmp(argv[i], "--fullscreen")) full_arg = 1;
        else if (!strcmp(argv[i], "--aspect") && i + 1 < argc) { wide_arg = strcmp(argv[i + 1], "4:3") != 0; i += 1; }
        else if (!strcmp(argv[i], "--verify")) verify = 1;
        else if (!strcmp(argv[i], "--dumptex")) tp_set_dump(1);                       /* port extra: every texture to mods\dump\<level>\ (docs/TEXTURES.md) */
    }
    if (verify) {
        int bad = data_verify(dir);
#ifdef WOODY_GUI
        char m[200] = "All game files belong to a supported CD (woodyre.log names it).";
        if (bad) snprintf(m, sizeof m, "%d game files are missing or match none of the supported CDs (the list is in woodyre.log).", bad);
        plat_message(m, bad); return 0;
#endif
        return bad != 0;
    }
#ifdef _WIN32
    SetProcessDPIAware();                                                              /* port extra: real pixels on a scaled desktop, so 4K is 4K */
#endif
    opt_read();                                                                        /* woodyre.cfg: the volumes (applied at sound start) and the display */
    setup_import(dir);                                                                 /* the Setup keys woodyre.cfg lacks: from Woody.cfg (docs/SETUP.md) */
    {   /* the display that runs: the cfg's, but a screenshot run keeps the fixed default (1280x800 window, wide, vsync) whatever the
         * cfg says; the command line and WOODY_VSYNC / WOODY_FPSCAP override both */
        static const Display def = { 1, 1280, 800, 0, 1, 0, 0, 1, 0, 0 };
        g_dnow = shot_path || wenv("WOODY_SHOTSEQ") || wenv("WOODY_LOGOSHOT") ? def : g_disp;
        if (res_w) { g_dnow.w = res_w; g_dnow.h = res_h; }
        if (full_arg >= 0) g_dnow.full = full_arg;
        if (wide_arg >= 0) g_dnow.wide = wide_arg;
        if (wenv("WOODY_VSYNC")) g_dnow.vsync = atoi(wenv("WOODY_VSYNC")) != 0;
        if (wenv("WOODY_FPSCAP")) g_dnow.cap = atoi(wenv("WOODY_FPSCAP")) < 0 ? 0 : atoi(wenv("WOODY_FPSCAP"));
        if (wenv("WOODY_AO")) g_dnow.ao = atoi(wenv("WOODY_AO")) != 0;
        if (wenv("WOODY_ANISO")) g_dnow.aniso = atoi(wenv("WOODY_ANISO")) < 1 ? 1 : atoi(wenv("WOODY_ANISO"));
        if (wenv("WOODY_SMAA")) g_dnow.smaa = atoi(wenv("WOODY_SMAA")) < 0 ? 0 : atoi(wenv("WOODY_SMAA")) > 4 ? 4 : atoi(wenv("WOODY_SMAA"));
        if (wenv("WOODY_MSAA")) g_dnow.msaa = atoi(wenv("WOODY_MSAA")) < 2 ? 0 : atoi(wenv("WOODY_MSAA"));
    }
    Window win; if (win_open(&win, "WoodyRE", 1280, 800)) return 1;
    gfx_apply();
    if (g_dnow.full || g_dnow.w != 1280 || g_dnow.h != 800) disp_apply(&win);
    else { win_vsync(g_dnow.vsync); if (g_dnow.cap > 0) timeBeginPeriod(1); }
    if (g_stats.have) g_stats.level = g_prev_level;                                    /* --stats belongs to the level --prev says we came from */
    save_reset(); if (file_read() <= 0) file_reset();                                     /* boot: the active struct is a reset one (0x402587); the slots come from woodyre.sav */
    if (!new_game && level_index(lvl) != 0) {                                          /* testing: straight into a level plays with a saved slot (WOODY_SLOT=1..4, else the first used one) */
        int s = wenv("WOODY_SLOT") ? atoi(wenv("WOODY_SLOT")) - 1 : -1;
        for (int i = 0; i < 4 && s < 0; i++) if (slot_pct(&g_file.slot[i])) s = i;
        if (s >= 0 && s < 4) g_save = g_file.slot[s];
    }
    if (!wenv("WOODY_NOSOUND") && !audio_init()) { char bf[512]; snprintf(bf, sizeof bf, "%s/../Music.bf", dir); printf("Music.bf: %d files\n", audio_bf_open(bf)); }
    opt_apply();                                                                       /* 0x4691e2: the volumes from the cfg at sound start */
    audio_reverse_stereo(wenv("WOODY_REVSTEREO") ? atoi(wenv("WOODY_REVSTEREO")) != 0 : g_setup.rev);   /* 0x4691f4: [0x5e81c0] = cfg +0x74 */
    in_read_cfg(dir);                                                                  /* 0x405e0f: Woody.cfg (key bindings, controller mode) */
    if (logo < 0) logo = !(argc > 2 && argv[2][0] != '-') && !wenv("WOODY_NOLOGO") && !shot_path && enter_at < 0 && !wenv("WOODY_KEYS") && !wenv("WOODY_SHOTSEQ") && g_logos;
    if (logo) logos_play(&win, dir);                                                   /* boot state 2 (0x402649): only when booting to the title; a level on the command line or a scripted run skips them */
    static Level L; g_level = level_index(lvl); if (level_load(&L, dir, lvl)) return 1;

    /* camera: start behind Woody (model 0, instance 0) if present */
    FreeCamera cam = { {0, 0, 0}, 0, 0, 70 };
    Instance *sel = (g_ins.nmodels && g_ins.models[0].ninstances) ? &g_ins.models[0].instances[0] : NULL;
    if (sel) { cam.pos = sel->position; cam.pos.y += 120; cam.pos.z -= 350; }
    if (have_cam) { cam.pos.x = cam_args[0]; cam.pos.y = cam_args[1]; cam.pos.z = cam_args[2]; cam.yaw = cam_args[3] * 3.14159265f / 180; cam.pitch = cam_args[4] * 3.14159265f / 180; }
    if (have_cam && wenv("WOODY_CAMFOV")) cam.fov_deg = (float)atof(wenv("WOODY_CAMFOV"));   /* testing: the free camera's vertical fov (70 by default; the game camera's is 83.97, docs/CAMERA.md 5.1) */
    else { cam.pos.x = (L.gel.bbox[0] + L.gel.bbox[1]) / 2; cam.pos.y = L.gel.bbox[3]; cam.pos.z = (L.gel.bbox[4] + L.gel.bbox[5]) / 2; cam.pitch = -1.2f; }

    if (!L.have_player) fly = 1;
    if (L.have_player && have_pos) { L.player.pos.x = pos_args[0]; L.player.pos.y = pos_args[1]; L.player.pos.z = pos_args[2]; L.player.floor_y = L.player.pos.y - 1000.0f; }
    if (L.have_player && have_yaw) L.player.yaw = yaw_arg;
    double t0 = L.t0, last = t0; int pg_prev[2] = {0, 0}, end_prev = 0, l_prev = 0; static int key_prev[256]; int paused = 0, dbg_paused = 0, tab_prev = 0, br_prev[2] = {0, 0}, f_prev[4] = {0, 0, 0, 0}, p_prev = 0, f5_prev = 0, f11_prev = 0; uint32_t frames = 0; double fps_t = t0;
    while (!win.quit) {
        win_poll(&win);
        {   /* WOODY_KEYS="T:KEY T:KEY:D ...": each entry holds KEY (RET ESC UP DOWN LEFT RIGHT SPACE CTRL RCTRL SHIFT BACK NUM0, a
             * letter, or a VK number) for 0.08 s (or D seconds), once, as soon as the level that is running has been up for T seconds - the
             * clock of --shot, so a sequence that spans a level change stays in step (testing: drives the menu pages and walks) */
            static const char *keys; static double held_until[256]; static unsigned char fired[64]; if (!keys) keys = wenv("WOODY_KEYS") ? wenv("WOODY_KEYS") : "";
            static const struct { const char *n; int vk; } kn[] = { {"RET",VK_RETURN}, {"ESC",VK_ESCAPE}, {"UP",VK_UP}, {"DOWN",VK_DOWN}, {"LEFT",VK_LEFT}, {"RIGHT",VK_RIGHT}, {"SPACE",VK_SPACE}, {"CTRL",VK_CONTROL}, {"BACK",VK_BACK}, {"RCTRL",VK_RCONTROL}, {"SHIFT",VK_SHIFT}, {"NUM0",VK_NUMPAD0} };
            double wt = wenv("WOODY_FIXDT") ? t0 + g_clock : win_time(), tn = wt - t0; int e = 0;   /* with WOODY_FIXDT the game clock, like the other hooks */
            for (const char *s = keys; *s && e < 64; e++) {
                char name[16] = ""; double t = 0; int used = 0;
                if (sscanf(s, " %lf:%15[A-Z0-9]%n", &t, name, &used) < 2 || !used) break;
                s += used; int vk = name[0] >= 'A' && name[0] <= 'Z' && !name[1] ? name[0] : atoi(name); double hold = 0.08;
                if (*s == ':' && sscanf(s, ":%lf%n", &hold, &used) == 1) s += used;
                for (unsigned i = 0; i < sizeof kn / sizeof kn[0]; i++) if (!strcmp(name, kn[i].n)) vk = kn[i].vk;
                if (!fired[e] && vk > 0 && vk < 256 && tn >= t) { fired[e] = 1; win.keys[vk] = 1; held_until[vk] = wt + hold; }
            }
            for (int k = 0; k < 256; k++) if (held_until[k] > 0 && wt >= held_until[k]) { win.keys[k] = 0; held_until[k] = 0; }
        }
        { static double cap = -1; if (cap < 0) cap = wenv("WOODY_FPS") ? atof(wenv("WOODY_FPS")) : 0;   /* WOODY_FPS=N: testing, frame-rate dependent code at N fps */
          if (cap > 0) while (win_time() - last < 1.0 / cap) Sleep(0);
          else if (g_dnow.cap > 0) { double due = last + 1.0 / g_dnow.cap, tn; while ((tn = win_time()) < due) if (due - tn > 0.002) Sleep(1); } }   /* port extra: the frame cap of the Display page / WOODY_FPSCAP (the original has none) */
        if (win.keys[VK_F11] && !f11_prev) { g_dnow.full ^= 1; g_disp_dirty = 1; }   /* port extra: F11 = fullscreen / window for this run (the Display page saves it) */
        f11_prev = win.keys[VK_F11];
        if (g_disp_dirty) disp_apply(&win);
        double now = win_time(); float dt = (float)(now - last); last = now;
        if (dt > 0.1f) dt = 0.1f;
        static double fixdt = -1; if (fixdt < 0) fixdt = wenv("WOODY_FIXDT") ? atof(wenv("WOODY_FIXDT")) : 0;   /* WOODY_FIXDT=N: testing, every frame advances 1/N s whatever */
        if (fixdt > 0) dt = (float)(1.0 / fixdt);                                       /* the wall clock says, so --shot frames repeat exactly (before/after diffs) */
        audio_offline_advance(dt);                     /* no-op unless WOODY_AUDIODUMP + WOODY_FIXDT (video capture) */
        g_clock += dt; g_now = (float)g_clock; g_dt = dt;   /* 0x401880: everything (Perso timers, animations, the script VM) runs on this one clock, so a hitch cannot make script delays
                                                        * run ahead of the action timers - a door would then teleport while action 17 is still running and 0x44a650 refuses the move */
        if (fixdt > 0) now = t0 + g_clock;             /* ... and the test hooks timed from the level start (--shot, WOODY_SHOTSEQ, WOODY_KEYS) follow the same clock */
        if (DBGKEY(VK_F5) && !f5_prev && L.have_player) { fly ^= 1; if (!fly) L.player.cam_init = 0; }
        f5_prev = win.keys[VK_F5];
        /* camera */
        float speed = (win.keys[VK_SHIFT] ? 3000.0f : 600.0f) * dt;
        Vec3 fw = cam_forward(&cam), rt = cam_right(&cam);
        if (fly && win.keys['W']) { cam.pos.x += fw.x * speed; cam.pos.y += fw.y * speed; cam.pos.z += fw.z * speed; }
        if (fly && win.keys['S']) { cam.pos.x -= fw.x * speed; cam.pos.y -= fw.y * speed; cam.pos.z -= fw.z * speed; }
        if (fly && win.keys['D']) { cam.pos.x += rt.x * speed; cam.pos.z += rt.z * speed; }
        if (fly && win.keys['A']) { cam.pos.x -= rt.x * speed; cam.pos.z -= rt.z * speed; }
        if (fly && (win.keys['E'] || win.keys[VK_SPACE])) cam.pos.y += speed;
        if (fly && win.keys['Q']) cam.pos.y -= speed;
        if (fly) { cam.yaw -= win.mouse_dx * 0.004f; cam.pitch -= win.mouse_dy * 0.004f; }
        if (cam.pitch > 1.5f) cam.pitch = 1.5f; if (cam.pitch < -1.5f) cam.pitch = -1.5f;
        /* toggles */
        for (int k = 0; k < 3; k++) { int down = DBGKEY(VK_F1 + k); if (down && !f_prev[k]) { if (k == 0) L.rnd.show_world ^= 1; else if (k == 1) L.rnd.show_instances ^= 1; else L.rnd.wireframe ^= 1; } f_prev[k] = down; }
        {   /* F4 steps the visibility back: frustum + .vis -> frustum only -> the whole level every frame */
            static const char *cn[3] = { "off (whole level)", "frustum only", "frustum + .vis" };
            int down = DBGKEY(VK_F4), top = L.gel.nsectors ? (L.rnd.vis ? 2 : 1) : 0;
            if (down && !f_prev[3]) { L.rnd.cull = L.rnd.cull ? L.rnd.cull - 1 : top; L.rnd.sec_dirty = 1; printf("culling: %s\n", cn[L.rnd.cull]); }
            f_prev[3] = down;
        }
        if (DBGKEY('P') && !p_prev) dbg_paused ^= 1; p_prev = win.keys['P'];
        paused = dbg_paused || menu_pauses_world() || bb_active();                    /* the pause menu and its pages stop the world (table 0x405af8); App state 3 (BlackBox) runs 0x401ab0 with app+0xf4 |= 8, the world paused (0x40166e) */
        in_frame(&win, fly, now, now - t0);                                          /* the 14 actions of this frame (0x402940, docs/INPUT.md) */
        /* menu keys (docs/MENU_NEWGAME.md 1.3): confirm = Enter RELEASED or the jump key (action 4 + 0xc) pressed; back = Esc released
         * or the duck key (action 5) pressed (or Backspace); Esc (action 9) released also leaves page 0 and skips the intro, like the attack key released */
        MenuKeys mk; memset(&mk, 0, sizeof mk);
        {
            #define PRS(k) (win.keys[k] && !key_prev[k])
            #define REL(k) (!win.keys[k] && key_prev[k])
            int synth = enter_at >= 0 && ((now - t0 >= enter_at && now - t0 < enter_at + 0.1) || (wenv("WOODY_ENTER2") && now - t0 >= enter_at + 2 && now - t0 < enter_at + 2.1));
            mk.syn = synth && !l_prev; l_prev = synth;                              /* --enter T: New game at once (testing) */
            mk.ok = REL(VK_RETURN) || in_pressed(12); mk.back = REL(VK_ESCAPE) || in_pressed(5) || PRS(VK_BACK);   /* 0x4033fb / 0x4464f0: Enter and Esc are fixed (DIK codes) */
            mk.up = in_pressed(2) || PRS(VK_UP); mk.dn = in_pressed(3) || PRS(VK_DOWN); mk.left = in_pressed(0) || PRS(VK_LEFT); mk.right = in_pressed(1) || PRS(VK_RIGHT);   /* port: the arrows always work in menus, whatever the bindings */
            mk.esc_rel = in_released(9); mk.esc_prs = in_pressed(9); mk.atk_rel = in_released(6);
            ctl_capture(&win, key_prev, &mk, dt);                                   /* the Controls page waiting for a key or button */
            #undef PRS
            #undef REL
            for (int k = 0; k < 256; k++) key_prev[k] = win.keys[k];
        }
        if (DBGKEY(VK_TAB) && !tab_prev && sel) {                                   /* next instance with animations */
            Instance *nxt = NULL; int found = 0;
            for (uint32_t mi = 0; mi < g_ins.nmodels && !nxt; mi++) for (uint32_t k = 0; k < g_ins.models[mi].ninstances; k++) {
                Instance *c = &g_ins.models[mi].instances[k];
                if (found && c->model->nanims > 1) { nxt = c; break; }
                if (c == sel) found = 1;
            }
            if (nxt) sel = nxt; else sel = &g_ins.models[0].instances[0];
            printf("selected instance %u (model with %u nodes, %u anims, type %d)\n", sel->index, sel->model->nnodes, sel->model->nanims, sel->type);
        }
        tab_prev = win.keys[VK_TAB];
        int br[2] = { DBGKEY(VK_OEM_4), DBGKEY(VK_OEM_6) };
        if (sel && sel->model->nanims) {
            if (br[0] && !br_prev[0]) { sel->anim = (sel->anim + (int)sel->model->nanims - 1) % (int)sel->model->nanims; sel->anim_time = 0; printf("anim %d (%u frames, %.2f s)\n", sel->anim, sel->model->anims[sel->anim].nframes, sel->model->anims[sel->anim].duration_s); }
            if (br[1] && !br_prev[1]) { sel->anim = (sel->anim + 1) % (int)sel->model->nanims; sel->anim_time = 0; printf("anim %d (%u frames, %.2f s)\n", sel->anim, sel->model->anims[sel->anim].nframes, sel->model->anims[sel->anim].duration_s); }
        }
        br_prev[0] = br[0]; br_prev[1] = br[1];
        /* frame step 9 (0x401c06..0x401c63): this frame's instance list from the camera as it stands, before any Think; the region
         * list world+0xc0 goes along while the Perso is a rider (subtypes 4/5) */
        L.rnd.perso = L.have_player ? L.player.inst : NULL;                   /* 0x42b380 draws it outside the list, cinematic or not */
        rnd_instance_list(&L.rnd, &win, &cam, L.have_player && L.player.race_char ? L.rnd.race : NULL);
        static double pf[5]; static int pfn; static const int prof = 1; double pt0 = win_time();
        /* player (provisional controller) + follow camera */
        if (L.have_player && !paused) {
            PlayerInput pin = { 0 };
            pin.forward = in_held(2) || (now - t0 >= walk_at && now - t0 < walk_at + walk_for);
            if (wenv("WOODY_INSTLOG") && (wenv("WOODY_INSTLOG2") || (int)(now - t0) != (int)(now - t0 - dt))) { Instance *qi = slot_instance((uint32_t)atoi(wenv("WOODY_INSTLOG"))); if (qi) printf("instlog %u: visible %d fade %.2f type %d scripted %d anim %d pos %.0f %.0f %.0f model %d", qi->index, qi->visible, qi->fade, qi->type, qi->scripted, qi->anim, qi->position.x, qi->position.y, qi->position.z, (int)(qi->model - g_ins.models)), printf(" nw0 %.0f %.0f %.0f cull_r %.0f anim_time %.2f speed %.2f alpha? setflags %x", qi->node_world[0].m[12], qi->node_world[0].m[13], qi->node_world[0].m[14], qi->model->cull_r, qi->anim_time, qi->anim_speed, qi->setflags), printf(" quat %.2f %.2f %.2f %.2f", qi->quat.x, qi->quat.y, qi->quat.z, qi->quat.w), puts(""); }
            /* WOODY_UVLOG=<slot> or =stand (the instance the player is standing on, Perso+0x298): one UV report per
             * instance, to tell a wrong texture from a wrong projection on a surface that looks untextured */
            if (wenv("WOODY_UVLOG")) {
                const char *e = wenv("WOODY_UVLOG"); static const Instance *uv_done;
                const Instance *qi = e[0] >= '0' && e[0] <= '9' ? slot_instance((uint32_t)atoi(e)) : L.player.att_inst;
                if (qi && qi != uv_done) { uv_done = qi; rnd_uv_report(&L.rnd, qi); }
            }
            if (wenv("WOODY_POSLOG") && (int)((now - t0) * 4) != (int)((now - t0 - dt) * 4)) {
                /* dev = angle between the camera->player direction and his facing: 0 = camera exactly behind him, +-180 = in front of him */
                float ax = L.player.pos.x - cam.pos.x, az = L.player.pos.z - cam.pos.z, fx = sinf(L.player.yaw), fz = cosf(L.player.yaw);
                printf("pos t %.2f: %.0f %.0f %.0f yaw %.0f ground %d | cam %.0f %.0f %.0f yaw %.0f dev %.0f mode %d", now - t0, L.player.pos.x, L.player.pos.y, L.player.pos.z, L.player.yaw * 57.3f, L.player.on_ground,
                       cam.pos.x, cam.pos.y, cam.pos.z, cam.yaw * 57.3f, atan2f(fx * az - fz * ax, fx * ax + fz * az) * 57.3f, g_cam.mode), puts("");
            }
            pin.back = in_held(3); pin.left = in_held(0); pin.right = in_held(1);
            pin.ax = in_held(0) ? g_in.val[0] : g_in.val[1]; pin.az = -(in_held(2) ? g_in.val[2] : g_in.val[3]);   /* 0x45a4b0: x = action 0 if held, else 1; y = 2, else 3 */
            pin.jump = in_held(4) || (jump_at >= 0 && now - t0 >= jump_at && now - t0 < jump_at + jump_len) || (jump2_at >= 0 && now - t0 >= jump2_at && now - t0 < jump2_at + jump2_len); pin.action = in_held(6) || (peck_at >= 0 && now - t0 >= peck_at && now - t0 < peck_at + peck_len);
            pin.special = in_held(11) || (special_at >= 0 && now - t0 >= special_at - 0.1 && now - t0 < special_at);   /* action 11 (RCtrl in the original); it fires on the release */
            pin.duck = in_held(L.player.race_char ? 8 : 5) || (duck_at >= 0 && now - t0 >= duck_at && now - t0 < duck_at + duck_len);   /* action 5, while riding action 8 (0x465b10; Space / LShift in the original's Woody.cfg, X in the port) */
            pin.look = !fly && in_held(7);                                          /* action 7 (Enter / V / joystick button 4 by default, docs/INPUT.md), on release */
            {   /* the look-around's mouse counts (0x459346 reads [0x5e6190] vt[2] / vt[3] = DIMOUSESTATE lX / lY). The original never polls
                 * its mouse (docs/INPUT.md 1.3: nothing calls vt[1] 0x467cc0 = GetDeviceState), so its counts never change and the mouse
                 * does nothing: the port gives 0 too. WOODY_LOOKMOUSE=1 (port extra) feeds the raw relative counts of this frame in
                 * instead, through the original's formula; WOODY_MOUSE="T:DX:DY[:D] ..." adds DX, DY counts per frame for D s (default
                 * 0.5) from T s on (testing, implies WOODY_LOOKMOUSE) */
                static int lm = -1; if (lm < 0) lm = (wenv("WOODY_LOOKMOUSE") && atoi(wenv("WOODY_LOOKMOUSE"))) || wenv("WOODY_MOUSE");
                if (lm) { pin.mouse_dx = win.raw_dx; pin.mouse_dy = win.raw_dy; }
                if (g_in.pad.kind != PADK_NONE) {                    /* PORT EXTRA: the right stick of a pad aims the look-around, worth what the left stick is (ftol(value * 5)) */
                    pin.mouse_dx += (int)(in_deadzone(g_in.pad.rx, g_pad_dz * 0.01f) * 5.0f); pin.mouse_dy += (int)(in_deadzone(g_in.pad.ry, g_pad_dz * 0.01f) * -5.0f); }
                /* PORT EXTRA: outside the look-around the right stick turns / raises the follow camera (player_camera) */
                int orbit = g_in.pad.kind != PADK_NONE && !pin.look && !L.player.look;
                L.player.cam_orbit_x = orbit ? in_deadzone(g_in.pad.rx, g_pad_dz * 0.01f) * g_cam_speed * 0.01f : 0;   /* scaled by the Camera speed option */
                L.player.cam_orbit_y = orbit ? in_deadzone(g_in.pad.ry, g_pad_dz * 0.01f) * g_cam_speed * 0.01f : 0;
                for (const char *e = wenv("WOODY_MOUSE"); e && *e; ) {
                    double t, d = 0.5; int dx, dy, n = 0;
                    if (sscanf(e, "%lf:%d:%d%n", &t, &dx, &dy, &n) < 3) break;
                    e += n; if (*e == ':') { int n2 = 0; sscanf(e + 1, "%lf%n", &d, &n2); e += 1 + n2; }
                    if (now - t0 >= t && now - t0 < t + d) { pin.mouse_dx += dx; pin.mouse_dy += dy; }
                    while (*e == ' ') e++;
                }
            }
            {   /* WOODY_PECKS="T1 T2 ...": more attack taps of 0.1 s (testing: dispenser, pick up, throw) */
                static double pk[16]; static int npk = -1; if (npk < 0) { npk = 0; const char *e = wenv("WOODY_PECKS"); while (e && *e && npk < 16) { char *q; double v = strtod(e, &q); if (q == e) break; pk[npk++] = v; e = q; } }
                for (int k = 0; k < npk; k++) if (now - t0 >= pk[k] && now - t0 < pk[k] + 0.1) pin.action = 1;
            }
            /* Perso state 9 (the results sequence, docs/PERSO_STATE9.md) keeps its input, as in the original: the Perso is in a scripted action
             * there (player_script_action), so jump / walk / attack do nothing, but ducking, the look-around and special refusals and the
             * key tests of the script messages 1048..1050 see the keys, while the page reads the same ones as its confirm */
            if (g_level == 0 && !fly) {                                             /* title: House is the backdrop of the menu pages (docs/GAMEFLOW.md 5) */
                memset(&pin, 0, sizeof pin);
                if (g_pose && !g_cin.state) { L.player.pos = g_pose->position; L.player.yaw = inst_yaw(g_pose); L.player.vel = (Vec3){ 0, 0, 0 }; }
                /* 0x44e690: outside the cinematic Woody is invisible and plays action 0x49 = animation 73 at speed 3 (10 s loop), whose camera track is the orbit */
                L.player.inst->visible = g_cin.state == 2 || g_cin.state == 3;
                g_title_t += dt;
            }
            for (int k = 0; k < 3; k++) g_act_prev[k] = g_act_now[k];
            g_act_now[0] = pin.left; g_act_now[1] = pin.right; g_act_now[2] = pin.action;
            if (g_level != 0) {                                                     /* the Perso writes straight into the save struct (0x44c800 / 0x44c840), through setters, so the */
                g_save.chr[g_char].lives = L.player.lives; g_save.chr[g_char].health = L.player.health;   /* idle House Perso never does: copying it every frame there overwrote the slot */
                g_save.chr[g_char].unique = L.player.unique_items; g_save.chr[g_char].charges = L.player.special_charges;   /* "Load game" had just put in g_save */
            }
            if (g_cam.mode == 4 && !fly) memset(&pin, 0, sizeof pin);              /* cinematic camera: the player is frozen (0x459090) */
            cin_update(&L.vm, dt, g_now);
            rockets_update(dt, &L.player, L.have_player && !fly);
            L.player.idle_hold = g_res.on || g_level == 0 || (g_cam.mode == 4 && !fly);   /* Perso state 9 / title / frozen (0x459090): no idle count, no sleeping */
            L.player.app_menu = (g_res.on && g_res.state < 5) || M.page >= 0;      /* App+0 == 0: 1140 opens page 0x1e with App_SetState(0) (0x404df0), the save
                                                                                     * pages keep it, "No" / Continue go back to 1 with 0x454050 (results state 5) */
            L.player.cam_mode = fly ? 0x100 : g_cam.mode;                         /* CamMgr+0x134 as the Perso sees it (0x44b9a6, 0x44ba26); F5 = the debug camera 0x100 */
            L.player.side_on = g_cam.plane_on;                                     /* Perso+0x4ec: 0x459c70 runs while it is set (0x44b7be) */
            if (!cin_running()) player_update(&L.player, &pin, dt, &L.vm, fly ? cam.yaw : L.player.cam_yaw);
            if (!paused && !cin_running()) bombs_update(dt);                        /* 0x44d820, frame step 18: after the Perso */
            if (!fly) {                                                             /* 0x4459c0 (frame step 33), also during a cinematic: iris, death, respawn */
                int gs = L.player.game_state;
                storm_update(dt, &L.player, cin_running() || g_cam.mode == 4);    /* 0x451cc0 runs first; states 0 and 3 (dead) stop the storm (0x445a04, 0x445a8c) */
                player_game_tick(&L.player, &L.vm, dt);
                if (gs == 0 || gs == 3) storm_stop();
            }
            player_sync_board(&L.player);
            if (L.player.race_cam_req) { L.player.race_cam_req = 0; g_cam.cut = 1; cam_set_mode(1); L.player.cam_init = 0; }   /* race sub-state 0: 0x41f9f0(2) + SetMode(0, 0) */
            if (L.player.fade_req) { fade_start(0.5f, L.player.fade_req == 1); L.player.fade_req = 0; }           /* door actions 17 / 18 */
            if (L.player.cam_end_req) { L.player.cam_end_req = 0;                                                    /* end of door action 18: 0x44e5a0 = 0x41f9d0(0.5), 0x41f9f0(1), SetMode(0, 0) */
                /* but only when the side view is off: 0x44dcf1 skips it while Perso+0x4ec is set, and the script turns
                 * that on again (message 1088) in the same frame as the end of the action for a door into a side section */
                if (!g_cam.plane_on) { g_cam.dur = 0.5f; g_cam.dur_from_speed = 0; g_cam.cut = 0; cam_set_mode(1); } }
            if (L.player.respawn_req) { L.player.respawn_req = 0;                                                  /* respawn 0x445930 (docs/PERSO_DEATH.md 3.4) */
                /* 0x44a810 -> Reset 0x44ab20 clears Perso+0x4ec (0x44ad22), so a death in a side section ends its plane lock; left on,
                 * it pulled the checkpoint position onto the plane, into the void next to the section, and Woody died again and again
                 * (issue #39). Then 0x458f90: 0x41f9f0(2) + SetMode(0, 0) = hard cut to the follow camera, whatever mode was running
                 * (the side view, the death camera of 0x459030 / 0x41fb50, a script camera). The script turns the side view on again
                 * only through the section's own door (1088), exactly as the first time. This runs before the plane projection below. */
                plane_release(); g_cam.death_cam = 0; cam_hard_reset(); bombs_discard_all(); }   /* 0x44ab20 -> 0x44db10: every bomb out goes, unexploded */
            if (L.player.gameover_req) { L.player.gameover_req = 0;                                                /* 0x44a8f5: 0x404e10 = SetPage(0x1d) + App_SetState(0) */
                g_cam.death_cam = 0; cam_hard_reset(); menu_enter(0x1d); }                                          /* 0x445930 still ends with 0x458f90 (no Reset, so the plane lock stays) */
            if (g_res.on) results_update(&L.vm, dt, mk.ok);      /* 0x454090: after the action tick, so a finished action starts the next one in the same frame */
            if (g_cam.mode != 0x20 && (L.player.dead_cam_req || (L.player.dead_kind == 7 && !g_cam.death_cam))) {     /* 0x41fb50: kind 1 is watched from where he hung (+100), kind 7 from where the camera is */
                g_cam.fix_pos = L.player.dead_kind == 7 ? g_cam.pos : (Vec3){ L.player.pos.x, L.player.pos.y + 100.0f, L.player.pos.z };
                g_cam.fix_target = L.player.inst; g_cam.fix_f = g_cam.look_off.y;
                g_cam.speed = 100.0f; g_cam.dur_from_speed = 1; g_cam.cut = 0;  /* 0x41f9b0(100) + 0x41f9f0(1): a smooth travel at 100 u/s (the race kind 1 asks every frame of the hang) */
                cam_set_mode(2); g_cam.death_cam = 1;
            }
            L.player.dead_cam_req = 0;
            if (!cin_running()) enemies_update(&g_enemies, &L.player, cam.pos, dt);
            /* the plane lock itself (0x459eb0) is the last step of Perso_Move in player.c: a pull of the displacement onto the plane, at most
             * |disp| per frame, before the collision - not a hard projection here (docs/CAMERA_SCRIPT.md 4.2.1) */
            if (L.player.look != g_cam.look_prev) {                               /* 0x4590fb: the camera controller follows a change of Perso state 3 */
                if (L.player.look) { player_look_start(&L.player); g_cam.cut = 1; cam_set_mode(0x200); }   /* 0x459050 */
                else if (!L.player.script_act) { g_cam.cut = 1; cam_set_mode(1); }     /* 0x45910e (not into state 5): 0x41f9f0(2), +0x368 = 0, SetMode(0, 0) */
                g_cam.look_prev = L.player.look;
            }
            if (!fly) cam_update(&L.player, &cam, dt, g_cam.mode == 0x20 ? (pin.forward ? 2 : (pin.back || pin.duck) ? 3 : 0) : in_held(10)); else cam.letterbox = 0;
            if (g_cam.actor) { g_cam.actor->position = cam.pos; player_volumes_actor(&L.player, &L.vm, cam.pos, g_cam.actor->id); }   /* 0x41f379: the message-800 camera object is a volume actor */
            if (g_level == 0 && !fly && g_cin.state < 2) {                           /* title orbit: camera mode 0x80 on the Perso's animation 73 (docs/TITLE.md 2): no letterbox, vfov 83.97, no smoothing */
                Vec3 eye, tgt; float ph = fmodf(g_title_t / 10.0f, 1.0f);
                if (ins_camera_eval(L.player.inst, 73, ph, &eye, &tgt)) {
                    Vec3 to = { tgt.x - eye.x, tgt.y - eye.y, tgt.z - eye.z };
                    cam.pos = eye; cam.yaw = atan2f(to.x, to.z); cam.pitch = atan2f(to.y, sqrtf(to.x * to.x + to.z * to.z)); cam.letterbox = 0; cam.fov_deg = 83.97f;
                }
            }
            if (jump_at >= 0) { if (now - t0 < jump_at) start_y = L.player.pos.y; else if (L.player.pos.y > max_y) { max_y = L.player.pos.y; printf("jump apex so far %.1f above start at t=%.2f (jumper state %d)\n", max_y - start_y, now - t0 - jump_at, L.player.jumper.state); } }
        }
        double pt1 = win_time();
        /* VM tick: time in 1/100 s like the original */
        if (!paused) {
            L.vm.defer_msgs = 1;
            eko_tick(&L.vm, (int32_t)((g_clock - dt) * 100.0));   /* 0x401a0c writes the VM clock AFTER the tick, so a tick always runs on the value of the previous frame */
            L.vm.defer_msgs = 0;
            /* 0x401a14..0x401a63: the tick only queues its messages; the game takes the queue afterwards and 0x441d40 empties it. So a
             * variable a handler writes (1082, 1084, 1085, ...) is not yet visible to the objects that run later in the same tick: the WWS
             * gate reveal (object 297) relies on it - 1085 LevelIsDone only reaches var 54 a tick later, so its "3 [45, 0, 0, 10]" (gate
             * closed) comes AFTER object 45's "3 [45, 0, 1, 1]" (gate open, woken by 1082 of object 287) instead of before it */
            { int n = L.vm.nmsgs; for (int i = 0; i < n; i++) on_msg(&L.vm, &L.vm.msgs[i], NULL); eko_msg_reset(&L.vm); }
            { int n = g_nretry; g_nretry = 0; for (int i = 0; i < n; i++) { Instance *ri = slot_instance(g_retry[i].args[0]); if (ri && inst_msg(ri, g_retry[i].id, g_retry[i].args, g_retry[i].nargs, g_now) && g_nretry < 32) g_retry[g_nretry++] = g_retry[i]; } }
            for (uint32_t mi = 0; mi < g_ins.nmodels; mi++) for (uint32_t k = 0; k < g_ins.models[mi].ninstances; k++) {
                Instance *ii = &g_ins.models[mi].instances[k];
                if (ii->scripted) inst_tick(ii, g_now, dt);
                else {
                    ii->anim_time += dt * ii->anim_speed;
                    /* the slot step belongs to the clock 0x43eee0 (0x43f0c9), so it comes before the pose and before the event scan
                     * 0x42f5e0: a chain part the clock just ended moves on now. After the scan, the scan read the clock gone past
                     * the end as a wrap of the part and collected its t = 0 events again (the get-up .ins 16 of the peck dive's
                     * rebound, logical 0xd = 16 -> 0, played its sound twice) */
                    if (L.have_player && L.player.inst && !L.player.inst->scripted) player_anim_settle(&L.player, ii);
                    enemies_anim_settle(&g_enemies, ii);
                }
                anim_sounds(ii);
            }
            g_snd_frame++;
        }
        if (!paused) ambient_update(dt);                                          /* class 90 thinks (ambient.c): new butterflies / motes / rain drops */
        if (!paused) water_update(dt, g_player);
        if (!paused) launchers_update((float)g_now, dt, &L.player, &L.gel, L.have_player && !fly && !L.player.dead_kind && !cin_running());
        if (!paused && !cin_running()) bombs_fly(dt, &L.gel);                       /* the carrying projectiles, 0x4490f0 after the VM */
        double pt2 = win_time();
        if (g_level != 0 && M.page < 0 && mk.esc_prs && L.have_player && !fly && !g_res.on && !cin_running() && g_next_level < 0 && (L.player.game_state == 2 || g_level == 25)) { pause_open(); paused = 1; }   /* 0x403331: action 9, in state 1 only while 0x445980 (Game state 2) */
        if (L.have_player && !fly) { menu_update(&L.vm, &mk, dt); carousel_frame(&cam, dt); }   /* the carousel sits in front of the final title camera */
        uniq_update();                                                                 /* 0x44f770 */
        if (M.quitting && (M.quit_t -= dt) <= 0) win.quit = 1;                       /* 0x404cb0 -> app+4 */
        { Vec3 cr = cam_right(&cam); audio_listener(&cam.pos.x, &cr.x); audio_pause(bb_active() ? dbg_paused || M.page >= 0 : paused); }   /* the listener is the camera (mgr+0x28); the SoundFx queue runs in BlackBox mode (0x401eb1: app+0xe4) */
        int vx, vy, vw, vh; disp_view(&win, &vx, &vy, &vw, &vh);                     /* port extra (docs/DISPLAY.md 3): the 3D view box, 4:3 or the whole window */
        Window view = win; view.vx = vx; view.vy = vy; view.width = vw; view.height = vh;
        postfx_begin(win.width, win.height);                                        /* port extra (postfx.c): MSAA / SMAA draw the 3D picture into an own target */
        rnd_frame(&L.rnd, &view, &cam, g_now);                 /* the same game clock as the instances: a texture override (message 16) starts on it */
        audio_update(snd_owner_active);                                             /* 0x401ee7: after the draw, with this frame's instance list */
        {   /* 2D layer (docs/HUD_TEXT.md 5.4): HUD, then the text box, then the fades. No HUD in menus, BlackBox, cinematics and the fall death camera (0x401e19) */
            {   /* pickups: no mesh, a pulsing sprite (50..110, period 1 s) 50 above the instance; type 34 sits on its animated volume node */
                Vec3 cr = cam_right(&cam), cf = cam_forward(&cam), cu = { cf.y * cr.z - cf.z * cr.y, cf.z * cr.x - cf.x * cr.z, cf.x * cr.y - cf.y * cr.x };
                if (cu.y < 0) { cu.x = -cu.x; cu.y = -cu.y; cu.z = -cu.z; }
                float w = sinf(3.14159265f * (float)fmod(now - t0, 2.0)), size = w * w * 60.0f + 50.0f;
                hud_world_sprites_begin(&cr.x, &cu.x);
                /* 0x42b400 runs the Updates over the frame's instance list in list order, so the halos are submitted (and, within
                 * one depth bucket, drawn) in that order; without a list (no renderer list yet) in model order */
                Instance *const *hl; uint32_t hn = game_instance_list(&hl), htot = 0;
                for (uint32_t mi = 0; mi < g_ins.nmodels; mi++) htot += g_ins.models[mi].ninstances;
                for (uint32_t hi = 0, mi = 0, k = 0; hl ? hi < hn : hi < htot; hi++) {
                    Instance *ii;
                    if (hl) ii = hl[hi];
                    else { while (k >= g_ins.models[mi].ninstances) { mi++; k = 0; } ii = &g_ins.models[mi].instances[k++]; }
                    if (!ii->visible || ii->fade > 0.98f || !game_enemy_thinks(ii)) continue;   /* the halo is the bonus Update vtbl[3]: listed instances only (0x42b400, BONUS.md 3.1) */
                    int n = ii->type == 30 ? 0 : ii->type == 35 ? 1 : ii->type == 34 ? 2 : ii->type == 36 ? 3 : ii->type == 37 || ii->type == 38 ? 4 : -1; if (n < 0) continue;
                    float p[3] = { ii->position.x, ii->position.y, ii->position.z };
                    if (ii->type == 34 && ii->node_world) { p[0] = ii->node_world[0].m[12]; p[1] = ii->node_world[0].m[13]; p[2] = ii->node_world[0].m[14]; }
                    hud_world_sprite(n, p, size);
                }
                hud_world_sprites_late();                                     /* the Perso (0x44b4a0) and the effects (0x46d040) come after the world draw */
                if (L.have_player && !fly && !cin_running() && g_level >= 1 && g_level <= 24) {
                    /* landing ring 0x44af90 (docs/PERSO_JUMP.md 5): fades in under Woody while he is in the air, out on the
                     * ground. Run by 0x44b4a0 only when not paused (PERSO_FRAME.md 1 step 24), so a paused frame has none */
                    static const float grey06[3] = { 0.6f, 0.6f, 0.6f };
                    Vec3 rp, rn; float ra;
                    if (!paused && player_landing_ring(&L.player, dt, &rp, &rn, &ra)) hud_world_spr(0x3a, &rp.x, 80.0f, 0x40, grey06, ra, 6, &rn.x, 0);
                }
                water_fx_draw(); storm_fx_draw(&cam.pos.x, paused ? 0 : dt);
                for (int li = 0; li < g_nlasers; li++) {                            /* Lazer_Draw 0x46e530: core (1,.7,.7) width 6 + glow (1,.4,.4) width 30 pulsing 0.5..1, ends fade over 70, then laser_fx_draw */
                    Laser *z = &g_lasers[li]; if (!z->on || !z->inst->visible) continue;
                    if (!paused) z->phase += dt * 127.75f;
                    float g = 0.5f - 0.5f * cosf(2 * 3.14159265f * (float)(((int)z->phase % 254 + 0x80) & 0x1ff) / 512.0f);
                    for (uint32_t mk = 0; mk < 8; mk++) {
                        Vec3 a, b; int kind; if (!laser_segment(z, mk, &L.gel, &a, &b, &kind)) break;
                        if (L.have_player && !paused && !fly && !L.player.dead_kind && !cin_running() && laser_hits_player(a, b, &L.player)) player_kill(&L.player, 2);
                        Vec3 d = { b.x - a.x, b.y - a.y, b.z - a.z }; float l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
                        if (l < 1) { laser_fx_draw(&z->fx[mk], a, b, kind, g, &cam.pos.x, paused ? 0 : dt); continue; }   /* hit kind 3: b = a, only the impact at a */
                        static const float core[3] = { 1, 0.7f, 0.7f }, glow[3] = { 1, 0.4f, 0.4f };
                        float f = kind == 2 ? 0 : (l > 140 ? 70.0f : l * 0.5f) / l;
                        Vec3 a1 = { a.x + d.x * f, a.y + d.y * f, a.z + d.z * f }, b1 = { b.x - d.x * f, b.y - d.y * f, b.z - d.z * f };
                        for (int layer = 0; layer < 2; layer++) {
                            const float *c = layer ? glow : core; float hw = layer ? 30.0f : 6.0f, al = layer ? g : 1.0f;
                            if (f > 0) { hud_world_beam(&a.x, &a1.x, &cam.pos.x, hw, c, 0, al); hud_world_beam(&b1.x, &b.x, &cam.pos.x, hw, c, al, 0); }
                            hud_world_beam(&a1.x, &b1.x, &cam.pos.x, hw, c, al, al);
                        }
                        laser_fx_draw(&z->fx[mk], a, b, kind, g, &cam.pos.x, paused ? 0 : dt);   /* pulse, lightning arc, impact */
                    }
                }
                launchers_draw(&cam.pos.x, paused ? 0 : dt); stars_draw(paused ? 0 : dt); bubbles_draw(&cam, paused ? 0 : dt); rockets_draw(paused ? 0 : dt); bombs_draw(&cam.pos.x, paused ? 0 : dt); fx_smoke_draw(paused ? 0 : dt); boss_fx_draw(paused ? 0 : dt); board_fx_draw(paused ? 0 : dt); g_fx_fwd = cam_forward(&cam); torch_update(paused ? 0 : dt); fx_update(paused ? 0 : dt, &cam.pos.x);
                hud_world_sprites_end();
                rnd_sorted(&L.rnd);                                           /* 0x428d00: fade list, glow faces and these sprites in depth buckets (docs/MODEL_RENDER.md 10) */
            }
            postfx_end();                                                /* the 3D picture into the window (resolved, smoothed); the 2D layer stays sharp */
            if (g_black_frame || (g_sfade.hold && !(g_sfade.rest > 0))) { rnd_fade(0); g_black_frame = 0; }                /* 1152 blanks the 3D picture only: the House intro shows its text on black */
            if (L.have_player && now - t0 >= pick_at - 0.5) {            /* set the counters a few frames early, so the HUD sees them change like it would in play */
                if (pre_bonus >= 0) { L.player.bonus_count = pre_bonus; pre_bonus = -1; }
                if (pre_health >= 0) { L.player.health = pre_health; pre_health = -1; }
            }
            if (pick_type && now - t0 >= pick_at && L.have_player) {     /* --pickup: the same path a script pickup takes, 400 units in front of the camera */
                Vec3 f = cam_forward(&cam), fp = { cam.pos.x + f.x * 400, cam.pos.y + f.y * 400, cam.pos.z + f.z * 400 };
                int t = pick_type, fx = t == 30 ? 0 : t == 35 ? 1 : t == 34 ? 2 : t == 36 ? 3 : (t == 37 || t == 38) ? 4 : -1;
                int kind = t == 30 ? 1 : t == 36 ? 2 : t == 35 ? 3 : t == 34 ? 4 : t == 37 ? 5 : 0;
                player_collect(&L.player, t, 300);
                if (fx >= 0) game_pickup_fx(fx, fp);
                if (kind && g_npick < 8) { g_pick[g_npick].kind = kind; g_pick[g_npick].pos = fp; g_npick++; }
                pick_type = 0;
            }
            if (door_at >= 0 && now - t0 >= door_at && L.have_player) {  /* --door: the message a door script sends, on the real handler */
                EkoMsg dm; memset(&dm, 0, sizeof dm);
                dm.id = 1040; dm.nargs = 2; dm.args[0] = 0x1000000u | (uint32_t)door_inst; dm.args[1] = (uint32_t)door_act;
                on_msg(&L.vm, &dm, NULL); door_at = -1;
                printf("  --door: 1040 [inst %d, action %d]", door_inst, door_act), puts("");
            }
            for (int i = 0; i < g_npick; i++) {                          /* 0x448510: the flight starts from where the bonus was on screen */
                float sc[2]; int on = rnd_project(&view, &cam, g_pick[i].pos, &sc[0], &sc[1]);
                if (on && vw * 3 > vh * 4) { float hw = 240.0f * vw / vh; sc[0] = 320.0f - hw + sc[0] / 640.0f * 2.0f * hw; }   /* 0..640 over a wide view -> the HUD's virtual x (port extra) */
                hud_anim_pickup(g_pick[i].kind, on ? sc : NULL, g_char);
            }
            g_npick = 0;
            hud_begin_view(vx, vy, vw, vh);                              /* port extra: 640x480 kept 4:3 and centred (docs/DISPLAY.md 3) */
            storm_overlay_draw(paused, dt);                              /* 0x46e0d0: after the effects (0x46d040), before the HUD */
            if (bb_active()) {                                           /* 0x401d1b: the BlackBox object (app+0xe4) every frame, frozen while a menu page is open (App state 0) */
                int held[7]; for (int k = 0; k < 7; k++) held[k] = in_held(k);
                if (bb_frame(M.page >= 0, dt, held) && g_next_level < 0) request_level(26, 0.5f);   /* 0x401d35: done -> RequestLevel(0.5, 0x1a, 0, 0x20), the credits: page 0x20 comes with every load of level 26 */
            }
            if (L.have_player && !fly)                                   /* 0x448450: 1 on the pause pages (extended HUD), 2 hidden on every other page and the results (0x404e9d) */
                hud_state(g_res.on ? 2 : M.page < 0 ? 0 : (M.page == 0x18 || M.page == 0x19) ? 1 : 2, L.player.inst->type == 18 || L.player.inst->type == 19);
            if (L.have_player && !fly && g_level >= 1 && g_level <= 24 && !cin_running() && !g_res.on && (M.page < 0 || M.page == 0x18 || M.page == 0x19) && (!g_cam.death_cam || g_hud_ext) && !wenv("WOODY_NOHUD")) {
                const Player *pl = &L.player; int race = pl->inst->type == 18 || pl->inst->type == 19;
                HudState hs = { g_char, race, pl->lives, race ? pl->race_bonus : pl->bonus_count, race ? pl->race_bonus : pl->bonus_got, race ? pl->race_total : pl->bonus_total,   /* 0x4477dc: [0x5e54f4] in race mode */
                                g_level != 1 && g_level != 11 && g_level != 18, pl->unique_items, pl->special_charges, g_hud_ext, pl->health, pl->charge * (2.0f / 3.0f) };
                hud_draw(&hs, dt);
                if (g_bossbar.on) { hud_boss_bar(g_bossbar.cur, g_bossbar.max, g_bossbar.t); if (!paused) g_bossbar.t += dt; }   /* hud+0x48: drawn by the HUD animator after the HUD */
            }
            if (L.have_player && !fly && !paused && L.player.iris_on) hud_iris(L.player.iris);   /* the Game iris 0x477920, drawn by 0x4459c0 after the HUD (0x401e55) and before the text box */
            {   /* menu page 0x1e runs from 1140 until page 6 takes over in state 4 (docs/RESULTS.md) */
                static int ticking;
                int counting = 0;
                if (g_res.on && g_res.state <= 3) {
                    HudResults hr = { g_stats.level, g_res.race, { g_stats.stats[0], g_stats.stats[1], g_stats.stats[2], g_stats.stats[3] }, g_stats.time, g_res.best, results_cats(g_stats.stats) };
                    counting = hud_results_draw(&hr, dt);
                }
                if (counting != ticking) { if (counting) audio_fx(61, &g_res, NULL); else audio_fx_stop(61, &g_res, 0); ticking = counting; }   /* 0x468e50: the tick loop SoundFx 0x3d */
            }
            if (L.have_player && !fly) menu_draw(dt);
            { uint32_t v = g_text_var & 0xffffff; hud_text_draw(v < L.vm.nvars && L.vm.varval[v] != 0, paused ? 0 : dt); }
            hud_end(); g_hud_ext = 0;
        }
        /* level change: PgUp / PgDn cycle through the levels (debug); a request fades out, swaps the level, fades in */
        for (int k = 0; k < 2; k++) { int down = DBGKEY(k ? VK_NEXT : VK_PRIOR); if (down && !pg_prev[k]) { int cur = g_level >= 0 && g_level < 27 ? g_level : 0; request_level((cur + (k ? 1 : 26)) % 27, 0.5f); } pg_prev[k] = down; }
        { static int side_done; if (wenv("WOODY_SIDE") && now - t0 >= 1.0 && !side_done && L.have_player) { side_done = 1; Instance *si = slot_instance(0x1000000 | (uint32_t)strtol(wenv("WOODY_SIDE"), NULL, 0)); if (si) cam_side_start(si, 2); } }   /* testing: force the side view on a marker instance */
        {   /* WOODY_SIDECHECK="slot:v slot:v ..." (testing, the 1088 pairs of the level script): for five points along the marker A-B, is the
             * line from the side camera (A..B + 340 up, 1000 to the side, the sign of 0x424bf0) to the body (+100) blocked by the world - and
             * for the opposite sign? The side the level design means is the one the walls leave open. */
            static int sc_done; const char *e = wenv("WOODY_SIDECHECK");
            if (e && !sc_done && now - t0 >= 0.5 && L.have_player) { sc_done = 1;
                for (const char *s = e; *s; ) { char *q; long slot = strtol(s, &q, 0); if (q == s) break; long v = *q == ':' ? strtol(q + 1, &q, 0) : 2; s = q; while (*s == ' ') s++;
                    Instance *in = slot_instance(0x1000000 | (uint32_t)slot); if (!in) continue; const Model *mo = in->model; int node = -1;
                    for (uint32_t i = 0; i < mo->nnodes && node < 0; i++) if (mo->nodes[i].kind == 0x20 && mo->nodes[i].type_code == 0 && mo->nodes[i].npoints >= 2) node = (int)i;
                    if (node < 0) { printf("SIDECHECK %ld: no marker\n", slot); continue; }
                    ins_pose(in, in->anim, in->anim_time);
                    Vec3 A = ins_point_world(in, mo->nodes[node].point_base), B = ins_point_world(in, mo->nodes[node].point_base + 1), d = { B.x - A.x, 0, B.z - A.z };
                    float l = sqrtf(d.x * d.x + d.z * d.z); if (l < 0.01f) d = (Vec3){ 1, 0, 0 }; else { d.x /= l; d.z /= l; }
                    int blk[2] = { 0, 0 };
                    for (int k = 0; k < 5; k++) for (int f = 0; f < 2; f++) {
                        float u = k / 4.0f, sg = (v == 1 ? -1.0f : 1.0f) * (f ? -1.0f : 1.0f);
                        Vec3 M = { A.x + (B.x - A.x) * u, A.y + (B.y - A.y) * u, A.z + (B.z - A.z) * u }, body = { M.x, M.y + 100.0f, M.z }, P = { M.x - d.z * 1000.0f * sg, M.y + 340.0f, M.z + d.x * 1000.0f * sg };
                        if (gel_ray_frac(L.player.gel, P, body) <= 1.0f) blk[f]++;
                    }
                    printf("SIDECHECK %ld v %ld: A %.0f %.0f %.0f B %.0f %.0f %.0f | engine side blocked %d/5, opposite side blocked %d/5\n", slot, v, A.x, A.y, A.z, B.x, B.y, B.z, blk[0], blk[1]);
                }
            }
        }
        { static int posat_done; float pa[4]; const char *e = wenv("WOODY_POSAT"); int k = 0, used;   /* testing: WOODY_POSAT="T x y z [T x y z ...]" = --pos, but T s into the level (moving platforms, carrying a bomb somewhere) */
            while (e && L.have_player && sscanf(e, "%f %f %f %f%n", &pa[0], &pa[1], &pa[2], &pa[3], &used) == 4) { if (!(posat_done >> k & 1) && now - t0 >= pa[0]) { posat_done |= 1 << k; L.player.pos = (Vec3){ pa[1], pa[2], pa[3] }; L.player.floor_y = pa[2] - 1000.0f; L.player.on_ground = 0; } e += used; k++; } }
        { static int aw_done; if (wenv("WOODY_AIRWIN") && !aw_done && L.have_player && now - t0 >= atof(wenv("WOODY_AIRWIN"))) { aw_done = 1; L.player.air_win = 0.5f; } }   /* testing: open the air attack window (an air peck without a jump, after WOODY_POSAT) */
        { static int setvar_done; float sv[3]; const char *e = wenv("WOODY_SETVAR"); int k = 0, used;   /* testing: WOODY_SETVAR="T var val [...]" = SetVar T s into the level (W2B boss fight: "1 1 1") */
            while (e && L.have_player && sscanf(e, "%f %f %f%n", &sv[0], &sv[1], &sv[2], &used) == 3) { if (!(setvar_done >> k & 1) && now - t0 >= sv[0]) { setvar_done |= 1 << k; game_var_set((uint32_t)sv[1], (int)sv[2]); } e += used; k++; } }
        if (wenv("WOODY_DOLLAR")) { double a = 0, b = 0; sscanf(wenv("WOODY_DOLLAR"), "%lf %lf", &a, &b); if (now - t0 >= a && now - t0 < b) g_hud_ext = 1; }   /* testing: WOODY_DOLLAR="T0 T1" = message 1172 every frame between T0 and T1 s (the Jackpot door) */
        {   /* testing: WOODY_MSGAT="T id a0 a1 ...[; T id ...]" = send a script message T s into the level, once (a0 = slot number or
             * reference, e.g. "2 17 216 255 1 100" = message 17 to slot 216: messages no level script sends, like 15/17) */
            static int msgat_done; const char *e = wenv("WOODY_MSGAT"); int k = 0;
            while (e && *e && k < 31) {
                char *end; float T = strtof(e, &end); if (end == e) break; e = end;
                EkoMsg em; memset(&em, 0, sizeof em); em.id = (uint32_t)strtol(e, &end, 0); e = end;
                while (em.nargs < EKO_MAX_MSG_ARGS) { long v = strtol(e, &end, 0); if (end == e) break; em.args[em.nargs++] = (uint32_t)v; e = end; }
                while (*e == ' ' || *e == ';') e++;
                if (!(msgat_done >> k & 1) && now - t0 >= T) { msgat_done |= 1 << k; on_msg(&L.vm, &em, NULL); }
                k++;
            } }
        {   /* testing: WOODY_KILLAT="T" = the pit message 1020 (Kill(1) + death camera) T s into the level, once; for the respawn.
             * WOODY_KILLAT="T K" calls Kill(K) itself instead (K = 2: the laser death with its skeleton flash) */
            static int killat_done; if (wenv("WOODY_KILLAT") && !killat_done && L.have_player && now - t0 >= atof(wenv("WOODY_KILLAT"))) {
                int kk = 0; const char *ks = strchr(wenv("WOODY_KILLAT"), ' '); if (ks) kk = atoi(ks + 1);
                killat_done = 1; if (kk > 0) player_kill(&L.player, kk); else { EkoMsg em; memset(&em, 0, sizeof em); em.id = 1020; on_msg(&L.vm, &em, NULL); } } }
        {   /* testing: WOODY_ENEMYAT="T inst x y z" puts that enemy (start, home and position) on x y z T s into the level, once:
             * for the Press / UnPress of enemies on a world_collision (WOODY_COLLOG) */
            static int enat_done; float T, x, y, z; unsigned ii;
            if (wenv("WOODY_ENEMYAT") && !enat_done && sscanf(wenv("WOODY_ENEMYAT"), "%f %u %f %f %f", &T, &ii, &x, &y, &z) == 5 && now - t0 >= T) {
                enat_done = 1; for (int i = 0; i < g_enemies.n; i++) if (g_enemies.e[i].inst->index == ii) { Enemy *en = &g_enemies.e[i]; en->pos = en->home = en->start = (Vec3){ x, y, z }; en->vfall = 0; enemy_place(en); printf("  ENEMYAT %u -> %.0f %.0f %.0f", ii, x, y, z), puts(""); } }
            /* WOODY_BOMBAT="T x y z": a bomb of the pool dropped there (template 0, 8 s fuse, no floor snap) T s into the level, once */
            static int bat_done; if (wenv("WOODY_BOMBAT") && !bat_done && sscanf(wenv("WOODY_BOMBAT"), "%f %f %f %f", &T, &x, &y, &z) == 4 && now - t0 >= T) {
                BombT bt = BOMB_T0; bt.life = 8.0f; bt.speed = 0; bat_done = 1; bomb_start(&bt, (Vec3){ x, y, z }, (Vec3){ 0, -1, 0 }, 0, -1, 0); } }
        if ((next_name && now - t0 >= next_at && !strcmp(next_name, "END")) || (DBGKEY(VK_END) && !end_prev)) {   /* End key / --next END T: finish the level as its exit door does (message 1083) */
            EkoMsg em; memset(&em, 0, sizeof em); em.id = 1083; on_msg(&L.vm, &em, NULL); if (next_name && !strcmp(next_name, "END")) next_name = NULL;
        }
        end_prev = win.keys[VK_END];
        if (next_name && now - t0 >= next_at) { request_level(level_index(next_name), 0.5f); next_name = NULL; }
        if (g_next_level >= 0) { g_switch_fade -= dt / g_fade_len; if (g_switch_fade < 0) g_switch_fade = 0; } else if (g_switch_fade < 1) { g_switch_fade += dt / 1.0f; if (g_switch_fade > 1) g_switch_fade = 1; }   /* the level load fades in over 1.0 s (0x404332) */
        { float f = g_switch_fade;
          if (g_sfade.rest > 0 && g_sfade.total > 0) { float k = g_sfade.rest / g_sfade.total, b = g_sfade.out ? k : 1.0f - k; if (b < f) f = b; g_sfade.rest -= dt; if (g_sfade.rest <= 0 && g_sfade.out && !g_sfade.script) g_sfade.hold = 1; }
          /* a finished fade-out keeps the 3D picture black until the next fade-in; that is drawn under the 2D layer (above) */
          if (f < 1.0f) rnd_fade(f); }
        hud_bars(win.width, win.height, vx, vy, vw, vh);                              /* port extra: the pillar- / letterbox bars black */
        {   /* WOODY_SHOTSEQ="prefix start step count": a burst of screenshots prefix_NNN.ppm (testing: popping, flicker) */
            static char pre[200]; static double st, sp; static int cnt = -1, k; if (cnt < 0) { cnt = 0; if (wenv("WOODY_SHOTSEQ")) sscanf(wenv("WOODY_SHOTSEQ"), "%199s %lf %lf %d", pre, &st, &sp, &cnt); }
            if (k < cnt && now - t0 >= st + k * sp) { char fn[256]; snprintf(fn, sizeof fn, "%s_%03d.ppm", pre, k); rnd_screenshot(&win, fn); if (!k && wenv("WOODY_AUDIODUMP")) printf("shotseq: frame 0 at audio frame %lld\n", audio_dump_pos()); k++; }
        }
        if (shot_path && now - t0 >= shot_after) { rnd_screenshot(&win, shot_path); printf("screenshot -> %s\n", shot_path); win.quit = 1; }
        double pt3 = win_time();
        win_swap(&win);
        frames++;
        if (prof && wenv("WOODY_PROF")) { double pt4 = win_time(); pf[0] += pt1 - pt0; pf[1] += pt2 - pt1; pf[2] += pt3 - pt2; pf[3] += pt4 - pt3; if (++pfn == 60) { printf("PROF ms/frame: player+enemies+camera %.2f  vm+instances %.2f  render+2D %.2f  swap %.2f", pf[0] / 60 * 1000, pf[1] / 60 * 1000, pf[2] / 60 * 1000, pf[3] / 60 * 1000); puts(""); pf[0] = pf[1] = pf[2] = pf[3] = 0; pfn = 0; } }
        if (g_next_level >= 0 && g_switch_fade <= 0) {
            if (g_stats.have && g_stats.level == g_level) g_stats.stats[2] = g_enemies.killed;   /* 0x40177c: App::Frame copies [0x4c532c] to app+0x7c again right before the unload 0x4049a0,
                                                                                    * so an enemy that finishes dying during the 0.5 s EndLevel fade still counts */
            const char *name = k_levels[g_next_level]; g_prev_level = g_level; g_level = g_next_level; g_next_level = -1;
            level_free(&L);
            if (level_load(&L, dir, name)) { fprintf(stderr, "level %s failed to load\n", name); return 1; }
            if (!L.have_player) fly = 1; else if (!have_cam) fly = 0;
            g_title_t = 0; hud_title_reset();
            t0 = L.t0; last = win_time(); sel = (g_ins.nmodels && g_ins.models[0].ninstances) ? &g_ins.models[0].instances[0] : NULL;
            lvl = L.name; continue;
        }
        if (now - fps_t > 2.0 && wenv("WOODY_FPSLOG")) printf("fps %.1f\n", frames / (now - fps_t));   /* testing: the frame cap / vsync */
        if (now - fps_t > 2.0) { char title[256]; snprintf(title, sizeof title, "WoodyRE%s - %s - %.0f fps - VM t=%d frame %u msgs %u - %s - woody %.0f %.0f %.0f %s - vol events %u - hearts %.0f lives %d bonus %d/%d", g_level == 0 ? " - TITLE: Enter = new game, L = continue" : "", lvl, frames / (now - fps_t), L.vm.time, L.vm.frame, L.vm.stat_msgs_total, fly ? "fly" : "play", L.player.pos.x, L.player.pos.y, L.player.pos.z, L.player.on_ground ? "ground" : "air", L.player.events_sent, L.player.health, L.player.lives, L.player.bonus_got, L.player.bonus_total); if (WOODY_DEBUG_TITLE) win_title(&win, title); if (L.have_player) printf("player t=%.1f pos %.0f %.0f %.0f vel %.0f %.0f %.0f %s floor %.0f cam %.0f %.0f %.0f\n", now - t0, L.player.pos.x, L.player.pos.y, L.player.pos.z, L.player.vel.x, L.player.vel.y, L.player.vel.z, L.player.on_ground ? (L.player.floor_is_hull ? "hull" : "ground") : "air", L.player.floor_y, cam.pos.x, cam.pos.y, cam.pos.z); frames = 0; fps_t = now; }
    }
    opt_write(); level_free(&L); audio_shutdown(); win_close(&win);   /* 0x401130: the cfg is written back at exit */
    return 0;
}

#if defined WOODY_GUI && defined _WIN32
/* the windowed release build (build.bat): no console; stdout goes to woodyre.log (main), a failed start says so. The
 * Android build is a WOODY_GUI build too, entered through SDL's SDL_main (android/app/CMakeLists.txt) */
int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    (void)hi; (void)hp; (void)cmd; (void)show;
    int r = main(__argc, __argv);
    if (r) { fflush(stdout); MessageBoxA(NULL, "WoodyRE could not start the game. Details are in woodyre.log.", "WoodyRE", MB_ICONERROR); }
    return r;
}
#endif
