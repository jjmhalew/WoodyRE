/* player.c - player controller ported from the decompiled Perso class (see player.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "player.h"
#include "audio.h"
#include "enemy.h"
#include "instance.h"
#include "pad.h"

/* Rumble 0x44d1b0(a, b): [0x5e618c]->vt[2](a, b), skipped in Perso state 2 (dead). The original's joystick method is an empty
 * `ret 8` (docs/MENU_OPTIONS.md 5.2); the port gives it to the pads of src/pad.c (PORT EXTRA): a = strength, b = seconds (the
 * charge run pulses (0.5, 0.15) every 0.2 s, the lightning gives (0.5, 1.5) to its 1.7 s stun). The values are P+0x98..0xd4. */
static void rumble(const Player *p, float strength, float seconds) { if (!p->dead_kind) pad_rumble(strength, seconds); }

/* ---- decompiled Perso parameters (docs/PERSO_FRAME.md 2.3 / 2.6, table 0x4b5f14, Woody column) ---- */
#define P_WALK_SPEED   600.0f     /* P+0x1c: RampA max speed, units/s */
#define P_ACC_TIME     0.25f      /* P+0x2c: RampA acceleration time, v = (t/T)^2 * target */
#define P_DEC_TIME     0.1f       /* P+0x30: RampA deceleration time, v = v0 * (1 - (t/T)^2) */
#define P_ICE_ACC_TIME 0.75f      /* P+0x34: RampA acceleration time on slippery ground (0x45a850) */
#define P_ICE_DEC_TIME 1.0f       /* P+0x38: ... and its deceleration time */
#define P_ICE_TURN     1.0f       /* P+0x3c: factor on the kept share of the old walking direction */
#define P_TURN_BLEND   0.25f      /* 0x4a9ca0: facing = slerp(old, wanted, |stick| * 0.25) per frame (0x45a320) */
#define P_REF_FPS      60.0f      /* the blend is per frame in the original; normalised to this rate here */
/* Jumper J = Perso+0x334 (docs/PERSO_JUMP.md 1): rise and fall are time parabolas, there is no gravity constant */
#define J_HEIGHT       380.0f     /* P+0x64: jump height H */
#define J_P68          650.0f     /* P+0x68: reference distance; x0.75 rising (0x4aabb4), x1.25 falling (0x4ab798) */
#define J_V            600.0f     /* P+0x6c */
#define J_TERMINAL     2000.0f    /* P+0x70: terminal fall speed */
#define J_REARM_H      100.0f     /* P+0x84: jump re-arms when the button is up and height above ground <= this */
#define J_COYOTE       0.15f      /* 0x4aa1c8 */
#define J_SHORT_HOP_T  -0.2f      /* 0x4aa430 */
#define J_APEX_T       -0.15f     /* 0x4ab7a0: state 1 -> 7 (attack window opens) */
#define J_HARD_FALL    1500.0f    /* P+0x7c */
#define P_SLIDE_SPEED  600.0f     /* P+0x40: RampB max, sliding down slopes with n.y < 0.71 (0x45aa60) */
#define P_SLIDE_TIME   0.25f      /* P+0x44 / P+0x48: RampB acceleration / deceleration time */
#define P_SLIDE_NY     0.71f      /* 0x4ab2d8 */
/* collision (docs/PERSO_MOVE.md 6) */
#define P_STEP         40.0f      /* 0x437180 step argument: clinging distance, wall test skips the lowest step+1 */
#define P_SUBSTEP      10.0f      /* 0x437180 substep length */
#define P_BODY_H       193.0f     /* P+0x0c: body height (61 when ducking) */
#define P_DUCK_H       61.0f      /* P+0x10: body height while ducked (docs/PERSO_DUCK.md 3.1) */
#define P_PROBE_Y      43.0f      /* P+0x00: ground probe / collision centre above the feet */
#define P_RADIUS       69.0f      /* P+0x04: horizontal collision radius (0x434820 in 0x4624f0) */
#define P_RACE_SPEED   1250.0f    /* P+0x1c of the race columns 3/4: the constant ride speed (docs/RACE.md 6) */
#define P_EDGE_LOOK    40.0f      /* P+0x80: how far ahead the ledge sensor 0x44b2e0 aims (docs/OBSTACLE.md 2) */
#define P_EDGE_T       3.0f       /* 0x4a988c: the sensor fires when the ray's first hit lies beyond 3 x its aim */
#define P_VOL_PROBE_Y  71.0f     /* volume test point above the feet: 0x462760(perso, 71.0), docs/EVENTS.md 2.1 */
/* follow camera (docs/CAMERA.md 3) */
#define CAM_HEIGHT     180.0f     /* camera y approaches T.y + 180 with 6*dt */
#define CAM_TARGET_Y   120.0f     /* position target T = player + (0,120,0) */
#define CAM_LOOK_Y     140.0f     /* look target = player + (0,140,0) */
#define CAM_RADIUS     40.0f
#define CAM_FOV_Y      83.97f     /* tan(hfov/2) = zoom 1.2 at 4:3 (0x41f690 / 0x4379a0) */
#define CAM_ORBIT_RATE 2.6f       /* PORT EXTRA, right stick: radians/s at full tilt (~150 deg/s) */
#define CAM_LIFT_RATE  400.0f     /* units/s of extra camera height at full tilt */
#define CAM_LIFT_MIN   (-120.0f)
#define CAM_LIFT_MAX   350.0f

static float vdot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 vsub(Vec3 a, Vec3 b) { Vec3 r = { a.x - b.x, a.y - b.y, a.z - b.z }; return r; }
static Vec3 vcross(Vec3 a, Vec3 b) { Vec3 r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; return r; }

/* point-in-polygon test in the plane's dominant projection */
static int poly_contains(const GelFile *g, const GelPoly *p, Vec3 q)
{
    float ax = fabsf(p->plane[0]), ay = fabsf(p->plane[1]), az = fabsf(p->plane[2]);
    int u = 0, v = 2;                                   /* project away the dominant axis */
    if (ay >= ax && ay >= az) { u = 0; v = 2; } else if (ax >= az) { u = 1; v = 2; } else { u = 0; v = 1; }
    const float *qq = &q.x; int sign = 0;
    for (uint32_t i = 0; i < p->nverts; i++) {
        const GelVert *a = &g->verts[p->indices[i]], *b = &g->verts[p->indices[(i + 1) % p->nverts]];
        const float *pa = &a->x, *pb = &b->x;
        float cr = (pb[u] - pa[u]) * (qq[v] - pa[v]) - (pb[v] - pa[v]) * (qq[u] - pa[u]);
        int s = cr > 0 ? 1 : (cr < 0 ? -1 : 0);
        if (!s) continue;
        if (!sign) sign = s; else if (s != sign) return 0;
    }
    return 1;
}

/* The query volume of one of the tests below, clamped to the level so an unbounded drop does not walk the
 * whole tree. Only the polygons of the kd leaves it meets can answer the test (docs/FORMAT_GEL.md 5). */
static void query_box(const GelFile *g, Vec3 c, float rxz, float ylo, float yhi, float box[6])
{
    if (ylo < g->bbox[2] - 1.0f) ylo = g->bbox[2] - 1.0f;
    if (yhi > g->bbox[3] + 1.0f) yhi = g->bbox[3] + 1.0f;
    if (yhi < ylo) yhi = ylo;
    box[0] = c.x - rxz; box[1] = c.x + rxz; box[2] = ylo; box[3] = yhi; box[4] = c.z - rxz; box[5] = c.z + rxz;
}

float gel_floor_below(const GelFile *g, Vec3 p, float step_up, float max_drop, int *found)
{
    float best = -1e30f, box[6]; *found = 0;
    query_box(g, p, 1.0f, p.y - max_drop, p.y + step_up, box);
    GelPolySet ps = gel_polys_in_box(g, box);
    for (uint32_t k = 0; k < ps.n; k++) {
        uint32_t i = ps.polys[k];
        const GelPoly *pl = &g->polys[i];
        if (pl->plane[1] < 0.5f || pl->nverts < 3) continue;          /* walkable: normal mostly up */
        float y = -(pl->plane[0] * p.x + pl->plane[2] * p.z + pl->plane[3]) / pl->plane[1];
        if (y > p.y + step_up || y < p.y - max_drop || y <= best) continue;
        Vec3 q = { p.x, y, p.z };
        if (poly_contains(g, pl, q)) { best = y; *found = 1; }
    }
    return best;
}

/* The instance tests below take their instances from the .col registration of the cells each query visits
 * (level.c gel_col_instances, docs/PERSO_MOVE.md 6.7) instead of a distance reject around the query point; the .col masks
 * follow an animated platform along its path (the W1B shuttles, model 42, 4400 units of travel: issue #27). */
static Vec3 g_ground_n = { 0, 1, 0 };   /* normal of the last world_ground() hit ([0x4b3108..10]) */
static int32_t g_ground_mat = -1;       /* material of that hit when it is a world polygon ([0x53a554] == 1, poly+8), else -1 */
static Vec3 g_ins_n;
/* the race board (message 1120) moves with its rider, so it must never be the rider's floor or wall: standing on its
 * hull lifted the board, which lifted him again, every frame */
static const Instance *g_rider, *g_rider_board;
/* the same for the instance a boss carries along (message 59, docs/BOSS14.md 9.1): the boss must not land on its own machine */
static const Instance *g_carrier, *g_carried, *g_skip_self;   /* g_skip_self: the player, while an actor asks for its own floor */
void player_set_carried(const Instance *owner, const Instance *follower) { g_carrier = owner; g_carried = follower; }
static int skip_inst(const Instance *in, const Instance *skip) { return in == skip || (skip && skip == g_rider && in == g_rider_board) || (skip && skip == g_carrier && in == g_carried) || in == g_skip_self; }
/* The loader's plane of a polygon (0x4280c2-0x428375): over every run of three consecutive vertices P, Q, R the longest
 * n = (R - Q) x (R - P) above 0.01, d = -n.R. Taken here from the world-space vertices: for a transform without a mirror
 * (no instance of the 28 levels has a negative scale) that is the same side as the node-space plane the original keeps in
 * poly+8..+0x14, and it is the true plane of the transformed polygon, also under a non-uniform scale. */
static int loader_plane(const Vec3 *v, uint32_t nv, float pl[4])
{
    float best = 0; int ok = 0;
    for (uint32_t t = 0; t < nv; t++) {
        Vec3 P = v[t], Q = v[(t + 1) % nv], R = v[(t + 2) % nv], n = vcross(vsub(R, Q), vsub(R, P)); float l = sqrtf(vdot(n, n));
        if (l > 0.01f && (!ok || l > best)) { best = l; ok = 1; pl[0] = n.x / l; pl[1] = n.y / l; pl[2] = n.z / l; pl[3] = -(pl[0] * R.x + pl[1] * R.y + pl[2] * R.z); }
    }
    return ok;
}
/* The normal the instance tests REPORT for a press-node polygon: the node-space loader normal times the node's world matrix,
 * normalised - vt[7] 0x4329a9 (-> [0x4c4bc0], the floor normal), vt[8] uniform branch 0x4333ed (times 1/scale) and
 * non-uniform branch 0x4336fd/0x433762 (normalised). With a non-uniform scale that is not the transformed polygon's true
 * normal (that would be M^-T n); the cylinder test uses it as the plane all the same. 276 press-node instances of the 28
 * levels have a non-uniform scale (e.g. WWS model 34, 2.11 x 2.43 x 0.50); on axis-aligned faces it makes no difference. */
static int press_normal(const Instance *in, const InsPoly *ip, Vec3 *out)
{
    const Model *m = in->model; float best = 0; int ok = 0; Vec3 nl = { 1, 0, 0 };
    int o = ins_point_owner(m, ip->indices[0]); if (o < 0 || !in->node_world) return 0;
    for (uint32_t t = 0; t < ip->nverts; t++) {
        Vec3 P = m->points[ip->indices[t]].pos, Q = m->points[ip->indices[(t + 1) % ip->nverts]].pos, R = m->points[ip->indices[(t + 2) % ip->nverts]].pos;
        Vec3 n = vcross(vsub(R, Q), vsub(R, P)); float l = sqrtf(vdot(n, n));
        if (l > 0.01f && (!ok || l > best)) { best = l; ok = 1; nl = (Vec3){ n.x / l, n.y / l, n.z / l }; }
    }
    if (!ok) return 0;
    const float *M = in->node_world[o].m;                                /* column major: column j = image of axis j */
    Vec3 w = { M[0] * nl.x + M[4] * nl.y + M[8] * nl.z, M[1] * nl.x + M[5] * nl.y + M[9] * nl.z, M[2] * nl.x + M[6] * nl.y + M[10] * nl.z };
    float l = sqrtf(vdot(w, w)); if (!(l > 0)) return 0;
    *out = (Vec3){ w.x / l, w.y / l, w.z / l }; return 1;
}
/* Floor provided by the press nodes (kind 1) of visible instances (inst->vt[7] = 0x432480 walks S+0x58/0x5c only).
 * GetHeight (0x435650) reports it as hit type 4 against 3 for a world polygon (docs/PERSO_MOVE.md 6.3); hit_inst/hit_node
 * return what was hit so the caller can raise world_collision events.
 * Floor test inst->vt[7] = 0x432480(p, id), after the world polygons of GetHeight (0x498475): the ray from p straight down
 * (to p - (0, 1, 0), taken into node space with the inverse node matrix 0x440fc0, so t is in world units) against the
 * polygons of every PRESS node. A polygon counts when p is on its front side (loader plane, dist >= 0, 0x432794), the ray
 * passes inside it (every edge: dir . ((v_i - p) x (v_i+1 - p)) > 0, 0x432886 - one-sided: only faces whose loader normal
 * points up, i.e. the tops of the outward-wound press nodes; p inside a node finds neither its top nor its bottom), the
 * normal is not horizontal (|n . dir| > 1e-5, 0x43290c) and t = dist / n.y is below [0x4c4bd4] (0x432941). That value is the
 * world floor's distance, and 0x498520 starts it at 0 (0x49852e): with no world floor under p no instance can be the floor
 * either. Culling (uniform scale only, 0x43259a): p within node radius * scale of the node origin in xz and the origin not
 * more than that above p - the node box here. The hit ([0x4c4bd0] = 4) is the instance, the press-list index, the polygon
 * and the normal press_normal(). best_dist in/out = the distance below p; returns 1 if an instance polygon won.
 * Which instances, in which order (0x498475): those registered in the cells the world search visited (gel_walk_down, called by
 * world_ground) and then the dynamic list - gel_col_instances(). "t < best" is strict, so of two floors at the same height
 * the first instance tested keeps it. */
static int ins_floor_below(const GelFile *g, const InsFile *ins, Vec3 p, float *best_dist, const Instance *skip, const Instance **hit_inst, const InsNode **hit_node)
{
    int won = 0; Vec3 v[32]; *hit_inst = NULL; *hit_node = NULL;
    const GelColRef *cr; uint32_t ncr = gel_col_instances(g, ins, &cr);
    for (uint32_t q = 0; q < ncr; q++) {
        const Instance *in = cr[q].in; if (skip_inst(in, skip)) continue;
        const Model *m = in->model;
        uint32_t ncn; const uint32_t *cn = ins_collision_nodes(m, &ncn);
        {
            for (uint32_t ci = 0; ci < ncn; ci++) {
                uint32_t ni = cn[ci]; const InsNode *n = &m->nodes[ni];
                float nb[6];                                              /* the node's own box: not under p, or wholly above it */
                if (ins_node_world_box(in, ni, nb) && (p.x < nb[0] || p.x > nb[1] || p.z < nb[4] || p.z > nb[5] || nb[2] > p.y)) continue;
                for (uint32_t f = 0; f < n->npolys; f++) {
                    const InsPoly *pl = &n->polys[f]; if (pl->nverts < 3 || pl->nverts > 32) continue;
                    for (uint32_t t = 0; t < pl->nverts; t++) v[t] = ins_point_world(in, pl->indices[t]);
                    float P[4]; if (!loader_plane(v, pl->nverts, P)) continue;
                    float dist = P[0] * p.x + P[1] * p.y + P[2] * p.z + P[3];
                    if (dist < 0) continue;                                   /* 0x43279f: p behind the polygon */
                    uint32_t e = 0;
                    for (; e < pl->nverts; e++) {                             /* 0x432808: (0, -1, 0) . (a x b) > 0 <=> (a x b).y < 0 */
                        Vec3 a = vsub(v[e ? e - 1 : pl->nverts - 1], p), b = vsub(v[e], p);
                        if (!(a.z * b.x - a.x * b.z < 0)) break;
                    }
                    if (e < pl->nverts || !(fabsf(P[1]) > 1e-5f)) continue;
                    float t = dist / P[1];
                    if (!(t < *best_dist)) continue;
                    Vec3 nw; if (!press_normal(in, pl, &nw)) nw = (Vec3){ P[0], P[1], P[2] };
                    *best_dist = t; won = 1; *hit_inst = in; *hit_node = n; g_ins_n = nw;
                }
            }
        }
    }
    return won;
}

/* ---- the player's body against one polygon: 0x408600 (world) / 0x435b90 (instance press nodes, the same code) -------------
 * docs/PERSO_MOVE.md 6.5. The body is a vertical cylinder of radius r around the centre c whose lower part is a cone: the
 * band runs from c.y - down to c.y + up, above c.y the radius is r, below it r(y) = r (down + y) / down (y relative to c),
 * 0 at the bottom of the band. v[] are world-space vertices, pl the polygon plane (unit normal, d).
 *  1. dist = n.c + d < 0 (c behind the polygon) -> no hit; the plane interval [dist - down n.y, dist + up n.y] must reach
 *     into (-r, r).
 *  2. Sutherland-Hodgman clip of the polygon (relative to c) to the band -down <= y <= up (jump table 0x409a94 on
 *     code(prev) + 4 code(cur) - 1, code = (y < up) + (y < 0) + (y < -down)): an edge emits its start point when that is
 *     in the band, then its crossings of up / 0 / -down in the order it meets them. The crossings of y = 0 also go to a
 *     second list M: the chord where the polygon cuts the centre plane.
 *  3. Every edge of the clipped polygon (closing edge first), then the open polyline M: solve |P + t (Q - P)|_xz =
 *     r0 + t dr (the cone radius varies along an edge below the centre; 0x4096c7) as a quadratic; a < 0.01, no real root,
 *     roots outside [0, 1] -> no contact. Contact: the midpoint of the two roots clamped to [0, 1] is the contact point;
 *     below the centre pen = r(y) - |xz|, above it pen = r - sqrt(f(t) + r^2) (0x409907); push = pen (n.x, n.z) goes to
 *     a positive maximum / negative minimum per axis (0x409945).
 *  4. No edge contact but the centre inside the xz outline of the clipped polygon (all edge cross products on one side,
 *     0x409a1e) -> a hit with push 0.
 * Returns 1 on a hit (0x407000 then reports the wall contact 3). acc = { +x max, -x min, +z max, -z min }. */
static int cyl_code(float y, float up, float down) { return (y < up) + (y < 0) + (y < -down); }
static int cyl_poly(const Vec3 *vw, uint32_t nv, const float pl[4], Vec3 c, float r, float up, float down, float acc[4])
{
    float dist = pl[0] * c.x + pl[1] * c.y + pl[2] * c.z + pl[3];
    if (dist < 0 || nv < 3 || nv > 32 || down == 0) return 0;
    float lo = dist - down * pl[1], hi = dist + up * pl[1];
    if (!(lo > -r) && !(hi > -r)) return 0;                           /* 0x40867a: the whole band behind -r */
    if (!(lo < r) && !(hi < r)) return 0;                             /* 0x4086a5: ... or beyond r */
    Vec3 L[3 * 32 + 3], M[32 + 2]; int nl = 0, nm = 0;
    Vec3 pv = { vw[nv - 1].x - c.x, vw[nv - 1].y - c.y, vw[nv - 1].z - c.z }; int pc = cyl_code(pv.y, up, down);
    for (uint32_t i = 0; i < nv; i++) {
        Vec3 cv = { vw[i].x - c.x, vw[i].y - c.y, vw[i].z - c.z }; int cc = cyl_code(cv.y, up, down);
        if (pc == 1 || pc == 2) L[nl++] = pv;                            /* the start point, when it lies in the band */
        float lvl[3] = { up, 0, -down }; int b0, b1, st;                  /* boundary k lies between code k and k + 1 */
        if (pc < cc) { b0 = pc; b1 = cc - 1; st = 1; } else { b0 = pc - 1; b1 = cc; st = -1; }
        if (pc != cc) for (int k = b0; st > 0 ? k <= b1 : k >= b1; k += st) {
            float t = (pv.y - lvl[k]) / (pv.y - cv.y);
            Vec3 q = { pv.x + (cv.x - pv.x) * t, pv.y + (cv.y - pv.y) * t, pv.z + (cv.z - pv.z) * t };
            L[nl++] = q; if (k == 1 && nm < 32 + 2) M[nm++] = q;
        }
        pv = cv; pc = cc;
    }
    if (!nl) return 0;                                                  /* 0x409653: nothing inside the band */
    int hit = 0;
    for (int e = 0; e < nl + (nm > 1 ? nm - 1 : 0); e++) {
        Vec3 P = e < nl ? L[e ? e - 1 : nl - 1] : M[e - nl], Q = e < nl ? L[e] : M[e - nl + 1];
        float r0 = r, dr = 0;
        if (Q.y < -0.01f || P.y < -0.01f) { r0 = r * (down + P.y) / down; dr = r * (down + Q.y) / down - r0; }   /* 0x4096c7: the cone */
        float dx = Q.x - P.x, dz = Q.z - P.z, a = dx * dx + dz * dz - dr * dr;
        if (!(a > 0.01f)) continue;
        float b = 2.0f * (P.z * dz + P.x * dx - dr * r0), cq = P.x * P.x + P.z * P.z - r0 * r0, disc = b * b - 4.0f * a * cq;
        if (disc < 0) continue;
        float s = sqrtf(disc), t1 = (-b - s) / (2.0f * a), t2 = (s - b) / (2.0f * a);
        if (t1 > t2) { float w = t1; t1 = t2; t2 = w; }
        if (t1 > 1.0f || t2 < 0) continue;
        hit = 1;
        float tm = (t1 + t2) * 0.5f; if (tm < 0) tm = 0; if (tm > 1.0f) tm = 1.0f;
        float y = P.y + (Q.y - P.y) * tm, pen;
        if (y < 0) { float x = P.x + dx * tm, z = P.z + dz * tm, q2 = x * x + z * z; pen = (y + down) * r / down - (q2 >= 0 ? sqrtf(q2) : 0); }
        else { float q2 = (a * tm + b) * tm + r * r + cq; pen = r - (q2 >= 0 ? sqrtf(q2) : 0); }
        float vx = pen * pl[0], vz = pen * pl[2];
        if (vx < 0) { if (vx < acc[1]) acc[1] = vx; } else if (vx > acc[0]) acc[0] = vx;
        if (vz < 0) { if (vz < acc[3]) acc[3] = vz; } else if (vz > acc[2]) acc[2] = vz;
    }
    if (hit) return 1;
    int le = 0, ge = 0;                                                 /* 0x409a1e: the centre inside the clipped outline */
    for (int e = 0; e < nl; e++) { Vec3 A = L[e ? e - 1 : nl - 1], B = L[e]; float cr = B.z * A.x - A.z * B.x; le += cr <= 0; ge += cr >= 0; }
    return le == nl || ge == nl;
}
/* 0x407000 for the player's sweep: world polygons of the cells the band touches, then the press nodes of the instances
 * (vt[8] 0x433140: skipped when non-collidable (+8 & 0x40); it takes the polygons into world space with the node matrix
 * and uses the loader's plane of each polygon, 0x4280c2: n = (R - Q) x (R - P) of the longest consecutive triple, which
 * on every press node of the shipped levels points away from the node, so the same front-face rule applies; the uniform
 * branch 0x43320a and the non-uniform one 0x4335d7 differ only in the node-radius cull the first one does first and in
 * how the rotated normal is brought to length 1, press_normal()). *hit = any polygon touched (the wall contact P+0x2e0);
 * returns (pos max + neg min) in x and z. */
static Vec3 body_push(const Player *p, Vec3 c, float r, float up, float down, int *hit)
{
    float acc[4] = { 0, 0, 0, 0 }, box[6]; Vec3 v[32]; int any = 0;
    const GelFile *g = p->gel;
    query_box(g, c, r, c.y - down, c.y + up, box);
    GelPolySet ps = gel_polys_in_box(g, box);
    for (uint32_t k = 0; k < ps.n; k++) {
        const GelPoly *pl = &g->polys[ps.polys[k]]; if (pl->nverts < 3 || pl->nverts > 32) continue;
        for (uint32_t t = 0; t < pl->nverts; t++) { const GelVert *gv = &g->verts[pl->indices[t]]; v[t] = (Vec3){ gv->x, gv->y, gv->z }; }
        any |= cyl_poly(v, pl->nverts, pl->plane, c, r, up, down, acc);
    }
    /* the instances registered in the cells the band touches (0x40aa30), then the dynamic list: 0x407171..0x407301. The
     * merge is a per-axis maximum / minimum, so their order does not matter here, only which ones are tested */
    const InsFile *ins = p->ins; const GelColRef *cr; uint32_t ncr = 0;
    if (ins) { gel_walk_cyl(g, c, r, up, down); ncr = gel_col_instances(g, ins, &cr); }
    for (uint32_t q = 0; q < ncr; q++) {
        const Instance *in = cr[q].in; const Model *m = in->model; int mi = (int)(m - ins->models);
        if (skip_inst(in, p->inst)) continue;
        {
            uint32_t ncn; const uint32_t *cn = ins_collision_nodes(m, &ncn);
            for (uint32_t ci = 0; ci < ncn; ci++) {
                uint32_t ni = cn[ci]; const InsNode *nd = &m->nodes[ni]; if (nd->kind != 1) continue;
                float nb[6];                                              /* the node's own box against the body */
                if (ins_node_world_box(in, ni, nb) && (nb[0] > c.x + r || nb[1] < c.x - r || nb[4] > c.z + r || nb[5] < c.z - r || nb[3] < c.y - down || nb[2] > c.y + up)) continue;
                for (uint32_t f = 0; f < nd->npolys; f++) {
                    const InsPoly *ip = &nd->polys[f]; if (ip->nverts < 3 || ip->nverts > 32) continue;
                    for (uint32_t t = 0; t < ip->nverts; t++) v[t] = ins_point_world(in, ip->indices[t]);
                    /* the plane handed to 0x435b90: the node-space loader normal times the node matrix, normalised (press_normal),
                     * d = -n . (first world vertex) (0x4334ab / 0x4337e3) - the same code for the uniform and the non-uniform branch */
                    Vec3 nw; if (!press_normal(in, ip, &nw)) continue;
                    float pl[4] = { nw.x, nw.y, nw.z, -(nw.x * v[0].x + nw.y * v[0].y + nw.z * v[0].z) };
                    float before[4] = { acc[0], acc[1], acc[2], acc[3] };
                    any |= cyl_poly(v, ip->nverts, pl, c, r, up, down, acc);
                    if (wenv("WOODY_PUSHLOG") && (before[0] != acc[0] || before[1] != acc[1] || before[2] != acc[2] || before[3] != acc[3])) printf("push: inst %u model %d node %u type %d fade %.2f at %.0f %.0f %.0f", in->index, (int)mi, ni, in->type, in->fade, c.x, c.y, c.z), puts("");
                }
            }
        }
    }
    *hit = any;
    if (wenv("WOODY_CELLCHECK") && ins) {                               /* testing: the same with every instance (the old selection) */
        static int depth; if (!depth) { depth = 1; gel_col_force_all(1); int h2; Vec3 o = body_push(p, c, r, up, down, &h2); gel_col_force_all(0); depth = 0;
            if (fabsf(o.x - (acc[0] + acc[1])) > 0.01f || fabsf(o.z - (acc[2] + acc[3])) > 0.01f || h2 != any)
                printf("CELLCHECK push at %.0f %.0f %.0f: cells %.1f %.1f (%d) | all %.1f %.1f (%d)", c.x, c.y, c.z, acc[0] + acc[1], acc[2] + acc[3], any, o.x, o.z, h2), puts(""); }
    }
    return (Vec3){ acc[0] + acc[1], 0, acc[2] + acc[3] };
}

/* Actor pushing 0x4627d0: for every other actor of the previous frame's list 0x4c5258 (RegisterActor2: the living
 * enemies) 0x433d40(other pos, other vt[32] - 5, other vt[33], own pos, own radius, own height): when the circles overlap in
 * xz (dist < R = r_other - 5 + r_own) and the height ranges [feet, feet + h] overlap, disp.xz += (own - other).xz *
 * (1 - dist / R): the separation s grows to s (2 - s / R) per frame, not scaled by dt. The original's dt is the raw
 * QueryPerformanceCounter time (0x42a3f0, clamped to 0.1 at 0x40185b, no frame cap), so this push is frame-rate dependent
 * there; the port runs the same recurrence 60 dt times per frame (as if at 60 fps, like its other per-frame formulas):
 * identical at 60 fps and without overshoot at low rates. */
static void actor_push(const Player *p, float r, float h, float dt, Vec3 *disp)
{
    if (!p->enemies) return;
    for (int i = 0; i < p->enemies->n; i++) {
        const Enemy *e = &p->enemies->e[i]; if (e->removed || !e->inst->visible || e->hp <= 0) continue;
        float R = enemy_radius(e) - 5.0f + r, dx = p->pos.x - e->pos.x, dz = p->pos.z - e->pos.z, d2 = dx * dx + dz * dz;
        if (!(d2 < R * R) || e->pos.y + enemy_height(e) <= p->pos.y || p->pos.y + h <= e->pos.y) continue;
        float s = sqrtf(d2), t = s, n = dt * P_REF_FPS; if (s < 1e-6f) continue;   /* 0x433d40 returns (0, 0) for coincident centres */
        for (int it = 0; n > 0 && it < 64; it++, n -= 1.0f) t += t * (1.0f - t / R) * (n < 1.0f ? n : 1.0f);
        float k = (t - s) / s; disp->x += dx * k; disp->z += dz * k;
        if (wenv("WOODY_ACTORLOG")) printf("  ACTOR push from inst %u (type %d): dist %.1f of %.1f -> %.1f %.1f\n", e->inst->index, e->type, s, R, dx * k, dz * k);
    }
}

/* Sphere push-out 0x407340(c, r, cell -1), used by the actors' sweep 0x437580 (Boss14) and the camera sweep 0x439c50:
 * [0x4c4bd0] = 0, then every world polygon of the cells the sphere touches (0x40a700; poly stamp 0x4c4c24) through
 * 0x409ad0, merged on x, y AND z (0x407405..0x407495), then the instances of those cells and the dynamic list through
 * inst->vt[9] = 0x433ff0 (the PRESS nodes, S+0x58/0x5c; skipped without a cell, when flag +8 & 0x40 is set or the
 * collision mask does not match; uniform scale: node sphere cull |o - c| > N+0x2c * s + r), merged on x and z ONLY
 * (0x407536..0x40758e, 0x4075e4..0x40763c: vt[9] writes its first hit with y = 0, 0x4342ff, and never merges y). The
 * result [0x4c4bb4..bc] = per axis the positive maximum plus the negative minimum (0x407651). So an instance never pushes
 * the camera up or down, only a world polygon does. */
static int g_sphere_contact;               /* [0x4c4bd0]: the last player_sphere_push touched something (3 world, 4 instance) */
/* One polygon against the sphere (c, r): 0x409ad0 (world, `this` = the polygon) and 0x439d60 (instance press polygon from
 * 0x433ff0 / 0x4343e7, cdecl, world-space vertices and plane) - the same code. dist = n.c + d must be in (0.001, r]
 * (0x439d93 / 0x439db1). Per edge prev -> cur (starting with the last vertex; e = cur - prev, w = c - cur) the edge normal
 * m in the polygon's plane pointing into it: s = m.w >= 0 counts the centre as inside that edge (0x439eea); outside it,
 * s^2 > |m|^2 r^2 (the centre more than r beyond the edge's line, 0x439f28) ends the test with no contact, otherwise the edge
 * is kept. The instance code takes m = e x n (0x439e61..0x439ea2) with the loader normal, which points against the
 * polygon's winding (press_normal); 0x409ad0 takes e' = prev - cur (0x409bf1) and so m = n x e with the .gel plane, which
 * points along the winding (every world polygon of W1A/W1B/K2A/W3D/House: the centroid is inside all its edges that way):
 * both are the inward edge normal, so here it is taken from `inward` = +1 (e x n) / -1 (n x e).
 *  all edges inside (0x439f59): the push is (r - dist) n, written as the polygon's result (positive part / negative part);
 *  else every kept edge (0x439ffa..0x43a290): a = |e|^2 (the instance code skips a <= 0.001, 0x43a040; the world code has no
 *  such test, but a zero edge fails the root tests there); the roots s1 <= s2 of |w - s e|^2 = r^2 (B = -2 w.e, 0x4a9504;
 *  D = B^2 - 4a(|w|^2 - r^2), none if D < 0); the sphere meets the segment s in [-1, 0] when s1 <= 0 and s2 >= -1; then the
 *  chord's midpoint m = (s1 + s2)/2 clamped to [-1, 0] (the closest point of the segment, q = cur + m e) and the push
 *  (c - q) (r - |c - q|) / |c - q| (0x43a171..0x43a1ed), merged per axis into the positive maximum / negative minimum.
 * So near a corner every edge the sphere cuts pushes, not only the nearest point. Returns 1 on contact. `axes` = 7 merges
 * x, y, z (world), 5 only x and z (instances, see above). The port skips |c - q| < 1e-4 (the original divides by 0 there). */
static float sphere_dist(const float pl[4], Vec3 c, int inst)            /* n.c + d in the original's summation order */
{
    return inst ? (pl[1] * c.y + pl[0] * c.x) + pl[2] * c.z + pl[3]        /* 0x439d78..0x439d8c */
                : (pl[1] * c.y + pl[2] * c.z) + pl[0] * c.x + pl[3];       /* 0x409ade..0x409af6 */
}
static int sphere_poly(const Vec3 *v, uint32_t n, const float pl[4], Vec3 c, float r, float inward, int inst, int axes, float acc[6])
{
    float dist = sphere_dist(pl, c, inst); if (!(dist > 0.001f) || !(dist <= r)) return 0;
    Vec3 nn = { pl[0] * inward, pl[1] * inward, pl[2] * inward }, E[32], W[32]; uint32_t inside = 0, ne = 0;
    for (uint32_t i = 0; i < n; i++) {
        Vec3 cur = v[i], e = vsub(cur, v[i ? i - 1 : n - 1]), w = vsub(c, cur), m = vcross(e, nn); float s = vdot(m, w);
        if (s >= 0) { inside++; continue; }
        if (vdot(m, m) * r * r < s * s) return 0;                                /* 0x439f28: wholly beyond this edge */
        E[ne] = e; W[ne] = w; ne++;
    }
    float px[3]; int hit = 0;
    if (inside == n) {                                                          /* 0x439f63: over the face */
        px[0] = (r - dist) * pl[0]; px[1] = (r - dist) * pl[1]; px[2] = (r - dist) * pl[2];
        for (int a = 0; a < 3; a++) if (axes >> a & 1) { if (px[a] < 0) { if (px[a] < acc[3 + a]) acc[3 + a] = px[a]; } else if (px[a] > acc[a]) acc[a] = px[a]; }
        return 1;
    }
    for (uint32_t k = 0; k < ne; k++) {
        Vec3 e = E[k], w = W[k]; float a = vdot(e, e); if (inst && !(a > 0.001f)) continue;
        float B = -2.0f * vdot(w, e), D = B * B - (vdot(w, w) - r * r) * a * 4.0f; if (!(D >= 0)) continue;
        float sD = sqrtf(D), h = 0.5f / a, s1 = (-B - sD) * h, s2 = (sD - B) * h;
        if (s1 > s2) { float t = s1; s1 = s2; s2 = t; }
        if (s1 > 0 || !(s2 >= -1.0f)) continue;                                  /* 0x43a0f4 / 0x43a109 */
        float mm = (s1 + s2) * 0.5f; if (mm > 0) mm = 0; else if (mm < -1.0f) mm = -1.0f;
        Vec3 q = { w.x - mm * e.x, w.y - mm * e.y, w.z - mm * e.z }; float l = sqrtf(vdot(q, q)); if (l < 1e-4f) continue;
        float k2 = (r - l) / l; px[0] = q.x * k2; px[1] = q.y * k2; px[2] = q.z * k2; hit = 1;
        for (int a2 = 0; a2 < 3; a2++) if (axes >> a2 & 1) { if (px[a2] < 0) { if (px[a2] < acc[3 + a2]) acc[3 + a2] = px[a2]; } else if (px[a2] > acc[a2]) acc[a2] = px[a2]; }
    }
    return hit;
}
Vec3 player_sphere_push(const Player *p, const Instance *skip, Vec3 c, float r)
{
    float acc[6] = { 0, 0, 0, 0, 0, 0 }, box[6]; Vec3 v[32];
    const GelFile *g = p->gel; g_sphere_contact = 0;
    query_box(g, c, r, c.y - r, c.y + r, box);
    GelPolySet ps = gel_polys_in_box(g, box);
    for (uint32_t k = 0; k < ps.n; k++) {
        const GelPoly *pl = &g->polys[ps.polys[k]]; if (pl->nverts < 3 || pl->nverts > 32) continue;
        float d0 = sphere_dist(pl->plane, c, 0); if (!(d0 > 0.001f) || !(d0 <= r)) continue;
        for (uint32_t t = 0; t < pl->nverts; t++) { const GelVert *gv = &g->verts[pl->indices[t]]; v[t] = (Vec3){ gv->x, gv->y, gv->z }; }
        if (sphere_poly(v, pl->nverts, pl->plane, c, r, -1.0f, 0, 7, acc)) g_sphere_contact = 3;
    }
    /* the instances of the cells the sphere touches (0x40a700), then the dynamic list (0x4074ca..0x407640); vt[9] 0x433ff0 */
    const InsFile *ins = p->ins; const GelColRef *cr; uint32_t ncr = 0;
    if (ins) { gel_walk_sphere(g, c, r); ncr = gel_col_instances(g, ins, &cr); }
    for (uint32_t q = 0; q < ncr; q++) {
        const Instance *in = cr[q].in; const Model *m = in->model;
        if (in == p->inst || skip_inst(in, skip)) continue;
        {
            uint32_t ncn; const uint32_t *cn = ins_collision_nodes(m, &ncn);
            for (uint32_t ci = 0; ci < ncn; ci++) {
                uint32_t ni = cn[ci]; const InsNode *nd = &m->nodes[ni]; if (nd->kind != 1) continue;
                float nb[6];                                              /* the node's box: as conservative as the sphere cull 0x434116 */
                if (ins_node_world_box(in, ni, nb) && (nb[0] > c.x + r || nb[1] < c.x - r || nb[2] > c.y + r || nb[3] < c.y - r || nb[4] > c.z + r || nb[5] < c.z - r)) continue;
                for (uint32_t f = 0; f < nd->npolys; f++) {
                    const InsPoly *pl = &nd->polys[f]; if (pl->nverts < 3 || pl->nverts > 32) continue;
                    for (uint32_t t = 0; t < pl->nverts; t++) v[t] = ins_point_world(in, pl->indices[t]);
                    /* the plane: the loader normal times the node matrix (0x434216..0x4342a1: times 1/s, uniform; normalised in the
                     * non-uniform branch 0x434543), d = -n . (first world vertex) - as for the cylinder, press_normal() */
                    Vec3 nw; if (!press_normal(in, pl, &nw)) continue;
                    float P4[4] = { nw.x, nw.y, nw.z, -(nw.x * v[0].x + nw.y * v[0].y + nw.z * v[0].z) };
                    if (sphere_poly(v, pl->nverts, P4, c, r, 1.0f, 1, 5, acc)) g_sphere_contact = 4;   /* 0x434323: [0x4c4bd0] = 4 */
                }
            }
        }
    }
    return (Vec3){ acc[0] + acc[3], acc[1] + acc[4], acc[2] + acc[5] };
}

/* GetHeight 0x435650 -> 0x498520: nearest surface below the point: world polygons with n.y > 1e-5 (any slope) that
 * contain the point in xz, and the press nodes of instances (hit_node != NULL then). */
static const Instance *g_ground_skip;      /* set by player_ground_query(): instance to ignore instead of the player */
static float world_ground(const Player *p, Vec3 pt, int *found, const Instance **hit_inst, const InsNode **hit_node)
{
    const GelFile *g = p->gel; float best = 1e30f, box[6]; int f1 = 0, f2; Vec3 gn = { 0, 1, 0 }; int32_t gm = -1;
    query_box(g, pt, 1.0f, g->bbox[2] - 1.0f, pt.y, box);            /* the column under the point */
    GelPolySet ps = gel_polys_in_box(g, box);
    for (uint32_t k = 0; k < ps.n; k++) {
        uint32_t i = ps.polys[k];
        const GelPoly *pl = &g->polys[i];
        if (pl->nverts < 3 || pl->plane[1] <= 1e-5f) continue;
        float dist = pl->plane[0] * pt.x + pl->plane[1] * pt.y + pl->plane[2] * pt.z + pl->plane[3];
        if (dist <= 0 || dist / pl->plane[1] >= best) continue;
        Vec3 q = { pt.x, pt.y - dist / pl->plane[1], pt.z };
        if (poly_contains(g, pl, q)) { best = dist / pl->plane[1]; f1 = 1; gn = (Vec3){ pl->plane[0], pl->plane[1], pl->plane[2] }; gm = (int32_t)pl->material; }
    }
    float y1 = pt.y - best;
    /* the instances only beat a world floor that was found: [0x4c4bd4] starts at 0 (0x49852e), and t < 0 never wins */
    float bi = f1 ? best : 0.0f;
    gel_walk_down(g, pt, f1, y1);                                        /* the cells 0x498520 visited: pt's cell down to the floor's */
    float bi0 = bi;
    f2 = ins_floor_below(g, p->ins, pt, &bi, g_ground_skip ? g_ground_skip : p->inst, hit_inst, hit_node);
    if (wenv("WOODY_CELLCHECK")) {                                     /* testing: what every instance (the old selection) would give */
        float b2 = bi0; const Instance *h2; const InsNode *n2; Vec3 keep_n = g_ins_n;
        gel_col_force_all(1); int w2 = ins_floor_below(g, p->ins, pt, &b2, g_ground_skip ? g_ground_skip : p->inst, &h2, &n2); gel_col_force_all(0); g_ins_n = keep_n;
        if (w2 != f2 || h2 != *hit_inst || fabsf(b2 - bi) > 0.01f)
            printf("CELLCHECK floor at %.0f %.0f %.0f: cells inst %d y %.1f | all inst %d y %.1f", pt.x, pt.y, pt.z, f2 ? (int)(*hit_inst)->index : -1, pt.y - bi, w2 ? (int)h2->index : -1, pt.y - b2), puts("");
    }
    if (f2) { *found = 1; g_ground_n = g_ins_n; g_ground_mat = -1; return pt.y - bi; }
    *hit_inst = NULL; *hit_node = NULL; *found = f1; g_ground_n = f1 ? gn : (Vec3){ 0, 1, 0 }; g_ground_mat = f1 ? gm : -1; return f1 ? y1 : pt.y;
}

/* the world_collision behind a GetHeight hit (0x436e03..0x436e65): hit type 2 (an instance press node), the model has
 * collisions (S+0x60), the node's type code is 1 -> inst+0x70[nvol + sub-index]; anything else 0xffffffff */
static uint32_t hit_collision(const Instance *hi, const InsNode *hn)
{
    if (!hi || !hn || hn->kind != 1 || hn->type_code != 1) return 0xffffffffu;
    const Model *hm = hi->model; uint32_t k = hm->nvolume_nodes + hn->sub_index;
    return hm->ncollision_ids && k < hi->nids ? hi->ids[k] : 0xffffffffu;
}
/* GetHeight for another actor (skip = its own instance). col (may be NULL) = the world_collision it stands over, for the
 * generic probe 0x436dc0 of enemies and bombs; n (may be NULL) = the floor normal ([0x4b3108..], GetHeight 0x435650:
 * (0, 1, 0) and y = pt.y when nothing is found) */
static float ground_query_full(const Player *p, const Instance *skip, Vec3 pt, int *found, uint32_t *col, Vec3 *n)
{
    const Instance *hi; const InsNode *hn; Vec3 keep = g_ground_n;
    int32_t keep_mat = g_ground_mat;
    g_ground_skip = skip; g_skip_self = skip ? p->inst : NULL; float y = world_ground(p, pt, found, &hi, &hn); g_ground_skip = NULL; g_skip_self = NULL;
    if (n) *n = g_ground_n;
    g_ground_n = keep; g_ground_mat = keep_mat;
    if (col) *col = *found ? hit_collision(hi, hn) : 0xffffffffu;
    return y;
}
float player_ground_query_col(const Player *p, const Instance *skip, Vec3 pt, int *found, uint32_t *col) { return ground_query_full(p, skip, pt, found, col, NULL); }
float player_ground_query_n(const Player *p, const Instance *skip, Vec3 pt, int *found, Vec3 *n) { return ground_query_full(p, skip, pt, found, NULL, n); }
float player_ground_query(const Player *p, const Instance *skip, Vec3 pt, int *found) { return ground_query_full(p, skip, pt, found, NULL, NULL); }
/* the same GetHeight with its hit type 2 ([0x53a554] == 2): the instance [0x53a560] and the press node [0x53a58c] under pt, for the
 * platform attach 0x436d80 of the probe 0x436dc0 (enemies); *hi = NULL for a world floor or nothing */
float player_ground_query_hit(const Player *p, const Instance *skip, Vec3 pt, int *found, uint32_t *col, const Instance **hi, uint32_t *node)
{
    const InsNode *hn; Vec3 keep = g_ground_n; int32_t keep_mat = g_ground_mat;
    g_ground_skip = skip; g_skip_self = skip ? p->inst : NULL; float y = world_ground(p, pt, found, hi, &hn); g_ground_skip = NULL; g_skip_self = NULL;
    g_ground_n = keep; g_ground_mat = keep_mat;
    if (col) *col = *found ? hit_collision(*hi, hn) : 0xffffffffu;
    if (!*found || !*hi || !hn) { *hi = NULL; *node = 0; } else *node = (uint32_t)(hn - (*hi)->model->nodes);
    return y;
}

/* Landing ring 0x44af90 (docs/PERSO_JUMP.md 5), run by the Perso post-render update 0x44b4a0 every frame that is not
 * paused. Not in Perso state 2 (dead), 4 (climbing), 5 (scripted) or 8 (rocket), nor while the game's mode object is in a
 * cinematic/menu mode (0x44f2e0); then even the timers stand still. Otherwise: on the ground +0x580 counts up and the
 * alpha +0x588 drops 1020/s (gone in 0.25 s), in the air +0x584 counts up and the alpha climbs 255/s (full after 1 s),
 * clamped to 0..255. The ring itself is one sprite: bank 0 image 3 (the ripple ring of the splash, byte-identical to
 * image 58), mode 0x12, size 80, rotation 64/512, rgb 0.6, alpha +0x588 * 0.8/255, flags 6 (own colour and rotation,
 * flat in the plane of S+0x230 = the floor normal P+0x458, additive), at (x, floor height P+0x224 + 1, z) - straight
 * under him, also after landing while it fades. */
int player_landing_ring(Player *p, float dt, Vec3 *pos, Vec3 *normal, float *alpha)
{
    if (!p->inst || p->dead_kind || p->script_act || p->ride || p->climb_sub || p->use_root) return 0;   /* 0x44af93: states 5, 4, 2, 8 */
    if (p->on_ground) { p->ring_ground_t += dt; p->ring_air_t = 0; } else { p->ring_air_t += dt; p->ring_ground_t = 0; }   /* 0x44afd4, byte +0x22c */
    if (p->ring_ground_t > 0) p->ring_a -= dt * 1020.0f;                /* [0x4aac70] */
    else if (p->ring_air_t > 0) p->ring_a += dt * 255.0f;               /* [0x4aa308] */
    if (p->ring_a > 255.0f) p->ring_a = 255.0f;
    else if (p->ring_a < 0) { p->ring_a = 0; return 0; }                /* 0x44b185: below 0 it is reset and nothing is drawn */
    *pos = (Vec3){ p->pos.x, p->floor_y + 1.0f, p->pos.z };             /* P+0x1f4, P+0x224 + 1.0, P+0x1fc */
    if (normal) *normal = p->race_floor_n;                              /* P+0x458 = Mover+0xd0, the floor query's normal (docs/RACE.md 1) */
    *alpha = p->ring_a * 0.0031372549f;                                 /* [0x4aac6c] = 0.8 / 255 */
    return 1;
}

/* ground type Perso+0x308 (0x4628e0 -> 0x46295f): byte 3 of the flag word of the texture group behind the material of
 * the floor polygon ("m_nGroundType", docs/FORMAT_TEX_COL_VIS_LIT.md 1). 1 = slippery, 2 = dust/sand/snow. Only world
 * polygons have one: a floor made by an instance node, a polygon without a material (bit 15) or a missing .tex is 0. */
static int ground_type(const Player *p, int32_t mat)
{
    if (wenv("WOODY_ICE")) return 1;                                  /* testing: every floor slippery (no shipped level has one) */
    if (mat < 0 || (mat & 0x8000) || !p->tex) return 0;
    uint32_t mi = (uint32_t)mat; if (mi >= p->tex->nmaterials) return 0;
    uint32_t g = p->tex->materials[mi].group; if (g >= p->tex->ngroups) return 0;
    return (int)(p->tex->groups[g].flags >> 24);
}

/* ---- platform attachment (Perso+0x298, 0x436d80 store / 0x436d20 delta; docs/PERSO_MOVE.md 6.1) -------------
 * While standing on an instance node the contact point is remembered in node space; next frame the node's
 * movement (world(local) - previous world position) is added to the player's displacement. */
static int mat4_inv_apply(const Mat4 *m, Vec3 w, Vec3 *out)        /* out = M^-1 * w for an affine column-major M */
{
    const float *a = m->m;
    float x = w.x - a[12], y = w.y - a[13], z = w.z - a[14];
    float c00 = a[5] * a[10] - a[9] * a[6], c01 = a[9] * a[2] - a[1] * a[10], c02 = a[1] * a[6] - a[5] * a[2];
    float det = a[0] * c00 + a[4] * c01 + a[8] * c02; if (fabsf(det) < 1e-12f) return 0;
    float c10 = a[8] * a[6] - a[4] * a[10], c11 = a[0] * a[10] - a[8] * a[2], c12 = a[4] * a[2] - a[0] * a[6];
    float c20 = a[4] * a[9] - a[8] * a[5], c21 = a[8] * a[1] - a[0] * a[9], c22 = a[0] * a[5] - a[4] * a[1];
    out->x = (c00 * x + c10 * y + c20 * z) / det; out->y = (c01 * x + c11 * y + c21 * z) / det; out->z = (c02 * x + c12 * y + c22 * z) / det;
    return 1;
}
int player_node_local(const Instance *in, uint32_t node, Vec3 w, Vec3 *local)   /* 0x431700: world point -> node space (the probe's attach 0x436d80) */
{
    return in && in->node_world && node < in->model->nnodes && mat4_inv_apply(&in->node_world[node], w, local);
}
static Vec3 attach_delta(Player *p)
{
    Vec3 d = { 0, 0, 0 };
    if (!p->att_inst || !p->att_inst->node_world || !p->att_inst->visible) return d;
    Vec3 nw = mat4_apply(&p->att_inst->node_world[p->att_node], p->att_local);
    d = vsub(nw, p->att_world);
    if (vdot(d, d) > 200.0f * 200.0f) { d = (Vec3){ 0, 0, 0 }; p->att_inst = NULL; }   /* teleporting platform: let go */
    return d;
}
static void attach_store(Player *p, const Instance *inst, const InsNode *node, Vec3 contact)
{
    p->att_inst = NULL;
    if (!inst || !node || !inst->node_world) return;
    uint32_t ni = (uint32_t)(node - inst->model->nodes);
    if (!mat4_inv_apply(&inst->node_world[ni], contact, &p->att_local)) return;
    p->att_inst = inst; p->att_node = ni; p->att_world = contact;
}
static void slide_reset(Player *p)                                       /* Mover_SetDir 0x459ff0: 0x467110 on RampB, M+0xd0 = up (0x45a110), M+0xdc = 0 */
{
    p->ground_n = (Vec3){ 0, 1, 0 }; p->slide_dir = (Vec3){ 0, 0, 0 }; p->slide_speed = p->slide_target = p->slide_t1 = p->slide_t2 = 0; p->slide_phase = 0; p->sliding = 0;
}

/* ---- Jumper (0x462d70 update, 0x462fd0 tick) ------------------------------------ */
/* P+0x64 / +0x68 / +0x6c for the parameter column of the player (0x4b5f14): the race columns 3/4 jump 400 high with
 * 1250 / 1250 (T_jump 0.375 s, T_fall 0.625 s, docs/RACE.md 6); every other character keeps Woody's values */
static float g_jH = J_HEIGHT, g_jP68 = J_P68, g_jV = J_V;
static void jumper_start_jump(Jumper *j) { j->D = g_jP68 * 0.75f; j->t = (j->D / g_jV) * -0.5f; j->h_prev = 0; j->v_down = 0; j->short_hop = j->fell_off = 0; }   /* 0x462d10 */
static void jumper_start_fall(Jumper *j, int fell_off) { j->D = g_jP68 * 1.25f; j->fell_off = fell_off; j->h_prev = g_jH; j->t = 0; }                       /* 0x462d40 */
static int jumper_tick(Jumper *j, float dt, int on_ground)
{
    j->t += dt;
    float u = j->t / (0.5f * j->D / g_jV), h = g_jH - g_jH * u * u;
    if (j->t >= J_APEX_T && j->state == 1 && !j->short_hop) { j->state = 7; j->open_window = 1; }   /* 0x457560(0.5): air attack window */
    if (j->t >= 0 && (j->state == 7 || (j->state == 1 && j->short_hop))) {
        j->state = j->short_hop ? (on_ground ? 6 : 4) : 3;
        jumper_start_fall(j, 0); h = g_jH;
    }
    if (u > 1.0f) { float k = u - 1.0f < 0.5f ? u - 1.0f : 0.5f; j->dy = -(((J_TERMINAL - j->v_down) * 2 * k + j->v_down) * dt); }
    else { j->dy = h - j->h_prev; j->v_down = -j->dy / dt; }
    j->h_prev = h;
    return !(u < 0.45f && !j->short_hop);
}
static void jumper_update(Jumper *j, int jump_held, int allowed, int on_ground, float height_above_ground, float dt)
{
    if (j->state == 2) { j->dy = 0; j->fallen = 0; j->hard_fall = 0; }
    if (!allowed) jump_held = 0;                                  /* 0x462d9c: Perso_Move's input flag off = the key counts as up */
    if (height_above_ground <= J_REARM_H && !jump_held && allowed) j->armed = 1;   /* 0x462dc0: but it re-arms only with input allowed (a lock,
                                                                   * an attack or the knockback timer keep a held key from firing on release of the lock) */
    if (j->coyote) { j->coyote_t += dt; if (j->coyote_t > J_COYOTE) j->coyote = 0; }
    switch (j->state) {
    case 2:
        if (jump_held && on_ground && j->armed) { j->state = 0; jumper_start_jump(j); jumper_tick(j, dt, on_ground); j->armed = 0; return; }
        if (!on_ground) { j->coyote = j->armed; if (j->armed) j->coyote_t = 0; j->state = 3; jumper_start_fall(j, 1); jumper_tick(j, dt, on_ground); j->armed = 0; }
        return;
    case 0: j->state = 1; /* fallthrough */
    case 1:
        if (!jump_held && j->t < J_SHORT_HOP_T) {                 /* variable jump height: only the clock jumps */
            float u = J_SHORT_HOP_T / (0.5f * j->D / g_jV);
            j->short_hop = 1; j->t = J_SHORT_HOP_T; j->h_prev = g_jH - g_jH * u * u;
        }
        jumper_tick(j, dt, on_ground); return;
    case 7: if (jumper_tick(j, dt, on_ground) && on_ground) j->state = 6; return;
    case 3:
        if (jumper_tick(j, dt, on_ground)) j->state = 4;
        if (j->coyote && jump_held) { j->coyote = 0; j->state = 0; jumper_start_jump(j); jumper_tick(j, dt, on_ground); j->armed = 0; }
        return;
    case 4: if (j->fallen > J_HARD_FALL) { j->hard_fall = 1; j->state = 5; } /* fallthrough */
    case 5: if (jumper_tick(j, dt, on_ground) && on_ground) j->state = 6; return;
    case 6: j->state = 2; return;
    }
}

/* ---- the second air action (docs/PERSO_JUMP.md 1.5) ------------------------------------------------------------
 * 0x465e50, a pre-step of every Perso state (0x44b764): back on the ground (+0x22c) it may be used again; action 4 JUST pressed
 * while the air-move window +0x6f8 is open (the jumper opens it with the attack window at t >= -0.15, the recoil of a peck at
 * T < 0.5), more than P+0x84 = 100 above the ground and not used since the ground starts it once per air time, by subtype:
 * 3 (Knothead, script type 2) an air dash along the facing, 0.15 s at 3000 u/s; 2 (Splinter, type 3) a double jump straight up,
 * 0.25 s at 1200 u/s less 1 % per 1/60 s; SoundFx 0x3a. Woody (1) and the riders (4/5) only get the "used" flag. No animation
 * request and no controller reset: whatever the Jumper state asks for (the somersault, anim 5) plays on. */
static void air_move_start(Player *p, const PlayerInput *in)
{
    int pressed = in->jump && !p->am_jprev; p->am_jprev = in->jump;      /* 0x467420(4) */
    if (p->on_ground) p->am_used = 0;                                    /* 0x465e62 */
    if (!(p->am_win > 0) || !pressed || p->am_on || p->am_used || !(p->pos.y - p->floor_y > J_REARM_H)) return;   /* 0x465e68..0x465eb5 */
    p->am_acc = 0; p->am_on = 1; p->am_used = 1;
    if (p->subtype == 3) {                                               /* 0x465ee3: M+0x10, normalised when longer than 0 */
        p->am_dir = (Vec3){ sinf(p->yaw), 0, cosf(p->yaw) }; p->am_t = 0; p->am_dur = 0.15f; p->am_speed = 3000.0f;
    } else if (p->subtype == 2) {                                        /* 0x465f8b */
        p->am_dir = (Vec3){ 0, 1, 0 }; p->am_t = 0; p->am_dur = 0.25f; p->am_speed = 1200.0f;
    } else { p->am_on = 0; return; }                                     /* 0x465fcf */
    audio_fx(0x3a, NULL, NULL);
    if (wenv("WOODY_JUMPLOG")) printf("  AIR MOVE %s (jumper %d t %.3f, %.0f above the floor, anim %d)\n", p->subtype == 3 ? "dash" : "double jump", p->jumper.state, p->jumper.t, p->pos.y - p->floor_y, p->lanim);
}
/* 0x465fe0, the head of Perso_Move (states 0/6/2/3): the window runs down; while active, +0x204 = dir * step * speed with
 * step = the part of this frame inside the length, the direction keeping the last displacement; returns 1 = Perso_Move stops */
static int air_move_tick(Player *p, float dt, Vec3 *disp)
{
    if (p->am_win > 0) p->am_win -= dt;
    if (!p->am_on) return 0;
    if (p->am_t >= p->am_dur) { p->am_on = 0; return 0; }               /* 0x466026: the frame after the end is a normal one */
    float t0 = p->am_t; p->am_t += dt; if (p->am_t > p->am_dur) p->am_t = p->am_dur;
    float step = p->am_t - t0;
    if (p->subtype == 2) { p->am_acc += step; while (p->am_acc >= 1.0f / 60.0f) { p->am_speed *= 0.99f; p->am_acc -= 1.0f / 60.0f; } }   /* 0x4a9990, 0x4ab7d8 */
    float l = sqrtf(vdot(p->am_dir, p->am_dir)), k = step * p->am_speed;
    if (l > 0) { p->am_dir.x /= l; p->am_dir.y /= l; p->am_dir.z /= l; }
    p->am_dir = (Vec3){ p->am_dir.x * k, p->am_dir.y * k, p->am_dir.z * k };
    *disp = p->am_dir;
    return 1;
}

/* ---- volumes ------------------------------------------------------------------ */
int volume_contains(const Instance *inst, uint32_t node, Vec3 p)
{
    const Model *m = inst->model; const InsNode *n = &m->nodes[node];
    if (!n->polys || n->npolys < 4) return 0;
    /* centroid of the transformed points decides the outward side of each face */
    Vec3 cen = { 0, 0, 0 }; uint32_t cnt = 0;
    for (uint32_t k = 0; k < n->npoints; k++) { Vec3 w = ins_point_world(inst, n->point_base + k); cen.x += w.x; cen.y += w.y; cen.z += w.z; cnt++; }
    if (!cnt) return 0;
    cen.x /= cnt; cen.y /= cnt; cen.z /= cnt;
    int valid = 0;
    for (uint32_t f = 0; f < n->npolys; f++) {
        const InsPoly *pl = &n->polys[f]; if (pl->nverts < 3) continue;
        Vec3 a = ins_point_world(inst, pl->indices[0]), b = ins_point_world(inst, pl->indices[1]), c = ins_point_world(inst, pl->indices[2]);
        Vec3 nrm = vcross(vsub(b, a), vsub(c, a));
        float side_c = vdot(nrm, vsub(cen, a)), side_p = vdot(nrm, vsub(p, a));
        if (fabsf(side_c) < 1e-6f) continue;                          /* degenerate face (or unposed instance) */
        valid++;
        if ((side_c > 0) != (side_p > 0) && side_p != 0) return 0;   /* p on the other side of a face than the centroid */
    }
    return valid >= 4;
}

/* The player is the instance the level script gives a Perso class (SetTypeInstance 1, 2, 3, 18 or 19): Woody in the W
 * levels, type 2 in the K levels, 3 in the S levels, 18 in the races. Until the script has run it is model 0 instance 0. */
static void race_enter(Player *p, int respawn);
static void race_snd_stop(Player *p);
void player_bind(Player *p, Instance *inst)
{
    p->inst = inst; p->pos = inst->position; p->floor_y = p->pos.y; p->cam_init = 0;
    /* facing from the instance quaternion: rotation about y composed with the -90 deg x model rotation */
    ins_pose(inst, 0, 0);
    Vec3 fwd = mat4_apply(&inst->world, (Vec3){ 0, -1, 0 }); fwd = vsub(fwd, inst->position);
    p->yaw = atan2f(fwd.x, fwd.z);
    p->spawn_pos = p->start_pos = p->pos; p->spawn_yaw = p->yaw;        /* +0x318 = +0x30c (0x44a44a) */
    p->race_char = inst->type == 18 || inst->type == 19;                  /* subtypes 5/4: Reset 0x44ab20 enters state 1 through SurfEnter */
    p->subtype = inst->type == 2 ? 3 : inst->type == 3 ? 2 : inst->type == 18 ? 5 : inst->type == 19 ? 4 : 1;   /* the Perso ctor's argument per script type (0x403560..0x403607) */
    g_jH = p->race_char ? 400.0f : J_HEIGHT; g_jP68 = p->race_char ? 1250.0f : J_P68; g_jV = p->race_char ? 1250.0f : J_V;
    if (p->race_char) race_enter(p, 0);                                   /* SetTypeInstance, inside the init tick; the Game ctor's SurfEnter follows (player_race_start) */
}
/* the Game ctor 0x445850 runs after the init tick has delivered the init messages (0x404850, then 0x404900): its 0x445930 ->
 * Reset -> SurfEnter (0x44f8a0 puts back every race bonus, the rider gets 0x5d). The six race scripts send 1120 from the
 * trigger volume at the start, i.e. in the first game frame, so the board has no controller yet and gets nothing here. */
void player_race_start(Player *p) { if (p->race_char) race_enter(p, 1); }

int player_init(Player *p, InsFile *ins, const GelFile *gel, const TexFile *tex)
{
    memset(p, 0, sizeof *p);
    if (!ins->nmodels || !ins->models[0].ninstances) return -1;
    p->gel = gel; p->ins = ins; p->tex = tex; p->cur_col = 0xffffffffu; p->step_u = -1.0f; p->crush = 1.0f; p->ground_n = (Vec3){ 0, 1, 0 };
    player_bind(p, &ins->models[0].instances[0]);
    p->jumper.state = 2; p->jumper.armed = 1;                      /* 0x462c90 reset */
    p->health = 3.0f; p->lives = 3; p->lanim = -1; p->board_lanim = -1;
    p->game_state = 1; p->iris_from = 0; p->iris_to = 1.0f; p->iris_dur = 1.0f; p->iris = 0;   /* Game ctor 0x445850: the level opens with the iris, 0 -> 1 in 1 s */
    p->cam_dist = 400.0f; p->cam_height = 180.0f; p->cam_zoom = 1.2f;
    /* volume table */
    for (uint32_t mi = 0; mi < ins->nmodels; mi++) p->nvol += ins->models[mi].nvolume_nodes * ins->models[mi].ninstances;
    p->inside = (uint8_t *)calloc(p->nvol ? p->nvol : 1, 1);
    p->vol_inst = (Instance **)calloc(p->nvol ? p->nvol : 1, sizeof *p->vol_inst);
    p->vol_node = (uint32_t *)calloc(p->nvol ? p->nvol : 1, 4); p->vol_id = (uint32_t *)calloc(p->nvol ? p->nvol : 1, 4);
    uint32_t v = 0;
    for (uint32_t mi = 0; mi < ins->nmodels; mi++) {
        Model *m = &ins->models[mi];
        for (uint32_t k = 0; k < m->ninstances; k++) for (uint32_t j = 0; j < m->nvolume_nodes; j++) {
            Instance *in = &m->instances[k];
            p->vol_inst[v] = in; p->vol_node[v] = m->volume_nodes[j] - 1;   /* node lists in the file are 1-based */ p->vol_id[v] = j < in->nids ? in->ids[j] : 0; v++;
        }
    }
    /* pose every instance once so volume tests are valid before the first rendered frame */
    for (uint32_t mi = 0; mi < ins->nmodels; mi++) for (uint32_t k = 0; k < ins->models[mi].ninstances; k++) ins_pose(&ins->models[mi].instances[k], 0, 0);
    p->spawn_pos = p->pos; p->spawn_yaw = p->yaw;
    printf("player: start (%.0f %.0f %.0f) yaw %.1f deg, %u trigger volumes\n", p->pos.x, p->pos.y, p->pos.z, p->yaw * 57.2958f, p->nvol);
    if (wenv("WOODY_VOLDUMP")) for (uint32_t v = 0; v < p->nvol; v++)   /* id of every trigger volume (VOL_FLAG5 in the level script) and the instance carrying it */
        printf("  VOL 0x%x: inst %u at %.0f %.0f %.0f", p->vol_id[v], p->vol_inst[v]->index, p->vol_inst[v]->position.x, p->vol_inst[v]->position.y, p->vol_inst[v]->position.z), puts("");
    return 0;
}

float gel_ray_hit(const GelFile *g, Vec3 a, Vec3 b, Vec3 *n_out)
{
    float best = 2.0f;
    GelPolySet ps = gel_polys_on_seg(g, a, b);
    for (uint32_t k = 0; k < ps.n; k++) {
        const GelPoly *pl = &g->polys[ps.polys[k]]; if (pl->nverts < 3) continue;
        float da = pl->plane[0] * a.x + pl->plane[1] * a.y + pl->plane[2] * a.z + pl->plane[3];
        float db = pl->plane[0] * b.x + pl->plane[1] * b.y + pl->plane[2] * b.z + pl->plane[3];
        if ((da > 0) == (db > 0)) continue;
        float t = da / (da - db); if (t >= best) continue;
        Vec3 q = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
        if (!poly_contains(g, pl, q)) continue;
        best = t;
        if (n_out) { *n_out = (Vec3){ pl->plane[0], pl->plane[1], pl->plane[2] };                  /* towards the side the ray came from */
                     if (da < 0) { n_out->x = -n_out->x; n_out->y = -n_out->y; n_out->z = -n_out->z; } }
    }
    return best;
}
float gel_ray_frac(const GelFile *g, Vec3 a, Vec3 b) { return gel_ray_hit(g, a, b, NULL); }

/* the endless ray 0x497a30(a, dir, -1) (docs/OBSTACLE.md 1): the world part 0x497b10 walks the cells from the one holding a
 * (0x408180) through their exit faces and answers 3 with t = the first polygon, or 1 when the ray leaves the world; then every
 * instance of the visited cells and of the dynamic list answers through vtbl[6] 0x431de0 with its press nodes, and a nearer
 * one makes it 4. t is in units of dir, not normalised: the callers pass an unnormalised dir and read it that way.
 * Port: a segment to where the ray leaves the level's bounding box, world polygons and then instance press nodes up to the
 * world hit, as the endless laser does. Like 0x497c5a only the world polygons that face a count (plane(a) >= 0, the ray
 * going in). On 1 the original leaves [0x4c4bd4] stale, which here is simply "no hit".
 * The instance side (0x497a61..0x497b09): the instances registered in the cells the world walk visited (gel_walk_seg,
 * endless: up to the world hit or out of the world) and then the dynamic list, each through vt[6] 0x431de0: per press node,
 * the FIRST polygon in list order that a sees from its front (loader plane >= 0, 0x432169) and that the ray enters (every
 * edge (v_k - a) x (v_k+1 - a) . dir > 0, 0x432261) is the node's only candidate: t = -f(a) / n.dir, recorded when
 * t < [0x4c4bd4] (strict, 0x43230a) - then or otherwise the node is done (0x432447). So the nearest wins, the first tested
 * on a tie, and a node whose first entered polygon is farther hides a nearer one behind it in its list. */
static float gel_ray_front(const GelFile *g, Vec3 a, Vec3 b)
{
    float best = 2.0f;
    GelPolySet ps = gel_polys_on_seg(g, a, b);
    for (uint32_t k = 0; k < ps.n; k++) {
        const GelPoly *pl = &g->polys[ps.polys[k]]; if (pl->nverts < 3) continue;
        float da = pl->plane[0] * a.x + pl->plane[1] * a.y + pl->plane[2] * a.z + pl->plane[3];
        float db = pl->plane[0] * b.x + pl->plane[1] * b.y + pl->plane[2] * b.z + pl->plane[3];
        if (da < 0 || db >= 0) continue;                                   /* 0x497c73 / 0x497ca2: back faces and parallel rays are skipped */
        float t = da / (da - db); if (t >= best) continue;
        Vec3 q = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
        if (poly_contains(g, pl, q)) best = t;
    }
    return best;
}
int player_ray_endless(const Player *p, Vec3 a, Vec3 dir, float *t)
{
    const float *bb = p->gel->bbox; const float o[3] = { a.x, a.y, a.z }, d[3] = { dir.x, dir.y, dir.z }; float K = 1e30f;
    for (int k = 0; k < 3; k++) {
        if (d[k] > 1e-6f) { float e = (bb[2 * k + 1] - o[k]) / d[k]; if (e < K) K = e; }
        else if (d[k] < -1e-6f) { float e = (bb[2 * k] - o[k]) / d[k]; if (e < K) K = e; }
    }
    if (!(K < 1e29f)) return 1;
    if (K < 1.0f) K = 1.0f;
    Vec3 b = { a.x + dir.x * K, a.y + dir.y * K, a.z + dir.z * K }, dd = vsub(b, a), v[32];
    float f = gel_ray_front(p->gel, a, b); int kind = f <= 1.0f ? 3 : 1;
    const GelColRef *cr; uint32_t ncr = 0;
    if (p->ins) { gel_walk_seg(p->gel, a, b, kind == 3 ? f : 2.0f, 1); ncr = gel_col_instances(p->gel, p->ins, &cr); }
    for (uint32_t q = 0; q < ncr; q++) {
        const Instance *in = cr[q].in; const Model *m = in->model; uint32_t ncn; const uint32_t *cn = ins_collision_nodes(m, &ncn);
        for (uint32_t ci = 0; ci < ncn; ci++) {
            const InsNode *nd = &m->nodes[cn[ci]];
            for (uint32_t pi = 0; pi < nd->npolys; pi++) {
                const InsPoly *pl = &nd->polys[pi]; if (pl->nverts < 3 || pl->nverts > 32) continue;
                for (uint32_t c = 0; c < pl->nverts; c++) v[c] = ins_point_world(in, pl->indices[c]);
                float P[4]; if (!loader_plane(v, pl->nverts, P)) continue;
                float fa = P[0] * a.x + P[1] * a.y + P[2] * a.z + P[3]; if (fa < 0) continue;   /* 0x432174 */
                uint32_t e = 0;
                for (; e < pl->nverts; e++) { Vec3 u = vsub(v[e ? e - 1 : pl->nverts - 1], a), w = vsub(v[e], a); if (!(vdot(vcross(u, w), dd) > 0)) break; }
                if (e < pl->nverts) continue;                                 /* 0x43226c: not entered, next polygon */
                float den = P[0] * dd.x + P[1] * dd.y + P[2] * dd.z;
                if (den < 0) { float ti = -fa / den; if (ti < (kind == 1 ? 1e30f : f)) { f = ti; kind = 4; } }   /* 0x432302..0x432315 */
                break;                                                          /* 0x432447: the node is done */
            }
        }
    }
    *t = kind == 1 ? 0 : f * K;
    return kind;
}

int player_segment_blocked(const Player *p, Vec3 a, Vec3 b) { return gel_ray_frac(p->gel, a, b) <= 1.0f; }

/* ---- logical animations: table 0x4b6180 (0x1c bytes per record: sub[4], prio, speed, restart) --------------
 * A logical animation is a chain of up to four .ins animations played in order, the last one looping (-1 = hold);
 * the speed divides the .ins duration (docs/PERSO_JUMP.md 4). Priorities are not used here. The 128 records are
 * copied from the exe: 0x1a..0x1e results, 0x25..0x30 deaths, 0x3b..0x3e rocket, 0x5c..0x7f the race rider (docs/RACE.md). */
typedef struct { int sub[4]; int prio; float speed; int restart; } LogAnim;
static const LogAnim *log_anim(int n)
{
    static const LogAnim t[0x80] = {
        {{0,0,0,0},1100,3,0}, {{0,0,0,0},6500,3,0}, {{1,2,2,2},1501,3,1}, {{2,2,2,2},1500,3,0}, {{3,4,4,4},1501,8,1}, {{5,6,7,7},1050,2,1}, {{6,7,7,7},1000,3,0}, {{6,7,7,7},5000,3,1},
        {{8,0,0,0},1500,3,1}, {{7,32,33,33},1600,3,0}, {{34,0,0,0},5200,3,1}, {{9,9,9,9},1600,3,1}, {{10,11,-1,-1},1600,3,1}, {{16,0,0,0},1600,3,1}, {{19,6,7,7},1600,3,1}, {{21,13,13,13},1600,3,1},
        {{38,51,51,51},1700,3,1}, {{51,51,51,51},1700,3,0}, {{51,52,0,0},1700,3,0}, {{86,0,0,0},5500,3,1}, {{12,13,13,13},5001,5,1}, {{13,13,13,13},5000,3,0}, {{14,6,7,7},5000,3,0}, {{15,-1,-1,-1},5002,3,0},
        {{17,-1,-1,-1},6000,3,1}, {{18,-1,-1,-1},6000,3,1}, {{74,-1,-1,-1},6000,3,1}, {{75,-1,-1,-1},6000,3,1}, {{78,-1,-1,-1},6000,3,1}, {{76,-1,-1,-1},6000,3,1}, {{77,-1,-1,-1},6000,3,1}, {{20,0,0,0},5110,3,1},
        {{22,0,0,0},5110,3,1}, {{68,47,47,47},5110,3,1}, {{67,47,47,47},5110,3,1}, {{26,25,25,25},5110,3,1}, {{64,60,60,60},5110,3,1}, {{29,30,30,30},6000,3,1}, {{28,30,30,30},6000,3,1}, {{69,30,30,30},6000,3,1},
        {{70,30,30,30},6000,3,1}, {{31,-1,-1,-1},6000,3,1}, {{27,-1,-1,-1},6000,3,1}, {{37,-1,-1,-1},6000,3,1}, {{36,-1,-1,-1},6000,3,1}, {{63,-1,-1,-1},6000,3,1}, {{35,-1,-1,-1},6000,3,1}, {{48,33,33,33},6000,3,1},
        {{85,-1,-1,-1},6000,3,1}, {{24,25,25,25},1750,4,1}, {{25,25,25,25},1750,3,0}, {{23,0,0,0},1750,4,1}, {{39,0,0,0},1750,3,1}, {{40,0,0,0},1750,3,1}, {{45,-1,-1,-1},1800,3,1}, {{41,-1,-1,-1},1800,3,1},
        {{42,43,43,43},1800,3,1}, {{43,43,43,43},1800,3,1}, {{44,7,7,7},1800,3,1}, {{84,-1,-1,-1},1800,3,1}, {{80,-1,-1,-1},1800,3,1}, {{81,82,82,82},1800,3,1}, {{82,82,82,82},1800,3,1}, {{83,7,7,7},1800,3,1},
        {{47,47,47,47},1100,3,0}, {{50,49,49,49},1501,3,1}, {{49,49,49,49},1500,3,0}, {{61,0,0,0},1500,3,1}, {{62,7,7,7},1500,3,1}, {{46,47,47,47},1500,3,1}, {{71,0,0,0},1500,3,1}, {{53,54,54,54},1501,3,1},
        {{55,56,56,56},1000,3,0}, {{55,56,56,56},5000,3,1}, {{57,47,47,47},1500,3,1}, {{56,66,33,33},1600,3,0}, {{47,47,47,47},5200,3,1}, {{65,33,33,33},6000,3,1}, {{58,60,60,60},1750,4,1}, {{60,60,60,60},1750,3,0},
        {{59,47,47,47},1750,4,1}, {{72,0,0,0},6000,3,1}, {{73,0,0,0},6000,3,1}, {{74,0,0,0},6000,3,1}, {{75,0,0,0},6000,3,1}, {{76,0,0,0},6000,3,1}, {{77,0,0,0},6000,3,1}, {{78,0,0,0},6000,3,1},
        {{79,0,0,0},6000,3,1}, {{0,88,87,87},1110,3,0}, {{89,89,89,89},1110,3,0}, {{0,90,0,0},1110,3,0}, {{0,0,0,0},1100,3,1}, {{42,0,0,0},1100,3,1}, {{1,20,8,8},1501,2,1}, {{20,8,8,8},1501,6,1},
        {{8,8,8,8},1501,3,1}, {{21,2,0,0},1501,6,1}, {{2,0,0,0},1501,3,1}, {{3,22,9,9},1501,2,1}, {{22,9,9,9},1501,6,1}, {{9,9,9,9},1501,3,1}, {{23,4,0,0},1501,6,1}, {{4,0,0,0},1501,3,1},
        {{5,6,6,6},1750,5,1}, {{6,6,6,6},1750,3,1}, {{7,0,0,0},1750,5,1}, {{24,25,25,25},1501,8,1}, {{29,26,27,27},1500,3,1}, {{26,27,27,27},1000,3,0}, {{26,27,27,27},5000,3,1}, {{28,0,0,0},1500,3,1},
        {{30,32,33,33},6000,3,1}, {{34,-1,-1,-1},6000,3,1}, {{38,-1,-1,-1},6000,3,1}, {{36,-1,-1,-1},6000,3,1}, {{37,-1,-1,-1},6000,3,1}, {{39,40,40,40},6000,3,1}, {{39,-1,-1,-1},6000,3,1}, {{41,-1,-1,-1},6000,3,1},
        {{10,0,0,0},6000,3,1}, {{11,0,0,0},6000,3,1}, {{12,0,0,0},6000,3,1}, {{13,0,0,0},6000,3,1}, {{14,0,0,0},6000,3,1}, {{15,0,0,0},6000,3,1}, {{16,0,0,0},6000,3,1}, {{17,0,0,0},6000,3,1},
    };
    return (n >= 0 && n < 0x80) ? &t[n] : &t[0];
}
/* 0x463e30: a scripted action names a raw .ins animation; the logical record is the first one that starts with it */
static int log_from_raw(int act) { for (int n = 0; n < 0x80; n++) if (log_anim(n)->sub[0] == act) return n; return -1; }
static float anim_len(const Player *p, int n, int k)                       /* 0x436b90 AnimLen(n, k) */
{
    const LogAnim *a = log_anim(n); const Model *m = p->inst->model; int s = a->sub[k];
    return (s >= 0 && (uint32_t)s < m->nanims) ? m->anims[s].duration_s / a->speed : 0.0f;
}
/* advance through the chain of logical animation n. Runs in the request and again after the clock (player_anim_settle):
 * the pose wraps the time (ins_pose), so a sub-animation the clock has just carried past its end would show its first
 * frame for one frame before the next request moves on - Woody stood up for a frame between going down (.ins 24) and
 * lying (25), and crouched for one at the end of getting up (23). */
static void ctl_chain(Instance *wi, int n, int *lsub)
{
    const LogAnim *a = log_anim(n); const Model *m = wi->model;
    if (n < 0) return;
    int nx = *lsub < 3 ? a->sub[*lsub + 1] : 0;
    if ((uint32_t)wi->anim < m->nanims && *lsub < 3 && (nx < 0 || (uint32_t)nx >= m->nanims)) {     /* hold the last frame: the clock advances after this, so stop it just before the end */
        if (wi->anim_time >= m->anims[wi->anim].duration_s - 0.15f) { wi->anim_time = m->anims[wi->anim].duration_s * 0.999f; wi->anim_speed = 0; }
    } else if ((uint32_t)wi->anim < m->nanims && wi->anim_time >= m->anims[wi->anim].duration_s && *lsub < 3) {
        { wi->anim_time -= m->anims[wi->anim].duration_s; wi->anim = nx; (*lsub)++; }
    }
}
/* one anim controller (class 0x4ab7b4) on one instance: the rider's +0x494 or the race board's +0x498 */
static void ctl_request(Instance *wi, int *lanim, int *lsub, int n, float rate, const Player *log)
{
    const LogAnim *a = log_anim(n); const Model *m = wi->model;
    if (n != *lanim) {
        if (wenv("WOODY_ANIMLOG")) { if (log) printf("  lanim %d -> %d (jumper %d t %.3f)\n", *lanim, n, log->jumper.state, log->jumper.t); else printf("  board lanim %d -> %d\n", *lanim, n); }
        *lanim = n; *lsub = 0;
        if ((uint32_t)a->sub[0] < m->nanims && (a->restart || wi->anim != a->sub[0])) { wi->anim = a->sub[0]; wi->anim_time = 0; }
    }
    wi->anim_speed = a->speed * rate;
    ctl_chain(wi, *lanim, lsub);
}
static void anim_request(Player *p, int n, float rate) { ctl_request(p->inst, &p->lanim, &p->lanim_sub, n, rate, p); }   /* 0x436b70 Request + 0x436a50 Tick */
/* the board's controller B = +0x498 (docs/RACE.md 7): created by 1120 without a request, so at the level start (SurfEnter runs
 * before the script's 1120, which comes from the start volume in the first frame) the board shows nothing of the start anim
 * and keeps its pose until the first lean, jump or crouch; B->Reset() = board_lanim -1. Ticked every frame (0x463e60). */
static void board_request(Player *p, int n) { if (p->board && p->board != p->inst) ctl_request(p->board, &p->board_lanim, &p->board_lanim_sub, n, 1.0f, NULL); }
/* the slot step of the instance clock 0x43eee0 (0x43f0c9: past the end of slot0 it moves slot1..3 up) for the instance `in` just
 * clocked, so the event scan 0x42f5e0 that follows sees the new part (animPrev != animNow) and not a wrap of the old one */
void player_anim_settle(Player *p, const Instance *in)
{
    if (p->inst && p->inst == in && p->inst->model) ctl_chain(p->inst, p->lanim, &p->lanim_sub);
    if (p->board && p->board != p->inst && p->board == in && p->board->model) ctl_chain(p->board, p->board_lanim, &p->board_lanim_sub);
}
static void board_tick(Player *p) { if (p->board_lanim >= 0) board_request(p, p->board_lanim); }
static void race_request(Player *p, int n) { anim_request(p, n, 1.0f); board_request(p, n); }   /* A->Request(n) + B->Request(n) */

/* ---- attack controller 0x457a50 + trigger 0x457330 (docs/PERSO_JUMP.md 2) -----------------------------------
 * Ported: peck dash with auto-aim (1,2), hit pause and recoil (3,4,5), wall/ground rebound (6,7), charge run with
 * auto-steer (9,10), brake (11) also at a ledge (check_steep, 0x44b2e0), hit loop against the enemies. Not ported:
 * peckable surfaces (8), rumble. The dash additionally ends on landing, which the original leaves to its ray probe. */
static void lock_move(Player *p, float t) { p->move_lock = t; p->ramp_phase = 0; p->speed = 0; }   /* 0x44cce0 */

/* ---- standing still, 0x464500 (docs/PERSO_MOVE.md 4.3) ---------------------------------------------------------
 * +0x230 counts the seconds he stands idle. Up to 10 s he plays idle anim 0; when the count passes 10 he picks a
 * variation, 0x43ff20(0, 7) = rand() % 7 == 0 (one in SEVEN, not eight): 0x5a (.ins 89) for twice its length and then
 * the count starts over, otherwise 0x59 = .ins 0 -> 88 (sits down) -> 87 (asleep, loops). Once .ins 87 plays and the
 * count is past 10.5 s he gets the zzz bubble, whose live flag is +0x52c: it lasts until the count is reset. */
static void idle_reset(Player *p) { p->idle_t = 0; p->sleep_bubble = 0; }   /* 0x464620 */
static int idle_anim(Player *p, float dt)
{
    if (p->idle_hold) return 0;                                             /* Perso state != 0: 0x464630 neither calls it nor resets */
    int first = p->idle_t <= 10.0f;
    p->idle_t += dt;
    if (p->idle_t <= 10.0f) return 0;
    if (first) p->idle_var = rand() % 7 == 0;
    int want = p->idle_var ? 0x5a : 0x59;
    if (p->idle_var && anim_len(p, 0x5a, 0) * 2.0f + 10.0f <= p->idle_t) idle_reset(p);
    if (p->inst->anim == 0x57 && p->idle_t > 10.5f && !p->sleep_bubble) { p->sleep_bubble = 1; game_bubble(p->inst, 4, 2.5f, 130.0f, 50.0f, &p->sleep_bubble); }
    return want;
}
/* did the fraction of a looping animation pass `t` between the previous frame and this one? */
static int phase_passed(float prev, float cur, float t) { return cur >= prev ? (t > prev && t <= cur) : (t > prev || t <= cur); }
static void jumper_reset(Jumper *j) { memset(j, 0, sizeof *j); j->state = 2; j->armed = 1; }       /* 0x462c90 */
static void jumper_force_fall(Jumper *j, int force)                                                 /* 0x463170 */
{
    if ((j->state == 3 || j->state == 4 || j->state == 5) && !force) return;
    j->coyote = 0; jumper_start_fall(j, 1); j->v_down = 0; j->state = 4;
}
typedef struct { int kind; float t; const Instance *in; const InsNode *node; Vec3 n; } InsRayHit;
static int ray_instances(const Player *p, const Instance *skip, Vec3 a, Vec3 b, float t_world, InsRayHit *h);
/* 0x464e00(p, inst): may he hang in this wall? inst = the hit instance when the polygon is peckable, else NULL. No new grab
 * while the let-go timer +0x524 runs or when dead (+0x26c); the wall must be (nearly) vertical, |n.y| <= 0.05 (0x4ab7d0) */
static int climb_grab(Player *p, Vec3 n, const Instance *inst)
{
    if (p->regrab > 0 || p->dead_kind || !inst || fabsf(n.y) > 0.05f) return 0;
    float l = sqrtf(n.x * n.x + n.z * n.z);
    p->wall_n = l > 0 ? (Vec3){ n.x / l, 0, n.z / l } : (Vec3){ n.x, 0, n.z }; p->wall_inst = inst;   /* +0x510..0x518, +0x51c */
    return 1;
}
/* 0x4575b0: ONE ray 0x4359b0 from the feet + 5 along v, world and instances (the last instance polygon tested wins and
 * replaces a world hit, as everywhere). Any hit sparks; a peckable press node (type code 4) that 0x464e00 accepts sticks the
 * beak in (substate 8, docs/PERSO_JUMP.md 2.4), anything else bounces him off (6 / 7). Before the grab came only from the
 * ground grab ray of 0x464ef0 (feet + 40, 169 ahead) every frame of the dash, so he grabbed up to 100 units out from the wall
 * and 35 higher than the original can; a grab just below the top then went straight into the climb-over that far out, which
 * set him down in front of the wall instead of on it (and into the pit below in a side section) */
static int attack_probe(Player *p, Vec3 v)                                                          /* 0x4575b0 */
{
    Vec3 a = { p->pos.x, p->pos.y + 5.0f, p->pos.z }, b = { a.x + v.x, a.y + v.y, a.z + v.z }, face_n;
    float f = gel_ray_hit(p->gel, a, b, &face_n); InsRayHit h;
    int k = ray_instances(p, NULL, a, b, f <= 1.0f ? f : 2.0f, &h);
    if (k) { f = h.t; face_n = h.n; if (k == 3) { float l = sqrtf(vdot(v, v)); face_n = l > 1e-6f ? (Vec3){ -v.x / l, -v.y / l, -v.z / l } : (Vec3){ 0, 1, 0 }; } }
    if (f > 1.0f) return 0;
    /* 0x4575b0 fires the impact on ANY hit: kind 1, no normal, a 0.05 s flash (docs/PARTICLES.md 4) */
    game_peck_fx(1, (Vec3){ a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f }, &face_n);
    if (climb_grab(p, face_n, k == 2 && h.node->type_code == 4 ? h.in : NULL)) {   /* 0x4576a0..0x4576e8 */
        p->atk = 8; p->atk_t = anim_len(p, 0xf, 0); lock_move(p, p->atk_t);     /* 0x4576ee..0x457726 */
        p->yaw = atan2f(-p->wall_n.x, -p->wall_n.z); p->speed = 0; p->ramp_phase = 0; p->vel = (Vec3){ 0, 0, 0 };   /* RampA.dir = M.velDir = M.dir = -n */
        rumble(p, 0.5f, 0.5f);
        printf("  CLIMB beak in instance %u (substate 8)\n", h.in->index);
        return 1;
    }
    int found; const Instance *hi; const InsNode *hn;
    float gy = world_ground(p, (Vec3){ p->pos.x, p->pos.y + 1.0f, p->pos.z }, &found, &hi, &hn);
    int n = (found && p->pos.y - gy > 100.0f) ? 0xe : 0xd;
    p->atk = n == 0xe ? 7 : 6; p->atk_t = anim_len(p, n, 0); lock_move(p, p->atk_t);
    rumble(p, 0.5f, 0.5f);                                                                         /* P+0x9c, P+0x98 */
    return 1;
}
/* target finder 0x4632e0 / 0x463420 (finder object Perso+0x604): the instances of this frame's list world+0x64 whose type
 * word (vtbl[4] 0x403fe0 = inst+0x104) has bit 0x400, strictly within r of the feet (3D, instance origin +0xc), at most the
 * first 16 in list order, bubble-sorted by distance; the first is taken. Bit 0x400 = Enemy.attackable: set by the enemy
 * PostLoad 0x419e30 / Reset 0x41a010, cleared in the dead state of types 4..13, never for the bosses 14..16; 1201 / 1202
 * (enemies_msg1201) are never sent. 0x463303: the walk over the list stops as soon as 16 candidates are in (so a 17th enemy is
 * never seen, however near); 0x4633a5: bubble sort, swapping only when d[j] > d[j + 1] (ties keep the list order), +0x84 = 0;
 * 0x463420 returns entry 0. Only the enemies carry bit 0x400 in the port (as in the shipped game) */
static Enemy *enemy_of(const Player *p, const Instance *in)
{
    for (int i = 0; i < p->enemies->n; i++) if (p->enemies->e[i].inst == in) return &p->enemies->e[i];
    return NULL;
}
static Enemy *nearest_enemy(Player *p, float r)
{
    if (!p->enemies) return NULL;
    Instance *const *list; uint32_t nl = game_instance_list(&list);
    Enemy *c[16]; float cd[16]; int n = 0;
    for (uint32_t k = 0; list && k < nl && n < 16; k++) {                    /* 0x463303 */
        Enemy *e = enemy_of(p, list[k]); if (!e || e->removed || !e->attackable) continue;   /* vtbl[4] 0x403fe0, bit 0x400 (0x463323) */
        Vec3 d = vsub(list[k]->position, p->pos); float dd = sqrtf(vdot(d, d));   /* instance origin +0xc against the feet */
        if (dd < r) { c[n] = e; cd[n] = dd; n++; }
    }
    if (!list)                                                               /* no renderer (tests without a window): every thinking enemy */
        for (int i = 0; i < p->enemies->n && n < 16; i++) {
            Enemy *e = &p->enemies->e[i]; if (e->removed || !e->attackable || !e->inst->visible || !game_enemy_thinks(e->inst)) continue;
            Vec3 d = vsub(e->inst->position, p->pos); float dd = sqrtf(vdot(d, d)); if (dd < r) { c[n] = e; cd[n] = dd; n++; }
        }
    for (int i = 0; i < n; i++)                                              /* 0x4633a5 */
        for (int j = 0; j < n - i - 1; j++)
            if (cd[j] > cd[j + 1]) { float t = cd[j]; cd[j] = cd[j + 1]; cd[j + 1] = t; Enemy *q = c[j]; c[j] = c[j + 1]; c[j + 1] = q; }
    return n ? c[0] : NULL;
}
static void auto_aim(Player *p)                                          /* 0x4579a0: the charge run steers to the nearest enemy */
{
    Enemy *t = nearest_enemy(p, 500.0f); if (!t) return;
    float dx = t->pos.x - p->pos.x, dz = t->pos.z - p->pos.z; if (dx * dx + dz * dz > 1.0f) p->yaw = atan2f(dx, dz);
    p->target = t;
}
/* the beak vector p+0x59c / p+0x5a8: 0x44b4a0 (frame step 24, after the render) stores marker typecode 0 of the Perso's own
 * mesh (0x42f6b0(p, 0, &p+0x59c, 0)) in world space, +0x5cc = found; without one the hit loop uses the instance position
 * for both (0x457d1b, with the "Please get the latest version of the Woody mesh" log). The port reads the pose of the last draw. */
static void beak_vector(const Player *p, Vec3 *a, Vec3 *b)
{
    const Instance *in = p->inst; const Model *mo = in->model;
    for (uint32_t i = 0; i < mo->nnodes; i++) if (mo->nodes[i].kind == 0x20 && mo->nodes[i].type_code == 0 && mo->nodes[i].npoints >= 2) {
        *a = ins_point_world(in, mo->nodes[i].point_base); *b = ins_point_world(in, mo->nodes[i].point_base + 1); return; }
    *a = *b = in->position;
}
/* hit loop 0x457ceb..0x458b96 (docs/PERSO_JUMP.md 3), over every actor of the list (t != p), tp = t->vtbl[34]() = its feet:
 *  dash (substate 2): 0x433920(&dash start p+0x5e4, &p+0x1f4 (the position), 100, &tp, t->vtbl[32]() radius, t->vtbl[33]() height)
 *    = a sphere of 100 swept along the dash against the target's capsule-shaped cylinder; hit point = the beak tip p+0x59c, dir 0;
 *  otherwise (the charge run 9/10, and every actor after a dash hit, whose substate is 3 by then): the beak segment
 *    a = p+0x59c, b = a + normalize(p+0x5a8 - a) * 50 (0x4a9030, written back to p+0x5a8) against 0x433de0(a, b, tp + (0, h/2, 0),
 *    radius, h); a hit writes [0x53a558] = 0.5, hit point = lerp(a, b, [0x53a558]), dir = normalize_xz(tp - p+0x1f4); in
 *    substate 10 also Mover_SetDir(M, dir) 0x459ff0 (he turns to the target at once).
 * The loop does not stop on a hit. The target handles it: vtbl[39](p, 1.0, &dir, &hitpoint, isPeck) (0x458b53), whose
 * Enemy_TakeDamage 0x41adc0 puts the hit star 0x4750e0 on the hit point; a true answer = vtbl[38](3) (inside enemy_hit). */
static void attack_hit_loop(Player *p)
{
    if (!p->enemies) return;
    Vec3 ba, bb; beak_vector(p, &ba, &bb);                               /* 0x457ceb: without the marker both are the instance position */
    { Vec3 d = vsub(bb, ba); float l = sqrtf(vdot(d, d)); if (l > 0) d = (Vec3){ d.x / l, d.y / l, d.z / l };   /* 0x4588af..0x458987 */
      bb = (Vec3){ ba.x + d.x * 50.0f, ba.y + d.y * 50.0f, ba.z + d.z * 50.0f }; }
    for (int i = 0; i < p->enemies->n; i++) {
        Enemy *e = &p->enemies->e[i]; if (e->removed || !e->attackable || !e->inst->visible) continue;
        float r = enemy_radius(e), h = enemy_height(e); Vec3 tp = e->pos, dir = { 0, 0, 0 }, pt;
        int peck = p->atk == 2;
        if (peck) {
            if (!sweep_sphere_cyl(p->dash_start, p->pos, 100.0f, tp, r, h)) continue;   /* 0x457dcb */
            pt = ba;                                                                     /* 0x458a10 */
        } else {
            if (!(seg_cyl(ba, bb, (Vec3){ tp.x, tp.y + h * 0.5f, tp.z }, r, h) >= 0)) continue;   /* 0x4589d0 */
            g_hit_frac = 0.5f;                                                           /* 0x4589e9: the answer, always 0.5 */
            float dx = tp.x - p->pos.x, dz = tp.z - p->pos.z, l = sqrtf(dx * dx + dz * dz);   /* 0x458a3c */
            dir = l > 0 ? (Vec3){ dx / l, 0, dz / l } : (Vec3){ dx, 0, dz };
            pt = (Vec3){ ba.x * (1 - g_hit_frac) + bb.x * g_hit_frac, ba.y * (1 - g_hit_frac) + bb.y * g_hit_frac, ba.z * (1 - g_hit_frac) + bb.z * g_hit_frac };
            if (p->atk == 10) p->yaw = atan2f(dir.x, dir.z);                            /* 0x458b07: Mover_SetDir */
        }
        if (peck) p->atk = 3;                                                            /* 0x458b18 */
        rumble(p, 0.5f, 0.3f);                                                           /* P+0xa4, P+0xa0; no SoundFx 6 here: that is the bomb explosion (0x44d730 is in Bomb_Explode 0x44d6e0) */
        int died = enemy_hit(e, 1.0f /* P+0x90 */, dir, pt, peck);         /* isPeck = 1 for the dash, 0 otherwise */
        printf("  ATTACK hit enemy %u (%s)%s at %.0f %.0f %.0f (feet %.0f %.0f %.0f)\n", e->inst->index, peck ? "peck" : "charge", died ? " - dead" : "", pt.x, pt.y, pt.z, p->pos.x, p->pos.y, p->pos.z);
    }
}

/* 0x44b2e0, the ledge sensor of the charge run (docs/OBSTACLE.md 2), at the end of the Perso frame (after the collision,
 * before Orient 0x44bd00), so the attack controller reads the previous frame's answer. On the ground it casts the endless
 * ray from the collision centre (feet + P+0 = 43) towards the floor point P+0x80 = 40 ahead along the facing: dir =
 * (40·f.x, -43, 40·f.z). Flat floor answers t = 1, a wall t < 1; only a first hit (world or press node) at t > 3, i.e.
 * more than 86 below the feet and 120 or more ahead, sets +0x234. A ray that finds nothing (1) does not. */
static void check_steep(Player *p)
{
    p->steep_edge = 0;                                                   /* 0x44b2fb */
    if (!p->on_ground) return;                                           /* 0x44bcf0 = +0x22c */
    Vec3 a = { p->pos.x, p->pos.y + P_PROBE_Y, p->pos.z }, dir = { sinf(p->yaw) * P_EDGE_LOOK, -P_PROBE_Y, cosf(p->yaw) * P_EDGE_LOOK };
    float t; int k = player_ray_endless(p, a, dir, &t);
    if ((k == 3 || k == 4) && t > P_EDGE_T) p->steep_edge = 1;          /* 0x44b43e..0x44b45b */
    if (wenv("WOODY_EDGELOG") && (p->atk == 9 || p->atk == 10))
        printf("  EDGE atk %d pos %.0f %.0f %.0f kind %d t %.2f -> %d\n", p->atk, p->pos.x, p->pos.y, p->pos.z, k, t, p->steep_edge);
}

static void attack_update(Player *p, const PlayerInput *in, float dt)
{
    p->use_atk_disp = 0;
    if (p->charge > 0) p->charge -= dt;
    Vec3 dir = { sinf(p->yaw), 0, cosf(p->yaw) };
    switch (p->atk) {
    case 1: {                                                             /* dash start: aim at the nearest attackable target within 500 that is > 50 below, else diagonally down */
        Enemy *t = nearest_enemy(p, 500.0f); p->has_target = 0; p->target = NULL;
        p->atk_dir = (Vec3){ dir.x, -2.0f, dir.z };
        if (t) {
            p->aim = t->pos; p->aim.y += enemy_height(t) * 0.8f; p->target = t;
            if (p->pos.y - p->aim.y > 50.0f) { p->atk_dir = vsub(p->aim, p->pos); p->has_target = 1; enemy_warn_dive(t, p->atk_dir);   /* 0x457f90 -> vtbl[37] */ float yl = sqrtf(p->atk_dir.x * p->atk_dir.x + p->atk_dir.z * p->atk_dir.z); if (yl > 0.01f) p->yaw = atan2f(p->atk_dir.x, p->atk_dir.z); }
        }
        p->dash_start = p->pos; audio_fx(55 + rand() % 3, NULL, NULL);          /* 0x45752b: air attack cry, 0x37 + rand(0,3) */
        { float l = sqrtf(vdot(p->atk_dir, p->atk_dir)); p->atk_dir.x /= l; p->atk_dir.y /= l; p->atk_dir.z /= l; }
        p->jumper.fallen = 0; p->jumper.hard_fall = 0; p->air_win = 0; p->am_win = 0; p->atk = 2; return; }   /* 0x457560(0, 1), 0x465e00(0, 1) */
    case 2:
        p->atk_disp = (Vec3){ p->atk_dir.x * dt * 1500.0f, p->atk_dir.y * dt * 1500.0f, p->atk_dir.z * dt * 1500.0f }; p->use_atk_disp = 1;
        if (!attack_probe(p, (Vec3){ p->atk_dir.x * 50.0f, p->atk_dir.y * 50.0f, p->atk_dir.z * 50.0f }))
            attack_probe(p, (Vec3){ dir.x * 100.0f, 0, dir.z * 100.0f });
        if (p->atk == 2 && p->on_ground) { p->atk = 6; p->atk_t = anim_len(p, 0xd, 0); lock_move(p, p->atk_t); }
        if (p->atk == 2) attack_hit_loop(p);
        return;
    case 3: p->atk = 4; p->target = NULL; p->atk_t = anim_len(p, 0xc, 0); p->atk_disp = (Vec3){ 0, 0, 0 }; p->use_atk_disp = 1; return;   /* hit: hang still */
    case 4:
        p->use_atk_disp = 1; p->atk_disp = (Vec3){ 0, 0, 0 };
        if ((p->atk_t -= dt) > 0) return;
        /* recoil direction (-dir.x, 0.8, ~0): the original uses dir.y for z at 0x458686, so z is ~0 */
        p->atk_dir = (Vec3){ -dir.x, 0, 0 }; { float l = fabsf(p->atk_dir.x); if (l > 1e-4f) p->atk_dir.x /= l; p->atk_dir.y = 0.8f; l = sqrtf(vdot(p->atk_dir, p->atk_dir)); p->atk_dir.x /= l; p->atk_dir.y /= l; }
        p->atk_t = 0.75f; p->atk = 5; return;
    case 5:
        p->atk_t -= dt; { float v = dt * p->atk_t * 1000.0f; p->atk_disp = (Vec3){ p->atk_dir.x * v, p->atk_dir.y * v, p->atk_dir.z * v }; } p->use_atk_disp = 1;
        if (p->atk_t < 0.5f && !(p->air_win > 0)) { p->air_win = 0.5f; if (p->am_win < 0.5f) p->am_win = 0.5f; }   /* chained attack possible (0x458873: 0x465e00(0.5) too) */
        if (p->atk_t <= 0) { jumper_force_fall(&p->jumper, 1); p->atk = 0; }
        return;
    case 6:
        jumper_reset(&p->jumper);
        if ((p->atk_t -= dt) < 0.4f) p->vy_corr -= (p->pos.y - p->floor_y) * 2.5f;   /* spring toward the ground */
        if (p->atk_t <= 0) { p->pos.y = p->floor_y; p->on_ground = 1; p->atk = 0; }  /* 0x462990 snap to ground */
        return;
    case 7: jumper_reset(&p->jumper); if ((p->atk_t -= dt) <= 0) { p->atk = 0; jumper_force_fall(&p->jumper, 0); } return;
    case 8:                                                               /* 0x458524: the beak sticks in the wall for AnimLen(0xf), then */
        jumper_reset(&p->jumper);                                         /* SetState(4), anim 0x15, climbing (+0x50c = 2), grip 0.8 */
        if ((p->atk_t -= dt) <= 0) { p->atk = 0; p->climb_sub = 2; p->grip = 0.8f; p->peck_t = 0.3f; anim_request(p, 0x15, 1.0f); }
        return;
    case 9:
        if (p->steep_edge) {                                               /* 0x457abe: a ledge ahead ends the windup, before the aim */
            if (in->jump) { lock_move(p, 0); p->atk = 0; }                 /* 0x457ae1: action 4 held => LockMove(0, 1), the jump follows */
            else player_brake_charge(p);                                   /* 0x457b0a */
            return;
        }
        if ((p->atk_t -= dt) <= 0) p->atk = 10;
        if (p->atk9_first) p->atk9_first = 0; else auto_aim(p);            /* 0x457b6c: no aim on the first windup frame (+0x5fd) */
        dir = (Vec3){ sinf(p->yaw), 0, cosf(p->yaw) };
        p->use_atk_disp = 1; p->atk_disp = (Vec3){ dir.x * dt * 700.0f, 0, dir.z * dt * 700.0f }; attack_hit_loop(p); return;
    case 10:
        auto_aim(p); dir = (Vec3){ sinf(p->yaw), 0, cosf(p->yaw) };
        if (p->charge > 0 && !p->steep_edge && !in->jump) {               /* 0x457c09..0x457c3d: charge left, no ledge ahead, no jump */
            if ((p->charge_rumble_t -= dt) <= 0) { p->charge_rumble_t += 0.2f; rumble(p, 0.5f, 0.15f); }   /* 0x457c43: +0x600 (never reset), P+0xac, P+0xa8 */
            p->use_atk_disp = 1; p->atk_disp = (Vec3){ dir.x * dt * 700.0f, 0, dir.z * dt * 700.0f }; attack_hit_loop(p); return; }
        if (in->jump) { lock_move(p, 0); p->atk = 0; return; }             /* jump cancels the run */
        player_brake_charge(p); return;                                    /* 0x457e16: the charge ran out or a ledge is ahead */
    case 11: if ((p->atk_t -= dt) <= 0) p->atk = 0; return;
    default: return;
    }
}
/* ---- Perso state 6, carrying a bomb (docs/BOMB_CARRY.md) ---------------------------------------------------------
 * 0x463c90: every way out of state 6 other than the throw lets go of the bomb where it is, straight down at speed 0 */
static void bomb_drop(Player *p)
{
    p->state6 = 0;                                                          /* the callers stand for SetState(!= 6) */
    if (!p->bomb) return;
    game_bomb_launch(p->bomb, (Vec3){ 0, -1, 0 }, 0); p->bomb = NULL; p->carrying = 0;   /* 0x463c90 leaves the sub-state +0x58c alone */
}
/* 0x463530, after the animation choice: part A holds the bomb on the track of Woody's top-level 0x80 node (the node the
 * camera mode 0x80 reads) and lets go of it at a fixed moment of the throw animations; part B is the sub-state machine:
 * 0 / 1 pick-up (1.2 s, no walking), 2 carrying, attack -> 3 / 4 ground throw (0.933 s, no walking) or 5 / 6 air throw (0.6 s).
 * The room test before a ground throw (0x434830) is a stub in this build: always free. Sub-states 7 / 8 (putting it
 * down) are never reached. Part A needs the bomb in his hands (+0x594), part B only state 6: after the look-around bug
 * (docs/PERSO_LOOK.md 5) the sub-states run on without a bomb, and the next attack is an empty throw that ends state 6. */
static void carry_frame(Player *p, float dt)
{
    if (!p->state6) return;
    if (p->bomb) {
    int ins = p->inst->anim, rel = 0;
    switch (ins) {                                                          /* jump table 0x463bfc */
    case 61: rel = p->bt < 0.46667f; break;                                 /* ground throw (0x43) */
    case 62: rel = p->bt < 0.3f; break;                                     /* air throw (0x44) */
    case 66: rel = p->bt < 0.16667f; break;                                 /* long fall (0x4b): it drops out of his hands */
    }
    if (rel) {
        Vec3 dir = { 0, -1, 0 }; float sp = 0; int thrown = p->bsub == 4 || p->bsub == 6;
        if (thrown) { Vec3 f = { sinf(p->yaw), 1.0f, cosf(p->yaw) }; float l = sqrtf(vdot(f, f)); dir = (Vec3){ f.x / l, f.y / l, f.z / l }; sp = 1000.0f; }   /* 0x46380f: 45 degrees up */
        game_bomb_launch(p->bomb, dir, sp); p->bomb = NULL; p->carrying = 0; p->bsub = 0; p->state6 = 0;
        p->throw_hold = thrown ? p->bt : 0;                                 /* state 0 at once; the throw plays on under its move lock */
        if (wenv("WOODY_BOMBLOG")) printf("  BOMB leaves Woody's hands (%s)\n", thrown ? "thrown" : "dropped");
        return;
    }
    const Model *m = p->inst->model; Vec3 hand, tgt;
    float ph = (uint32_t)ins < m->nanims && m->anims[ins].duration_s > 0 ? p->inst->anim_time / m->anims[ins].duration_s : 0;
    if (ins_camera_eval(p->inst, ins, ph, &hand, &tgt)) game_bomb_hold(p->bomb, hand, p->inst->quat);   /* no track: it stays where it was */
    }
    switch (p->bsub) {                                                      /* jump table 0x463c28 */
    case 0: p->bt = anim_len(p, 0x45, 0); lock_move(p, p->bt); p->bsub = 1; /* fallthrough */
    case 1: if ((p->bt -= dt) <= 0) p->bsub = 2; break;
    case 2: if (p->carry_pressed && !p->duck) p->bsub = p->on_ground ? 3 : 5; break;   /* 0x463963: no throw while ducking */
    case 3: p->bt = anim_len(p, 0x43, 0); lock_move(p, p->bt); p->bsub = 4; break;
    case 5: p->bt = anim_len(p, 0x44, 0); p->bsub = 6; break;
    case 4: case 6: if ((p->bt -= dt) <= 0) { bomb_drop(p); if (wenv("WOODY_BOMBLOG")) puts("  BOMB throw over: state 0"); } break;   /* no release moment reached (landed during an air throw, or an empty throw): SetState(0) drops it */
    }
    p->carry_pressed = 0;
}
static void attack_trigger(Player *p, const PlayerInput *in, float dt)
{
    int held = in->action, pressed = held && !p->action_prev, released = !held && p->action_prev;
    p->action_prev = held;
    if (p->look) return;                                                   /* state 3: 0x44ba70 picks up only in state 0, 0x457330 wants state 0 */
    if (p->state6) { if (pressed) p->carry_pressed = 1; return; }         /* state 6: 0x457330 wants state 0 */
    if (pressed && p->on_ground && !p->atk) {             /* 0x44bae1 -> 0x463430 before the trigger: the same press picks up a bomb within 69 + 200 */
        struct Bomb *b = game_bomb_pick(p->pos, 69.0f + 200.0f);
        if (b) { p->bomb = b; p->carrying = 1; p->state6 = 1; p->bsub = 0; p->charge = 0; p->carry_pressed = 0; p->throw_hold = 0; return; }
    }
    if (p->jumper.state != 2 && p->jumper.state != 6) p->charge = 0;       /* no charge in the air */
    if (p->air_win > 0) p->air_win -= dt;
    if (p->duck) return;                                                   /* 0x457388: no attack while ducking */
    if (p->atk == 5 && pressed && p->air_win > 0) { p->atk = 1; return; }  /* chained attack out of the recoil */
    if (p->move_lock > 0 || p->atk != 0) return;
    if (p->on_ground) {
        if (released) {                                                    /* charge run starts on RELEASE */
            p->atk_t = anim_len(p, 0x10, 0) + anim_len(p, 0x11, 0); p->atk = 9; p->atk9_first = 1;   /* 0x457499: +0x5fd */
            if (p->charge <= 0.1f) { lock_move(p, p->atk_t); p->charge = 0; }
        } else if (held) { p->charge += 4.0f * dt; if (p->charge > 1.5f) p->charge = 1.5f; }
    } else if (pressed && p->air_win > 0) p->atk = 1;                      /* air: peck dash */
}

/* Perso state (+0x21c), set by SetState 0x44c980 and read by 0x44bcf0: 0 means the player has his own controls.
 * The port keeps that state in the fields that stand for it - 2 = dead, 4 = hanging in a peckable wall, 5 = a
 * scripted action, 8 = riding a class-20 rocket, 6 = state6, 3 = look - so the messages that only answer "when the Perso is free"
 * (1042, and 0x465740 when he steps onto a rocket) ask here. */
int player_state_free(const Player *p) { return !p->dead_kind && !p->climb_sub && !p->use_root && !p->script_act && !p->ride && !p->state6 && !p->look && !p->follow; }

/* 0x458e40 Perso_BrakeCharge, the one brake that is called from outside the attack controller: the game code calls it
 * at 0x44542f, in the handler of message 1042, when the player is standing at a peck switch. Releasing the attack
 * button started the charge run (atk 9) in the Perso update of this very frame; the VM tick that follows brakes it
 * again, so what the player sees at the switch is the peck animation 0x12 in place and not a run into it.
 * Only the two charge states are braked - the dash and the rebounds are left alone - and the brake is the same one
 * the controller uses internally (0x457b0a / 0x457e16, docs/PERSO_JUMP.md 2.3). */
void player_brake_charge(Player *p)
{
    if (p->atk != 9 && p->atk != 10) return;
    p->charge = 0;
    p->atk_t = anim_len(p, 0x12, 0) + anim_len(p, 0x12, 1);
    lock_move(p, p->atk_t);                                                /* LockMove(T, 0) */
    p->atk = 11;                                                           /* atk_anim[11] = 0x12 plays the peck */
}

/* ---- damage, death, respawn (docs/PERSO_MOVE.md 4.4, PERSO_FRAME.md 4.1) ------------------------------------ */
void player_kill(Player *p, int kind)                                   /* vt[38] Kill(kind) 0x44c110 */
{
    if (boss15_protects(p->enemies)) return;                             /* an actor of category 2 / subtype 12 whose vtbl[36] holds: Boss2 beaten (docs/BOSS15_16.md) */
    if (p->race_char) {                                                  /* subtypes 4/5: the race kill 0x44c4c0 (docs/RACE.md 4.9) */
        if (p->dead_kind && !(kind == 7 && p->dead_kind != 7)) return;   /* only water overrides a running death */
        if (kind == 2 && p->invuln_respawn > 0) return;                   /* no invulnerability test for the other kinds */
        p->death_delay = kind == 2 || kind == 8 ? 1.5f : kind == 3 || kind == 6 ? 2.5f : kind == 7 ? 4.0f : 3.5f;
        p->cam_dist = 200.0f; race_snd_stop(p);                           /* 0x44c508, 0x468e20 */
        if (kind == 7) { jumper_reset(&p->jumper); p->att_inst = NULL; game_splash((Vec3){ p->pos.x, p->pos.y + 110.0f, p->pos.z }, sqrtf(vdot(p->vel, p->vel)), 50.0f); }   /* 0x44c65e */
        else if (kind == 1) { jumper_force_fall(&p->jumper, 0); game_bubble(p->inst, 0, 2.5f, 180.0f, 50.0f, NULL); }
        p->nograv_t = kind == 1 ? anim_len(p, 0x75, 0) : kind == 2 ? anim_len(p, 0x72, 0) : 0;   /* +0x240 */
        p->dead_T = 0; p->dead_cam_req = 0; p->hit_anim_t = 0; p->script_act = 0; p->ride = NULL; p->race_crouch = 0;
        p->atk = 0; p->charge = 0; p->health = 0; p->dead_kind = kind;  /* +0x4d8 = 1, state := 2 */
        if (kind == 2) game_skeleton();                                  /* 0x44c59d: the skeleton flash 0x477e40 */
        printf("  PLAYER killed in the race (kind %d), lives %d\n", kind, p->lives);
        return;
    }
    if (kind == 2 || kind == 9) rumble(p, 0.5f, 1.5f);                  /* 0x44c3ab: Perso +0x1e4, +0x1e0 = P+0xd4, P+0xd0, before the immunity tests */
    if ((kind == 2 || kind == 6 || kind == 9) && game_soft_hazards() && !p->dead_kind) {   /* PORT EXTRA: a laser, lightning or blast costs a heart */
        if (p->invuln_respawn > 0 || p->soft_t > 0) return;
        p->soft_t = 1.0f; p->invuln_hit = 0;                             /* the blasts' own 0-damage Hit just before must not refuse this one */
        if (kind == 6) { p->ride = NULL; bomb_drop(p); }                 /* as the Kill: off the rocket, the bomb in his hands went off */
        if (!player_hit(p, 1.0f, (Vec3){ 0, 0, 0 })) return;
        kind = 3;                                                        /* the last heart: the ordinary death */
    }
    if (p->dead_kind) { if (!((kind == 7 && p->dead_kind != 7) || (kind == 1 && p->dead_kind != 1))) return; }
    if ((kind == 2 || kind == 9 || kind == 3 || kind == 8 || kind == 4 || kind == 5 || kind == 6) && p->invuln_respawn > 0) return;
    p->death_delay = 3.5f;                                              /* +0x288: time until the fade */
    if (kind == 2 || kind == 9) p->death_delay = 1.5f; else if (kind == 3 || kind == 8) p->death_delay = 3.0f;
    else if (kind == 6) p->death_delay = 2.5f; else if (kind == 7) p->death_delay = 0.0f;
    if (kind == 7) { jumper_reset(&p->jumper); p->att_inst = NULL; game_splash((Vec3){ p->pos.x, p->pos.y + 110.0f, p->pos.z }, sqrtf(vdot(p->vel, p->vel)), 50.0f); }   /* 0x44c33e: the splash, speed = this frame's displacement / dt (0x44d170) */
    else if (kind != 2 && kind != 9) jumper_force_fall(&p->jumper, 0);
    p->nograv_t = kind == 1 ? anim_len(p, 0x2f, 0) : (kind == 2 || kind == 9) ? anim_len(p, 0x30, 0) : 0;   /* +0x240: no fall while he hangs / is zapped */
    p->dead_T = 0; p->dead_cam_req = 0; p->hit_anim_t = 0; p->script_act = 0; p->ride = NULL; bomb_drop(p);   /* SetState(2) 0x44c9ad lets go of a bomb */
    p->look = 0;                                                        /* out of state 3 without +0x268: he stays faded out until Reset (docs/PERSO_LOOK.md 4) */
    p->atk = 0; p->charge = 0; p->health = 0; p->dead_kind = kind;      /* state := 2 */
    if (kind == 1) game_bubble(p->inst, 0, 2.5f, 180.0f, 50.0f, NULL);  /* 0x44c2a9: "?!" over him as he drops into the pit */
    if (kind == 2 || kind == 9) game_skeleton();                         /* 0x44c41f: the skeleton flash 0x477e40 (docs/PARTICLES.md 6) */
    printf("  PLAYER killed (kind %d), lives %d\n", kind, p->lives);
}
int player_hit(Player *p, float damage, Vec3 dir)                       /* vt[39] Hit 0x44ca00: returns 1 when health ran out */
{
    if (p->dead_kind || p->invuln_respawn > 0 || p->invuln_hit > 0 || wenv("WOODY_GOD")) return 0;   /* WOODY_GOD: testing */
    rumble(p, 0.5f, 0.5f);                                               /* P+0xbc, P+0xb8 */
    jumper_force_fall(&p->jumper, 0);
    /* knockback 0x45a140: RampC to 500 u/s (0.1 s up), held 0.2 s, 0.5 s out; the player turns to face the attacker */
    float l = sqrtf(dir.x * dir.x + dir.z * dir.z);
    if (p->push_t <= 0) { p->push_dir = l > 0.01f ? (Vec3){ dir.x / l, 0, dir.z / l } : (Vec3){ 0, 0, 0 }; p->push_t = 0.2f; p->push_speed = 0; }
    if (l > 0.01f) p->yaw = atan2f(-dir.x, -dir.z);
    if (p->invuln_hit < 0.6f) p->invuln_hit = 0.6f;
    p->move_lock = 0; p->atk = 0;
    p->hit_anim = p->on_ground ? (p->duck ? 0x23 : 0x1f) : 0x20; p->hit_anim_t = anim_len(p, p->hit_anim, 0); p->lanim = -1;   /* 0x464b70: priority 5110, plays out over walking / jumping; 0x23 lying down (he stays down) */
    if (p->state6 && p->bomb) { p->hit_anim = p->on_ground ? (p->duck ? 0x24 : 0x21) : 0x22; p->hit_anim_t = anim_len(p, p->hit_anim, 0); p->bsub = 2; }   /* with a bomb: he keeps it, a throw or pick-up is broken off */
    else if (p->state6) p->hit_anim_t = 0;                               /* 0x464b84: state 6 with empty hands (the look-around bug) requests no hit animation */
    if (wenv("WOODY_ONEHIT")) damage = 99;                             /* testing: every hit kills */
    p->health -= damage; if (p->health < 0) p->health = 0;
    printf("  PLAYER hit, health %.0f\n", p->health);
    if (p->health <= 0) { p->health = 0; return 1; }
    return 0;
}
static void perso_reset(Player *p)                                      /* vt[17] Reset 0x44ab20 (no position: 0x44a810 / the cinematic end set it) */
{
    p->yaw = p->spawn_yaw; p->floor_y = p->pos.y;                        /* 0x44ab90: 0x459ff0(M, +0x324) = the checkpoint facing */
    jumper_reset(&p->jumper); p->on_ground = 1; p->invuln_respawn = 1.0f; p->invuln_hit = 0; p->move_lock = 0;
    if (p->health <= 0) p->health = 3.0f;
    bomb_drop(p); p->throw_hold = 0;                                     /* 0x44acb4 */
    p->ride = NULL; p->dead_kind = 0; p->dead_T = 0; p->nograv_t = 0; p->hit_anim_t = 0; p->script_act = 0; p->atk = 0; p->charge = 0; p->speed = 0; p->ramp_phase = 0; slide_reset(p); p->push_t = 0; p->push_speed = 0;
    p->att_inst = NULL; p->lanim = -1; p->board_lanim = -1; p->step_u = -1.0f; p->cam_init = 0; idle_reset(p);   /* 0x44abab A/B->Reset(), 0x44abcf */
    p->bonus_inv = p->bonus_inv_acc = 0; p->bonus_inv_cnt = 0; p->inst->tint_mode = 0;   /* 0x44ad46..0x44ad58: +0x704 +0x700 +0x708 +0x70c = 0, no more white blinking */
    p->duck = 0; p->duck_t = 0;                                          /* 0x44ad28 */
    p->crush = 1.0f;                                                     /* 0x44ab3b: +0x2e8 = 1 (and the scale +0x4c..+0x54) */
    p->look = 0; p->look_show = 0; p->inst->fade = p->inst->fade_target = 0; p->inst->fade_rate = 100.0f;   /* 0x44ac15 +0x268 = 0; 0x44ad6a: 0x44e7f0(0, 1), visible again after a death in state 3 */
    p->special_st = 0; p->special_t = 0;                                 /* 0x44ad5e / 0x44ad64 */
    p->am_win = 0; p->am_used = 0; p->am_on = 0;                         /* 0x44ad34 / 0x44ad3a / 0x44ad40 */
    p->follow = NULL;                                                    /* 0x44ac84: +0x55c = 0; Reset's SetState(0) (0x44ac3f) ends state 7 */
    if (p->race_char) race_enter(p, 1);                                /* 0x44ac33: SurfEnter + state 1 */
}
static void player_reset(Player *p)                                     /* respawn 0x44a810(0) in 0x445930: +0x318, Reset, ground snap */
{
    p->pos = p->spawn_pos; perso_reset(p);
    player_ground_snap(p);                                              /* 0x44a810 -> 0x462990 */
    /* 0x445930 -> 0x44a810 -> Reset 0x44ab20 clears Perso+0x4ec (0x44ad22): the side view's plane lock ends with the death,
     * and 0x445930 then calls 0x458f90 (hard cut back to the follow camera). Both live in the app (g_cam). */
    p->respawn_req = 1;
}
/* end of a real time cinematic, Game update 0x445af9..0x445b66 (docs/CINEMATIC.md 5): Perso vtbl[0x44] = Reset 0x44ab20
 * (0x445b23: state 0, or SurfEnter + state 1 for the race riders with the up filter back to (0,1,0); anim controllers reset,
 * 1 s invulnerability, carried bomb dropped, ...), then the facing 0x459ff0 and 0x44a650 = position + ground snap. The app
 * does the rest of Reset (plane lock off, every bomb in flight discarded 0x44db10) and the hard cut to the follow camera.
 * have = 0: the port could not evaluate the root end, he stays where he is (the original always has Game+0x198). */
void player_cin_end(Player *p, int have, Vec3 pos, float yaw)
{
    perso_reset(p);
    if (have) { p->yaw = yaw; p->pos = pos; }
    p->vel = (Vec3){ 0, 0, 0 }; p->speed = 0; p->ramp_phase = 0; p->lanim = -1; p->step_u = -1.0f; p->board_lanim = -1;   /* 0x44a650: A->Reset, B->Reset (as player_teleport) */
    player_ground_snap(p);                                              /* 0x44a678 */
}
/* The iris Game+4: 0x4776b0(from, to, dur) sets it, 0x477920(dt) advances it, draws it (0x4776d0, hud_iris: a black ring
 * around the screen centre with the inner radius iris * 0.99 * 480) and says whether it has arrived. */
static void iris_set(Player *p, float from, float to, float dur) { p->iris_from = from; p->iris_to = to; p->iris_dur = dur; p->iris_t = 0; }
static int iris_tick(Player *p, float dt)
{
    p->iris_t += dt; float f = p->iris_t / p->iris_dur; if (f > 1.0f) f = 1.0f;
    p->iris = p->iris_from - (p->iris_from - p->iris_to) * f; p->iris_on = 1;
    return f >= 1.0f;
}
/* Game sequence 0x4459c0 (docs/PERSO_FRAME.md 4.1): 1 iris opens (the Game ctor starts here, 0 -> 1 in 1 s) -> 2 play ->
 * (dead) 3 wait death_delay - 1 s -> 4 iris closes 1 -> 0 in 1 s -> lose a life -> 0 black for 0.25 s, respawn -> 1.
 * It is not a brightness fade: the picture stays at full brightness inside a shrinking circle round the screen centre,
 * which is where the follow camera keeps Woody. Ticked by the frame (step 33) whenever the world is not paused,
 * cinematics included; the iris is drawn only in the states 0, 1 and 4. */
void player_game_tick(Player *p, EkoVM *vm, float dt)
{
    p->iris_on = 0;
    if (dt <= 0) return;
    switch (p->game_state) {
    case 0: iris_tick(p, dt); p->game_t -= dt;
            if (p->game_t <= 0) {                                        /* 0x445930 -> Perso respawn 0x44a810, then 0x445a44 */
                if (p->lives == 0) { player_ground_snap(p); p->gameover_req = 1; }   /* 0x44a82b: no life left -> 0x44a8f5 0x404e10 = page 0x1d, no checkpoint, no Reset; only the snap 0x44a900 */
                else player_reset(p);
                iris_set(p, 0, 0, 0.1f); iris_set(p, 0, 1.0f, 1.0f); p->game_state = 1; }
            break;
    case 1: if (iris_tick(p, dt)) p->game_state = 2; break;
    case 2: if (p->dead_kind) { p->game_state = 3; p->game_t = 0; } break;
    case 3: p->game_t += dt; if (p->game_t >= p->death_delay - 1.0f) { iris_set(p, 1.0f, 0, 1.0f); p->game_state = 4; } break;   /* 0x445ac1 */
    case 4: if (iris_tick(p, dt)) {
                p->lives--;                                              /* 0x44c730: life lost (no clamp: 0 -> game over in 0x44a810), leave all volumes, msgmask 0x10 pulse */
                player_leave_all(p, vm);
                if (vm) { eko_msgmask_set(vm, p->inst->id, 0x10); p->mask10_frames = 2; }
                p->game_state = 0; p->game_t = 0.25f; } break;
    }
    if (p->mask10_frames > 0 && --p->mask10_frames == 0 && vm) eko_msgmask_clear(vm, p->inst->id, 0x10);
}
/* Pause menu "Start again" (page 0x19 = the pause menu while riding, result 18, 0x40584d): 0x445930 = the respawn step of the Game
 * sequence (state 0 with 0.1 s to go, the iris shut, Perso respawn 0x44a810(0) at the checkpoint), then the race restart 0x4560f0:
 * no checkpoint any more (+0x330 = 0, +0x4e0 = 0), respawn position = the start (+0x318 = +0x30c), respawn again. State 0 then
 * respawns once more after 0.1 s and opens the iris 0 -> 1 in 1 s (docs/PERSO_FRAME.md 4.1), as after a death but no life is lost.
 * The race bonuses come back through Reset -> SurfEnter (race_enter: 0x44f8a0, +0x264 = +0x4e0 = 0). "Reset all actors" 0x40c040
 * (vtbl[28] of every Npc but the Perso) is a no-op in the shipped game: slot 28 is 0x445840 = a bare `ret` in the Npc, Enemy,
 * Perso and all nine enemy/boss class vtables, so neither this restart nor a death respawn resets an enemy. [0x4b3354] = 0.2 has no reader. */
void player_restart(Player *p)
{
    iris_set(p, 0, 0, 0.1f); p->iris = 0; p->iris_t = 0; p->game_state = 0; p->game_t = 0.1f;   /* 0x445930: Fader(Game+4, 0, 0, 0.1), state 0, timer 0.1 */
    player_reset(p);                                                                          /* 0x44a810(0) */
    p->has_ckpt = 0; p->race_bonus_ckpt = 0; p->spawn_pos = p->start_pos; player_reset(p);  /* 0x4560f0: +0x330 = 0, +0x4e0 = 0, +0x318 = +0x30c, 0x44a810(0) */
    if (p->board) { p->board_lanim = -1; board_request(p, 0); }                               /* B->Reset(), B->Request(0): the board idles instead of the start anim 0x5d */
}

/* ---- peck climbing: Perso state 4 (0x4651d0). A press node (kind 1) with typecode 4 is a peckable wall. ---- */
static float g_climb_frac;   /* fraction of the last climb_ray hit */
/* The instance half of the ray 0x4359b0 (docs/PERSO_MOVE.md 6.6-6.7): 0x497ed0 runs vt[5] 0x432ab0 for the instances
 * registered in the cells the world part visited - a's cell (0x408180) along a->b to the cell of the world hit t_world, or
 * of b (gel_walk_seg) - and then for the dynamic list (gel_col_instances). Per press node, per polygon, with the loader
 * plane (world-space loader_plane, the same side as the node-space one): f(a) < 0 -> counted as behind and skipped
 * (0x432d8c); f(b) > 0 -> skipped (0x432dc3); every edge (v_k - a) x (v_k+1 - a) . (b - a) > 0 (0x432ee2), else skipped;
 * t = f(a) / (f(a) - f(b)). The polygon is recorded when t < [0x4c4bd4] OR [0x53a554] == 0 (0x432f7a..0x432f8e) - and only
 * the "inside" branch below ever writes [0x53a554], which 0x4359b0 zeroes before the cast: so EVERY such polygon is
 * recorded and the LAST one tested wins (last instance in cell order, last node, last polygon), not the nearest, and it
 * replaces a world hit even when that is nearer. A node whose polygons all have f(a) < 0 (0x4330c0, count == N+4): a lies
 * inside it -> [0x53a554] = 3, t = 0, instance and node recorded ([0x4c4bd0] = 2 -> hit kind 3); from then on no polygon
 * can win (t < 0 is impossible, [0x53a554] != 0), but a later inside node overwrites the instance again (last inside wins).
 * The normal of a kind-2 hit is M.n normalised (0x432f94..0x433054, press_normal), which faces a. Returns 0, 2 or 3.
 * `skip` (a bomb itself: flag 0x40 during its own ray, 0x449da2) and the player's own instance take no part. */
static int ray_instances(const Player *p, const Instance *skip, Vec3 a, Vec3 b, float t_world, InsRayHit *h)
{
    const GelFile *g = p->gel; const InsFile *ins = p->ins; Vec3 v[32], ab = vsub(b, a); int inside = 0;
    h->kind = 0; h->t = 2.0f; h->in = NULL; h->node = NULL; h->n = (Vec3){ 0, 1, 0 };
    if (!ins) return 0;
    gel_walk_seg(g, a, b, t_world, 0);
    const GelColRef *cr; uint32_t ncr = gel_col_instances(g, ins, &cr);
    float sb[6] = { fminf(a.x, b.x) - 1, fmaxf(a.x, b.x) + 1, fminf(a.y, b.y) - 1, fmaxf(a.y, b.y) + 1, fminf(a.z, b.z) - 1, fmaxf(a.z, b.z) + 1 };
    for (uint32_t q = 0; q < ncr; q++) {
        const Instance *in = cr[q].in; if ((skip && in == skip) || skip_inst(in, p->inst)) continue;
        const Model *m = in->model; uint32_t ncn; const uint32_t *cn = ins_collision_nodes(m, &ncn);
        int uniform = in->scale.x == in->scale.y && in->scale.x == in->scale.z;   /* 0x432b9d / 0x432bb7 */
        for (uint32_t ci = 0; ci < ncn; ci++) {
            uint32_t ni = cn[ci]; const InsNode *nd = &m->nodes[ni];
            if (uniform) {                                                  /* 0x432bc5..0x432c4a: skip the node (its inside test too) when */
                const float *M = in->node_world[ni].m; float R = in->scale.x * nd->radius;   /* |o - a|^2 > R^2 + |b - a|^2, o = the node origin (pivot) */
                Vec3 o = { M[12] - a.x, M[13] - a.y, M[14] - a.z };      /* NOT a segment-sphere test: a short ray that reaches into a big */
                if (R * R + vdot(ab, ab) < vdot(o, o)) continue;           /* node far from its origin is culled (the W1A stamper 186, 993) */
            }
            float nb[6]; if (ins_node_world_box(in, ni, nb) && (nb[0] > sb[1] || nb[1] < sb[0] || nb[2] > sb[3] || nb[3] < sb[2] || nb[4] > sb[5] || nb[5] < sb[4])) continue;
            uint32_t behind = 0;
            for (uint32_t pi = 0; pi < nd->npolys; pi++) {
                const InsPoly *pl = &nd->polys[pi]; if (pl->nverts < 3 || pl->nverts > 32) continue;
                for (uint32_t c = 0; c < pl->nverts; c++) v[c] = ins_point_world(in, pl->indices[c]);
                float P[4]; if (!loader_plane(v, pl->nverts, P)) continue;
                float fa = P[0] * a.x + P[1] * a.y + P[2] * a.z + P[3], fb = P[0] * b.x + P[1] * b.y + P[2] * b.z + P[3];
                if (fa < 0) { behind++; continue; }                         /* 0x432d8c */
                if (fb > 0 || !(fa - fb > 0)) continue;                    /* 0x432dc3 */
                uint32_t e = 0;
                for (; e < pl->nverts; e++) { Vec3 u = vsub(v[e], a), w = vsub(v[(e + 1) % pl->nverts], a); if (!(vdot(vcross(u, w), ab) > 0)) break; }
                if (e < pl->nverts || inside) continue;                    /* after an inside node t < 0 would be needed */
                h->kind = 2; h->t = fa / (fa - fb); h->in = in; h->node = nd;   /* no "t < best": the last one wins */
                if (!press_normal(in, pl, &h->n)) h->n = (Vec3){ P[0], P[1], P[2] };
            }
            if (nd->npolys && behind == nd->npolys) { inside = 1; h->kind = 3; h->t = 0; h->in = in; h->node = nd; }   /* 0x4330c8 */
        }
    }
    if (wenv("WOODY_CELLCHECK")) {                                     /* testing: the same with every instance (the old set, model order) */
        static int depth; if (!depth) { depth = 1; InsRayHit h2; gel_col_force_all(1); ray_instances(p, skip, a, b, t_world, &h2); gel_col_force_all(0); depth = 0;
            if (h2.kind != h->kind || h2.in != h->in || fabsf(h2.t - h->t) > 0.001f)
                printf("CELLCHECK ray %.0f %.0f %.0f -> %.0f %.0f %.0f: cells kind %d inst %d t %.3f | all kind %d inst %d t %.3f", a.x, a.y, a.z, b.x, b.y, b.z, h->kind, h->in ? (int)h->in->index : -1, h->t, h2.kind, h2.in ? (int)h2.in->index : -1, h2.t), puts(""); }
    }
    return h->kind;
}
/* the instance half for callers with a world test of their own: the cells up to the one-sided world hit (0x497fb0's walk),
 * the original's selection (above). *frac = the fraction of a->b (0 for a start inside a press node, whose normal the
 * original leaves stale - here the reverse ray direction). The original lets any instance hit replace the world hit. */
int player_ray_instances(const Player *p, const Instance *skip, Vec3 a, Vec3 b, float *frac, Vec3 *n_out, const Instance **inst_out)
{
    float tw = gel_ray_front(p->gel, a, b); InsRayHit h;
    int k = ray_instances(p, skip, a, b, tw < 1.0f ? tw : 2.0f, &h);
    *frac = k ? h.t : 2.0f; if (!k) return 0;
    if (k == 3) { Vec3 d = vsub(a, b); float l = sqrtf(vdot(d, d)); h.n = l > 1e-6f ? (Vec3){ d.x / l, d.y / l, d.z / l } : (Vec3){ 0, 1, 0 }; }
    *n_out = h.n; if (inst_out) *inst_out = h.in;
    return 1;
}
/* the probe ray of the peck climb (0x4575b0 for the grab, 0x46545e while climbing): 0x4359b0, of which only an instance hit
 * matters here - hit kind 2 on a press node with type code 4 is peckable ([0x53a58c] -> node type code); kind 3 (the start
 * inside a press node) is a hit that is not peckable. Instances as the original picks them (ray_instances: the cells up to
 * the world hit, the last polygon tested wins); the normal faces the player (one-sided). */
static int climb_ray(const Player *p, Vec3 from, Vec3 to, Vec3 *n_out, const Instance **inst_out, int *peckable)
{
    float tw = gel_ray_front(p->gel, from, to); InsRayHit h;
    if (!ray_instances(p, NULL, from, to, tw < 1.0f ? tw : 2.0f, &h)) return 0;
    g_climb_frac = h.t; *n_out = h.n; *inst_out = h.in; *peckable = h.kind == 2 && h.node->type_code == 4;
    if (h.kind == 3) { Vec3 d = vsub(from, to); float l = sqrtf(vdot(d, d)); *n_out = l > 1e-6f ? (Vec3){ d.x / l, d.y / l, d.z / l } : (Vec3){ 0, 1, 0 }; }
    return 1;
}
static Vec3 climb_probe_to(const Player *p, Vec3 from) { Vec3 to = { from.x + sinf(p->yaw) * (P_RADIUS + 100.0f), from.y, from.z + cosf(p->yaw) * (P_RADIUS + 100.0f) }; return to; }

/* 0x464ef0, the grab of a fresh press (before the attack controller 0x457a50 in the Perso frame, so on the ground and in the
 * air alike): attack JUST pressed, attack substate 0 or 10 (the charge run), not climbing yet; the ray of 0x4359b0 from the
 * feet + 40 along the facing over P+4 + 100 = 169 must end on a peckable press node that 0x464e00 accepts. Then SetState(4)
 * with +0x50c = 1 (anim 0x14 first, both from the ground and from the air), grip 0.8, Mover_SetDir(NULL), jumper reset,
 * attack window closed (0x457560(0, 1)), facing the wall. No spark here (only the dash probe 0x4575b0 sparks). The grab out
 * of a running dash is attack_probe's (substate 8). */
static int climb_try(Player *p, const PlayerInput *in)
{
    if (!(in->action && !p->action_prev) || (p->atk != 0 && p->atk != 10) || p->climb_sub || p->look || p->state6) return 0;
    Vec3 from = { p->pos.x, p->pos.y + 40.0f, p->pos.z }, to = climb_probe_to(p, from), n; const Instance *wi; int peck = 0;
    if (!climb_ray(p, from, to, &n, &wi, &peck) || !climb_grab(p, n, peck ? wi : NULL)) return 0;
    p->yaw = atan2f(-p->wall_n.x, -p->wall_n.z);
    p->climb_sub = 1; p->grip = 0.8f; jumper_reset(&p->jumper);         /* 0x4650af..0x4650ce */
    p->air_win = 0;                                                       /* 0x4650d9: 0x457560(0, 1) */
    p->atk = 0; p->charge = 0; p->speed = 0; p->ramp_phase = 0; p->vel = (Vec3){ 0, 0, 0 }; p->use_atk_disp = 0;   /* SetState clears +0x5b4 */
    printf("  CLIMB grab on instance %u\n", wi->index);
    return 1;
}
/* Perso state 4, 0x4651d0 (docs/OBJECTS.md 1.3). Returns this frame's displacement +0x204: the Perso frame clears it
 * (0x44b662) and only sub 2 writes it; the caller then runs Perso_MoveCollide 0x4624f0 with it, as the original does for
 * state 4 (0x44b834 -> 0x44b857), so the climber collides with the world and the instances like a walker: the wall he
 * climbs stops the 200 u/s press at the body radius, a side wall stops the sideways climb. */
static Vec3 climb_update(Player *p, const PlayerInput *in, float dt)
{
    Vec3 disp = { 0, 0, 0 };
    int tap = in->action && !p->climb_act_prev; p->climb_act_prev = in->action;
    if (wenv("WOODY_TAP")) { static float tt; tt += dt; if (tt > 0.3f) { tt = 0; tap = 1; } }   /* testing: tap the attack key automatically */
    if (p->grip > 0) p->grip -= dt;
    if (tap && p->grip < 0.5f) p->grip = 0.5f;                          /* tapping keeps him on the wall */
    if (p->grip <= 0 && p->climb_sub != 3) p->climb_sub = 4;
    switch (p->climb_sub) {
    case 1: anim_request(p, 0x14, 1.0f); p->climb_sub = 2; p->peck_t = 0.3f; break;
    case 2: {
        anim_request(p, 0x15, 1.0f);
        Vec3 side = { p->wall_n.z, 0, -p->wall_n.x };                   /* cross((0,1,0), n) */
        /* sideways with left / right (P+0x78), but not in the side view: 0x465387 skips it while Perso+0x4ec is set (left / right is
         * the walking axis there, and the side step would leave the plane, which only Perso_Move clamps) */
        float s = 300.0f * dt, a = p->side_on ? 0 : in->left ? -s : in->right ? s : 0;
        disp.y = 250.0f * dt;                                           /* always upwards, no input needed (P+0x74) */
        disp.x = side.x * a - p->wall_n.x * dt * 200.0f;                /* 0x4aa164: pressed against the wall */
        disp.z = side.z * a - p->wall_n.z * dt * 200.0f;
        /* the ray starts at the position of this frame, before the collision moves him (0x46545e) */
        Vec3 from = { p->pos.x, p->pos.y + 40.0f, p->pos.z }, to = climb_probe_to(p, from), n; const Instance *wi; int peck = 0;
        if (climb_ray(p, from, to, &n, &wi, &peck)) {
            if (!peck) { p->climb_sub = 4; break; }                     /* 0x465549: this frame still moves; the let-go runs next frame */
            if ((p->peck_t -= dt) <= 0) {                               /* a peck every 0.3 s: 0x479c80(0, hit point, wall normal) */
                float f = g_climb_frac * 0.95f;                         /* 0x4a9c9c: just short of the wall, so the mark sits on it */
                p->peck_t += 0.3f;
                game_peck_fx(0, (Vec3){ from.x + (to.x - from.x) * f, from.y + (to.y - from.y) * f, from.z + (to.z - from.z) * f }, &n);
            }
        } else {
            /* nothing in front any more: the top, when within 50 of the wall instance's typecode-0 marker */
            const Model *wm = p->wall_inst->model; int top = 0; float ty = 0;
            for (uint32_t i = 0; i < wm->nnodes && !top; i++) if (wm->nodes[i].kind == 0x20 && wm->nodes[i].type_code == 0 && wm->nodes[i].npoints >= 1) { ty = ins_point_world(p->wall_inst, wm->nodes[i].point_base).y; top = 1; }
            if (top && fabsf(p->pos.y - ty) < 50.0f) {
                p->climb_sub = 3; p->over_len = p->over_t = anim_len(p, 0x17, 0) > 0.05f ? anim_len(p, 0x17, 0) : 0.5f; anim_request(p, 0x17, 1.0f);
                p->use_root = 1; p->root_pos = p->pos;
            } else p->climb_sub = 4;                                    /* 0x465510 / 0x465602: also on a world hit */
        }
        break; }
    case 3: {
        anim_request(p, 0x17, 1.0f);
        /* Perso_RootMotion 0x44e290 (docs/OBJECTS.md 1.5): pos stays frozen while the root track of .ins anim 15 carries the model over
         * the edge; the position getter returns (1-f) pos + f q(f); on the last frame pos = the root's end relative to idle (anim 1),
         * facing -E.row2, then SnapToGround 0x462990 (floor under feet + 43) */
        p->over_t -= dt; float f = 1.0f - (p->over_t > 0 ? p->over_t / p->over_len : 0); Vec3 q, fw;
        if (ins_root_at(p->inst, 15, f, 1, &q, &fw)) p->root_pos = (Vec3){ p->pos.x + (q.x - p->pos.x) * f, p->pos.y + (q.y - p->pos.y) * f, p->pos.z + (q.z - p->pos.z) * f };
        if (p->over_t <= 0) {
            if (ins_root_at(p->inst, 15, 1.0f, 1, &q, &fw)) { p->pos = q; if (fw.x * fw.x + fw.z * fw.z > 1e-6f) p->yaw = atan2f(fw.x, fw.z); }
            p->use_root = 0; p->climb_sub = 0; player_ground_snap(p); p->lanim = -1; anim_request(p, 0, 1.0f); printf("  CLIMB over the top at %.0f %.0f %.0f\n", p->pos.x, p->pos.y, p->pos.z);
        }
        break; }
    default:                                                            /* 4: let go; no new grab for twice the fall animation */
        p->use_root = 0; p->regrab = 2.0f * anim_len(p, 0x16, 0); anim_request(p, 0x16, 1.0f);
        p->climb_sub = 0; jumper_force_fall(&p->jumper, 1); printf("  CLIMB let go\n");
        break;
    }
    return disp;
}

/* ---- bonuses (docs/BONUS.md): the script sends message 10 to the bonus instance; vtable[+0x70] per class ------ */
int player_collect(Player *p, int type, int arg)
{
    switch (type) {
    case 30: p->lives++; audio_fx(0, NULL, NULL); break;                  /* extra life, SoundFx 0 (0x44f43b) */
    case 34: p->bonus_got++; p->bonus_count++; audio_fx(3, NULL, NULL); break;                     /* "Bonus Woody", sound 3; 25 = a heart or a life */
    case 35: p->special_charges++; audio_fx(2, NULL, NULL); break;                                 /* charge for the special action (Perso+0x254), sound 2 */
    case 36: p->unique_items++; audio_fx(1, NULL, NULL); break;                                    /* unique item (save game flag), sound 1 */
    case 37: p->race_bonus++; audio_fx(5, NULL, NULL); break;                                      /* "Bonus Race", sound 5 */
    case 38: { float t = arg * 0.01f; p->bonus_inv = t; if (p->invuln_respawn < t) p->invuln_respawn = t; if (p->invuln_hit < t) p->invuln_hit = t; } break;   /* invulnerability 0x44c890: +0x704 = +0x700 = t, +0x270 / +0x280 = max */
    default: return 0;
    }
    /* reward in Perso_Update 0x44b530: 25 bonuses = one heart, or an extra life when already at 5 hearts */
    if (p->bonus_count >= 25) { p->bonus_count -= 25; audio_fx(4, NULL, NULL); if (p->health < 5.0f) p->health += 1.0f; else { p->lives++; audio_fx(0, NULL, NULL); } }   /* 0x44b6e4, 0x44b71a */
    printf("  BONUS type %d collected: woody bonus %d / %d, health %.0f, lives %d\n", type, p->bonus_got, p->bonus_total, p->health, p->lives);
    return 1;
}

static void player_transform(Player *p, int tilt)
{
    Instance *in = p->inst;
    /* q = yaw(y) * rotx(-90): (0.7071 c, -0.7071 s, -0.7071 s, -0.7071 c) with c=cos(yaw/2), s=sin(yaw/2) */
    float c = cosf(p->yaw * 0.5f), s = sinf(p->yaw * 0.5f);
    in->position = p->pos;
    in->quat.x = 0.70710678f * c; in->quat.y = -0.70710678f * s; in->quat.z = -0.70710678f * s; in->quat.w = -0.70710678f * c;
    if (tilt && p->race_char && !p->dead_kind) {
        /* Orient 0x44bd30 in state 1: up = the filtered floor normal +0x210 instead of (0,1,0); F = -facing, right = F x up,
         * fwd = up x right, rows +0x28/+0x34/+0x40 = right/fwd/up = the images of model x/y/z (so rider and board lean with
         * the slope). The port normalises the axes and turns them into the instance quaternion (Shepperd). */
        Vec3 up = p->race_upf, F = { -sinf(p->yaw), 0, -cosf(p->yaw) };
        float lu = sqrtf(vdot(up, up));
        if (lu > 1e-4f && up.y > 0) {
            up = (Vec3){ up.x / lu, up.y / lu, up.z / lu };
            Vec3 R = vcross(F, up); float lr = sqrtf(vdot(R, R));
            if (lr > 1e-4f) {
                R = (Vec3){ R.x / lr, R.y / lr, R.z / lr }; Vec3 Fw = vcross(up, R);
                float m00 = R.x, m10 = R.y, m20 = R.z, m01 = Fw.x, m11 = Fw.y, m21 = Fw.z, m02 = up.x, m12 = up.y, m22 = up.z, tr = m00 + m11 + m22; Quat q;
                if (tr > 0) { float k = sqrtf(tr + 1.0f) * 2; q = (Quat){ (m21 - m12) / k, (m02 - m20) / k, (m10 - m01) / k, 0.25f * k }; }
                else if (m00 > m11 && m00 > m22) { float k = sqrtf(1.0f + m00 - m11 - m22) * 2; q = (Quat){ 0.25f * k, (m01 + m10) / k, (m02 + m20) / k, (m21 - m12) / k }; }
                else if (m11 > m22) { float k = sqrtf(1.0f + m11 - m00 - m22) * 2; q = (Quat){ (m01 + m10) / k, 0.25f * k, (m12 + m21) / k, (m02 - m20) / k }; }
                else { float k = sqrtf(1.0f + m22 - m00 - m11) * 2; q = (Quat){ (m02 + m20) / k, (m12 + m21) / k, 0.25f * k, (m10 - m01) / k }; }
                in->quat = q;
            }
        }
    }
    Vec3 sc = in->scale; if (p->crush > 0) sc.z *= p->crush;          /* 0x44bd00: inst z scale +0x54 = P+0x2e8 (crush test 0x462a40) */
    mat4_from_trs(&in->world, in->position, in->quat, sc);
}
static void player_apply_transform(Player *p) { player_transform(p, 1); }
/* 0x44bf10: when Perso+0x4b4 (the race board, message 1120) is set, the board gets the Perso's position, centre, rotation
 * (the tilted one of the up filter) and cell every frame. Its animation is its own controller +0x498 (board_request), which
 * gets the rider's requests except for the kind-1 death (0x76) and the race restart (0) and has none at the level start. */
void player_sync_board(Player *p)
{
    Instance *b = p->board, *r = p->inst;
    if (!b || !r || b == r) return;
    b->position = r->position; b->quat = r->quat; b->world = r->world;
    if (wenv("WOODY_BOARDLOG")) printf("  BOARD inst %u vis %d pos %.0f %.0f %.0f anim %d t %.2f | rider inst %u pos %.0f %.0f %.0f anim %d t %.2f\n", b->index, b->visible, b->position.x, b->position.y, b->position.z, b->anim, b->anim_time, r->index, r->position.x, r->position.y, r->position.z, r->anim, r->anim_time);
}

void player_place(Player *p, Vec3 pos, float yaw)
{
    bomb_drop(p); p->throw_hold = 0;
    p->pos = pos; p->yaw = yaw; p->vel = (Vec3){ 0, 0, 0 }; p->speed = 0; p->ramp_phase = 0; p->floor_y = pos.y; p->atk = 0; p->move_lock = 0; p->lanim = -1; p->step_u = -1.0f;
    jumper_reset(&p->jumper); p->on_ground = 1; p->cam_init = 0; player_apply_transform(p);
}

/* start of a real time cinematic (0x44eab0) on the Perso: instance position = P0 and the rows written straight from d,
 * {d x (0,1,0), d, (0,1,0)}, so the main instance always stands upright. The race rider's tilt (the up filter +0x210
 * of Orient 0x44bd30) is not used: Perso_Update, and with it Orient, is skipped while the cinematic runs (0x401cf9).
 * S1R: kept tilted by the last slope, Splinter's anim-10 run to the door drifted off the floor into the air. */
void player_cin_place(Player *p, Vec3 pos, float yaw)
{
    player_place(p, pos, yaw); player_transform(p, 0);
}

/* the camera as a volume actor (docs/EVENTS.md 2.1): 0x41f379..0x41f3cf at the end of Camera::Update, only once a script
 * has named a camera object with message 800 (CamMgr+0x664; K2R, S2R, W2B). The object takes the camera position and is
 * tested with 0x434740 -> 0x430210, the plain (not "perso") messages 101/102/103 through 0x441c90. Unlike the player's
 * test there is no cache: "was inside" is the VM's own list (0x443e20 = eko_vol_has_actor_f1), as in the original. */
void player_volumes_actor(Player *p, EkoVM *vm, Vec3 pt, uint32_t actor)
{
    if (!vm) return;
    for (uint32_t v = 0; v < p->nvol; v++) {
        Instance *in2 = p->vol_inst[v]; if (!in2->visible) continue;                   /* 0x430210: cell -1 = no test at all */
        int now = volume_contains(in2, p->vol_node[v], pt), was = eko_vol_has_actor_f1(vm, p->vol_id[v] & 0xffffff, actor);
        if (!was) { if (now) { eko_vol_enter(vm, p->vol_id[v], actor); if (wenv("WOODY_CAMVOL")) printf("  CAMVOL enter 0x%x (inst %u)\n", p->vol_id[v], in2->index); } }
        else if (now) eko_vol_in(vm, p->vol_id[v], actor);
        else { eko_vol_leave(vm, p->vol_id[v], actor); if (wenv("WOODY_CAMVOL")) printf("  CAMVOL leave 0x%x (inst %u)\n", p->vol_id[v], in2->index); }
    }
}

/* trigger volumes: enter / in / leave -> script VM (player = "perso" variants); runs in every Perso state */
static void player_volumes_y(Player *p, EkoVM *vm, float probe_y)
{
    if (vm) {
        Vec3 probe = { p->pos.x, p->pos.y + probe_y, p->pos.z };
        for (uint32_t v = 0; v < p->nvol; v++) {
            Instance *in2 = p->vol_inst[v]; if (!in2->visible) { continue; }
            int now = volume_contains(in2, p->vol_node[v], probe);
            if (now && !p->inside[v]) { eko_vol_perso_enter(vm, p->vol_id[v], p->inst->id); p->events_sent++; printf("  VOL enter 0x%x (inst %u)\n", p->vol_id[v], in2->index); }
            else if (now) { eko_vol_perso_in(vm, p->vol_id[v], p->inst->id); }
            else if (p->inside[v]) { eko_vol_perso_leave(vm, p->vol_id[v], p->inst->id); p->events_sent++; printf("  VOL leave 0x%x (inst %u)\n", p->vol_id[v], in2->index); }
            p->inside[v] = (uint8_t)now;
        }
    }
}

static Quat q_unit(Quat q);
static void player_volumes(Player *p, EkoVM *vm) { player_volumes_y(p, vm, P_VOL_PROBE_Y); }
/* 0x44b888..0x44b8c4, after the state dispatch of 0x44b530 in every Perso state: msgmask 0x200 = onGround +0x22c (getter
 * 0x44bcf0; not "Perso state is free"). +0x22c is written only by Perso_MoveCollide 0x4624f0 (0x462725 / 0x462733: the
 * result of the probe 0x436f00, states 0/1/2/3/4/6) and set to 1 by Reset 0x44abc0, the ground snap 0x4629da and a scripted
 * action with a vector 0x44dede; states 5/7/8/9 skip 0x4624f0 and leave it as it was. keep = one of those states (the port's
 * rocket ride writes on_ground = 0 for its own use, the scripted action does not go through the ground test) */
static void perso_mask200(Player *p, EkoVM *vm, int keep)
{
    if (!keep) p->ground_22c = p->on_ground;
    if (vm) { if (p->ground_22c) eko_msgmask_set(vm, p->inst->id, 0x200); else eko_msgmask_clear(vm, p->inst->id, 0x200); }   /* 0x44b89c / 0x44b8bf */
}
/* 0x443ff0(perso id): PersoLeave on every volume the VM has him in (0x4444b0); the port's "was inside" cache follows */
void player_leave_all(Player *p, EkoVM *vm)
{
    if (vm) eko_actor_leave_all(vm, p->inst->id);
    for (uint32_t v = 0; v < p->nvol; v++) { if (p->inside[v]) printf("  VOL leave_all 0x%x (inst %u)\n", p->vol_id[v], p->vol_inst[v]->index); p->inside[v] = 0; }
}

/* ---- Perso state 1: riding the race board (subtypes 4/5 = script types 18/19, docs/RACE.md) ------------------ */
static Vec3 xz_unit(Vec3 v) { float l = sqrtf(v.x * v.x + v.z * v.z); return l > 0 ? (Vec3){ v.x / l, 0, v.z / l } : (Vec3){ v.x, 0, v.z }; }
/* the sound source +0x4a4 of the ride loop (SoundFx 60, 2D): 0x468e50 at the top of every ride frame starts it when the last
 * frame was marked (bit 0) and it is not playing (bit 1), stops it when the last frame was not marked, and clears the mark;
 * 0x468e40 marks it at the end of a ride frame, 0x468e20 stops it at once (crash, race kill, SurfEnter) */
static void race_snd_update(Player *p)
{
    if ((p->race_snd & 1) && !(p->race_snd & 2)) { audio_fx(60, NULL, NULL); p->race_snd |= 2; }
    if (!(p->race_snd & 1) && (p->race_snd & 2)) { audio_fx_stop(60, NULL, 0); p->race_snd &= ~2; }
    p->race_snd &= ~1;
}
static void race_snd_stop(Player *p) { audio_fx_stop(60, NULL, 0); p->race_snd &= ~2; }
static void race_enter(Player *p, int respawn)                             /* SurfEnter 0x456150, from Reset for subtypes 4/5 */
{
    race_snd_stop(p);                                                      /* 0x468e20(+0x4a4, 0, 60) */
    p->race_stuck = 0; p->race_sub = 0;
    if (respawn) game_race_bonus_reset();                                  /* 0x44f8a0: [0x5e54f8] = 0 and every type-37 bonus back (Respawn 0x44f8f0 re-cells it) */
    p->race_bonus = p->race_bonus_ckpt;                                    /* +0x264 = +0x71c = +0x4e0 */
    p->boost_t = 0; p->race_lean = 0; p->race_lean_t = 0; p->race_crouch = 0; p->race_crash_t = 0;
    for (int i = 0; i < 4; i++) p->bfx.has_prev[i] = 0;                    /* the spray trail starts over */
    p->race_upf = (Vec3){ 0, 1, 0 };                                       /* Reset: +0x210 = (0,1,0) */
    p->race_start_t = anim_len(p, 0x5d, 0);
    p->lanim = -1; anim_request(p, 0x5d, 1.0f);                            /* A->Reset(), A->Request(0x5d) */
    if (p->board) { p->board_lanim = -1; board_request(p, 0x5d); }         /* B->Reset(), B->Request(0x5d): only after 1120, so not at the level start */
}
void player_boost(Player *p, Vec3 p0, Vec3 dir, float speed, float dur)  /* 0x456000 */
{
    Vec3 d = xz_unit(dir);
    p->boost_t = dur; p->boost_speed = speed;
    p->boost_target = (Vec3){ p0.x + d.x * speed * dur * 1.1f, p0.y, p0.z + d.z * speed * dur * 1.1f };   /* 0x4aa168 */
    audio_fx(59, NULL, NULL);
}
/* 0x438400 on the track polyline: index of the segment nearest to P (3D), searched from 0 every frame */
static int race_nearest(const Trajectory *t, Vec3 P)
{
    int best = -1; float bd = 0;
    for (uint32_t i = 0; i + 1 < t->npoints; i++) {
        Vec3 A = t->points[i], B = t->points[i + 1], d = vsub(B, A), c;
        float dp = vdot(d, P), da = vdot(d, A), db = vdot(d, B);
        if (dp - db > 0) c = B; else if (dp - da < 0) c = A;
        else { float u = (dp - da) / (db - da); c = (Vec3){ A.x + d.x * u, A.y + d.y * u, A.z + d.z * u }; }
        Vec3 e = vsub(c, P); float d2 = vdot(e, e);
        if (best < 0 || d2 < bd) { best = (int)i; bd = d2; }
    }
    return best < 0 ? 0 : best;
}
/* Race_Ride 0x456210: 1250 units/s from the first frame, no acceleration; actions 0/1 turn the ride direction about the
 * world up at 1.8 rad/s (1.2 crouched), not during the start anim or a boost; a turn that would point the board at or past
 * perpendicular to the nearest track segment is undone. Returns the displacement of this frame. */
static Vec3 race_ride(Player *p, const PlayerInput *in, float dt)
{
    Vec3 disp = { 0, 0, 0 };
    race_snd_update(p);                                                    /* 0x468e50(+0x4a4, 0, 60, vol -1): the ride loop runs while the frames are marked */
    switch (p->race_sub) {
    case 2: {                                                              /* Race_Crash 0x456ba0 */
        race_snd_stop(p);
        if (p->race_crash_t == 0.0f) { p->cam_dist = 400.0f; race_request(p, 0x70); jumper_force_fall(&p->jumper, 0); }
        p->race_crash_t += dt;
        float L = anim_len(p, 0x70, 1) + anim_len(p, 0x70, 0);
        if (p->race_crash_t >= L) { jumper_update(&p->jumper, 0, 1, p->on_ground, p->pos.y - p->floor_y, dt); disp.y = p->jumper.dy; }   /* before L he hangs where he crashed */
        if (p->race_crash_t >= L && p->on_ground) rumble(p, 0.5f, 0.5f);  /* P+0xdc, P+0xd8; not on the 2 s way out */
        if ((p->race_crash_t >= L && p->on_ground) || p->race_crash_t >= 2.0f) player_kill(p, 8);
        return disp; }
    case 1: disp = p->race_dir; break;                                     /* 0x456cf0: M+0x1c, the direction of the previous frame */
    case 0:
        if (!p->race_path || p->race_path->npoints < 2) return disp;      /* before message 1120: nothing */
        p->race_cam_req = 1; p->cam_zoom = 1.5f; p->cam_height = 100.0f; p->cam_dist = 4.0f;   /* 0x41f9f0(2), SetMode(0,0), 0x41f660 / 0x41fa60 / 0x41fa80 */
        if (p->has_ckpt) disp = (Vec3){ sinf(p->spawn_yaw), 0, cosf(p->spawn_yaw) };             /* +0x324 */
        else disp = (Vec3){ p->race_path->points[1].x - p->race_path->points[0].x, 0, p->race_path->points[1].z - p->race_path->points[0].z };
        p->race_stuck = 0; p->race_sub = 1; p->race_lean = 0; p->race_lean_t = 0;
        break;
    }
    int i = race_nearest(p->race_path, p->pos);
    Vec3 seg = { p->race_path->points[i + 1].x - p->race_path->points[i].x, 0, p->race_path->points[i + 1].z - p->race_path->points[i].z };   /* not normalized */
    float step = P_RACE_SPEED * dt;
    if (p->boost_t > 0) { p->boost_t -= dt; p->bfx.mode = 3; step = p->boost_speed * dt; disp = (Vec3){ p->boost_target.x - p->pos.x, 0, p->boost_target.z - p->pos.z }; }
    else p->bfx.mode = 2;                                                  /* the spray: sprites twice as big during a boost */
    disp = xz_unit(disp);
    Vec3 old = disp;
    float turn = (p->race_crouch ? 1.2f : 1.8f) * dt;                      /* 0x4aa39c, 0x4ab290 */
    if (p->boost_t > 0) turn = 0;
    if (p->race_start_t > 0) p->race_start_t -= dt;                        /* start anim: no steering */
    else if (in->left || in->right) {                                      /* action 0 (-1) wins over action 1 (+1) */
        float a = (in->left ? -1.0f : 1.0f) * turn, c = cosf(a), s = sinf(a);
        disp = (Vec3){ old.x * c - old.z * s, 0, old.x * s + old.z * c };  /* RotateAxis 0x440d40 about (0,1,0) */
    }
    float d1 = vdot(disp, seg), d2 = vdot(old, seg);
    if (d1 < 0.5f && d1 < d2) disp = old;                                  /* 0x4a9014 */
    if (disp.x * disp.x + disp.z * disp.z < 1e-4f) disp = (Vec3){ 1, 0, 0 };   /* M_SetRideDir: |dir| < 0.01 -> x = 1 */
    p->race_dir = xz_unit(disp); p->yaw = atan2f(p->race_dir.x, p->race_dir.z);   /* M+0x1c = M+0x10: the facing */
    disp = (Vec3){ p->race_dir.x * step, 0, p->race_dir.z * step };
    jumper_update(&p->jumper, in->jump, 1, p->on_ground, p->pos.y - p->floor_y, dt);   /* 0x462d70 with action 4, input allowed */
    disp.y = p->jumper.dy;
    p->bfx.active = 1;                                                     /* 0x4567d1: the app ticks the spray emitter this frame */
    p->race_snd |= 1;                                                      /* 0x468e40 */
    return disp;
}
/* crouch 0x465b10 in state 1: action 8 (the attack key here), anims 0x68 / 0x69 / 0x6a; lowers the body to 81 and halves
 * the wall radius, the speed stays. The stand-up ray of the race columns is 2 units long and is left out. */
/* ---- special attack 0x458bf0 (docs/PERSO_SPECIAL.md): action 11 on RELEASE, one charge Perso+0x254 ----------------
 * On the ground in state 0 with a charge: anim 0x13 (.ins 86, 4.07 s) with move lock and invulnerability for its length,
 * the streak/ring effect, and at 1.5 s the camera shake and vtbl[39](3.0, dir 0, Woody's feet, kind 2) on EVERY actor
 * that thought last frame (max 32) - there is no range test at all. Otherwise SoundFx 9 (not for the race characters). */
static void special_update(Player *p, const PlayerInput *in, float dt)
{
    int released = !in->special && p->special_prev; p->special_prev = in->special;   /* 0x467440(11) */
    if (released) {
        if (p->on_ground && player_state_free(p) && !p->race_char && p->special_st == 0 && p->special_charges > 0) {
            float T = anim_len(p, 0x13, 0);
            p->special_charges--; p->special_st = 1; p->special_t = 0;
            if (p->move_lock < T) lock_move(p, T);                            /* 0x44cce0(T, 0) = max */
            if (p->invuln_respawn < T) p->invuln_respawn = T;                 /* 0x44cd10: +0x270 = max (Hit and Kill 2..6, 8, 9; not pits, not water) */
            p->atk = 0; p->charge = 0; p->lanim = -1;
            game_special_fx();                                                /* 0x47ab90 */
            puts("  PLAYER special attack");
        } else if (!p->race_char) audio_fx(9, NULL, NULL);
    }
    if (p->special_st == 1) {
        p->special_t += dt;
        if (p->special_t >= 1.5f) {                                           /* [0x4aa184] */
            game_cam_shake(2.0f); rumble(p, 1.0f, 1.0f); p->special_st = 2;   /* P+0xb4, P+0xb0 */
            int n = 0;
            for (int i = 0; p->enemies && i < p->enemies->n && n < 32; i++) {   /* 0x4c5258[], max 32 */
                Enemy *e = &p->enemies->e[i];
                if (e->removed || !e->attackable || !e->inst->visible || e->hp <= 0 || !game_enemy_thinks(e->inst)) continue;
                n++;
                int died = enemy_hit(e, 3.0f /* P+0x94 */, (Vec3){ 0, 0, 0 }, p->pos, 2);   /* vtbl[38](3) after it is an empty ret 4 */
                printf("  SPECIAL hits enemy %u%s\n", e->inst->index, died ? " - dead" : "");
            }
        }
    } else if (p->special_st == 2) {
        p->special_t += dt;
        if (anim_len(p, 0x13, 0) <= p->special_t) p->special_st = 0;
    }
}
float player_body_height(const Player *p)                                 /* 0x462490; 0x4624c0 = P+0x118 * inst+0x54 (the crush scale) */
{
    float s = p->crush > 0 ? p->crush : 1.0f;
    if (p->race_char) return (p->race_crouch ? 81.0f : 160.0f) * s;
    return (p->duck ? P_DUCK_H : P_BODY_H) * s;
}
/* ducking 0x465b10 (docs/PERSO_DUCK.md 1.2): hold action 5 on the ground -> 0x31 (down, 0.375 s), 0x32 (lying, every frame),
 * released and the segment feet+61 .. feet+132 free -> 0x33 (up, 0.2 s). Sub-states 1 and 3 end on their timer only.
 * With a bomb 0x4e/0x4f/0x50. Every frame he is down, LockMove(dt, 0) = max: no walking, turning, jumping or attacking.
 * It is a pre-step of Perso::Update (0x44b797) for EVERY Perso state but 2 (dead) and attacks, so it also runs in the states
 * 3/4/5/7/8/9 (docs/PERSO_DUCK.md 1.3). anim_owned: the Perso state owns the animation there - climbing 0x14..0x17 (prio
 * 5000..5002), a scripted action (6000), the rocket 0x3b..0x3e (1800) all outrank the duck set (1750), and the handlers
 * 0x4651d0 / 0x44db50 / 0x4657f0 read neither +0x694 nor +0x238 - so only the sub-state, its timers, the lock and the body
 * height (61, for shots and lasers) go on, and he comes out of the state still ducking (Reset 0x44ad28 is the only clear).
 * ground = +0x22c as 0x44bcf0 reads it: the port's on_ground, except on the rocket, where the port keeps on_ground 0 but the
 * original's flag stays 1 from the mount (0x465740 needs the ground; 0x4657f0 never writes +0x22c and MoveCollide does not run). */
static int ray_4359b0(const Player *p, Vec3 a, Vec3 b, float *t, const Instance **inst_out);
static void duck_update(Player *p, const PlayerInput *in, float dt, int anim_owned, int ground)
{
    if (p->dead_kind || p->atk) return;                                     /* state 2 / +0x5b4: nothing, no LockMove either */
    int b = p->state6;                                                      /* 0x465bc0: the bomb set whenever state == 6, bomb or not */
    switch (p->duck) {
    case 0: if (in->duck && ground) { p->duck = 1; p->duck_anim = b ? 0x4e : 0x31; p->duck_t = anim_len(p, p->duck_anim, 0); if (!anim_owned) p->lanim = -1; } break;
    case 1: if ((p->duck_t -= dt) <= 0) p->duck = 2; break;
    case 2: p->duck_anim = b ? 0x4f : 0x32;
            if (!in->duck) {                                                /* 0x4359b0 from feet + P+0x10 to feet + P+0x0c - P+0x10: any hit keeps him down */
                Vec3 a = { p->pos.x, p->pos.y + P_DUCK_H, p->pos.z }, e = { p->pos.x, p->pos.y + (P_BODY_H - P_DUCK_H), p->pos.z }; float f;
                const Instance *hi = NULL; int kind = ray_4359b0(p, a, e, &f, &hi), blocked = kind != 0;   /* 0x465d0b / 0x465d18: [0x53a554] != 0 */
                if (wenv("WOODY_DUCKLOG") && blocked) printf("  DUCK blocked: kind %d inst %u f %.3f\n", kind, hi ? hi->index : 0u, f);
                if (!blocked) { p->duck_anim = b ? 0x50 : 0x33; p->duck_t = anim_len(p, p->duck_anim, 0); p->duck = 3; if (!anim_owned) p->lanim = -1; }
            }
            break;
    case 3: if ((p->duck_t -= dt) <= 0) p->duck = 0; break;
    }
    if (p->duck && p->move_lock < dt) p->move_lock = dt;                     /* 0x44cce0(dt, 0): keeps a longer lock */
    if (wenv("WOODY_DUCKLOG")) { static int prev = -1; if (p->duck != prev) printf("  DUCK %d -> %d (t %.2f, key %d, lock %.3f, ramp %d)\n", prev, p->duck, p->play_time, in->duck, p->move_lock, p->ramp_phase); prev = p->duck; }
}
/* ---- look-around, Perso state 3 + camera mode 0x200 (docs/PERSO_LOOK.md) ----------------------------------------------
 * 0x44b980, every frame: action 7 RELEASED (0x467440(7)) toggles. In state 3 it goes back to the previous state (0x44c9f0,
 * +0x220 = 0 or 6, no SetState) and raises +0x268 (made visible by 0x44b4a0); the same happens without the key as soon as
 * the camera is no longer in mode 0x200 (a script camera, a teleport's camera reset). Entry wants state 0 without an attack
 * or state 6 in sub-state 2 (carrying), on the ground and the follow camera (index 0); otherwise sound 9. The entry is
 * 0x464620 (idle count reset) + SetState(3), which drops a carried bomb - and the way back restores state 6 anyway. */
static void look_update(Player *p, const PlayerInput *in)
{
    int rel = !in->look && p->look_key; p->look_key = in->look;
    /* 0x459346 (read by the camera controller in the same frame): the relative mouse, overruled by the direction keys,
     * each worth 5 counts: right (action 1) +5, left (0) -5, back (3) -5, forward (2) +5 */
    {   /* ftol(Value * 5) (0x459393 0x4a9884 = 5.0, 0x4593b3 0x4ab2c8 = -5.0): a keyboard key is worth 1.0, the stick its deflection */
        float vx = in->ax != 0 ? fabsf(in->ax) : 1.0f, vz = in->az != 0 ? fabsf(in->az) : 1.0f;
        p->look_dx = in->right ? (int)(vx * 5.0f) : in->left ? (int)(vx * -5.0f) : in->mouse_dx;
        p->look_dy = in->back ? (int)(vz * -5.0f) : in->forward ? (int)(vz * 5.0f) : in->mouse_dy;
    }
    /* 0x44b4a0 is frame step 24, AFTER the render of the frame in which the state changed: the fade follows one frame late
     * (for one frame the eye camera sits in a visible Woody, and the follow camera sees an invisible one) */
    if (p->look) { p->inst->fade = p->inst->fade_target = 1.0f; p->inst->fade_rate = 100.0f; }                         /* state 3: 0x44e7f0(1, 1) */
    if (p->look_show) { p->look_show = 0; p->inst->fade = p->inst->fade_target = 0; p->inst->fade_rate = 100.0f; }   /* +0x268: 0x44e7f0(0, 1) */
    int forced = p->look && p->cam_mode != 0x200;                           /* 0x44b99c: state 3 and CamMgr+0x138 != 9 */
    if (!rel && !forced) return;
    if (p->look) {                                                          /* 0x44b9f0 */
        p->look = 0; p->state6 = p->look_prev6; p->look_show = 1;
        if (wenv("WOODY_LOOKLOG")) printf("  LOOK off (%s), back to state %d, facing %.0f\n", forced ? "camera left mode 0x200" : "key", p->state6 ? 6 : 0, p->yaw * 57.2958f);
        return;
    }
    int ok = !p->race_char && !p->dead_kind && !p->climb_sub && !p->use_root && !p->script_act && !p->ride && !p->follow && (p->state6 ? p->bsub == 2 : !p->atk);
    if (ok && p->on_ground && p->cam_mode == 1) {                           /* 0x44ba11: +0x22c, CamMgr+0x138 == 0 */
        idle_reset(p);                                                      /* 0x464620 */
        p->look_prev6 = p->state6; bomb_drop(p);                            /* SetState(3): 0x44c9ad lets go of the bomb, +0x220 = 6 */
        p->atk = 0; p->use_atk_disp = 0; p->target = NULL; p->has_target = 0; p->throw_hold = 0;   /* SetState clears +0x5b4, +0x5cd, +0x5f0 */
        p->look = 1;
        if (wenv("WOODY_LOOKLOG")) printf("  LOOK on (from state %d), facing %.0f\n", p->look_prev6 ? 6 : 0, p->yaw * 57.2958f);
    } else {
        if (!p->app_menu) audio_fx(9, NULL, NULL);                          /* 0x44ba5f: "can't" (only while the App is in a game, App+0 != 0, 0x44ba52) */
        if (wenv("WOODY_LOOKLOG")) printf("  LOOK refused (ground %d, camera mode %d)\n", p->on_ground, p->cam_mode);
    }
}
/* 0x459050 (camera controller, the frame the Perso enters state 3): 0x44c080(Perso, CamMgr+0x540, 1) = eye at the feet +
 * 0.9 * body height * scale, both angles 0, the Perso's rotation as the base (+0x54), limits +-1.2566 (72 deg) on +0x7c and
 * the pair (-1, 1) on +0x78, which the clamp test reads as "no limit"; deltas 0; then 0x41f9f0(2) cut + SetMode(9) */
void player_look_start(Player *p) { p->look_yaw0 = p->yaw; p->look_yaw = 0; p->look_pitch = 0; }
/* per frame: 0x459346 writes the view's horizontal direction of the LAST update back into the Mover (M+0x34, +0x1c, +0x10:
 * he turns with the view, one frame behind) and moves the eye (0x44c080(.., 0)); then the mode update 0x425b80 turns by the
 * deltas: min(min(|d|, 64) * dt * pi/16, pi/10) per frame, d > 0 turns right / down, d < 0 left / up; yaw (+0x78) free, pitch
 * (+0x7c) within +-72 deg. The view = RotX(pitch) * base * RotY(yaw), eye unchanged (+0x24 = 0), no smoothing, no shake. */
static Vec3 s_look_fwd;   /* -row 1 of CamMgr+0x570 (L+0x30), the view matrix of the LAST 0x425b80: never reset (the CamMgr ctor
                           * 0x41dca0 leaves it alone), so on the entry frame it holds the previous look-around's view, zero the first time */
void player_look_camera(Player *p, FreeCamera *cam, float dt)
{
    {   /* 0x459405..0x4594cf: d = (-L+0x3c, 0, -L+0x44) normalised in xz, |d| < 0.01 -> d.x = 1; M+0x34 = M+0x1c = M+0x10 = d.
         * One frame behind the view; in the entry frame (0x459050 has just run) it is the stale matrix: (1, 0, 0) the first time */
        Vec3 d = { s_look_fwd.x, 0, s_look_fwd.z }; float l = sqrtf(d.x * d.x + d.z * d.z);   /* M+0x38 = 0 (0x459433) */
        if (l > 0) { d.x /= l; d.z /= l; }
        if (sqrtf(d.x * d.x + d.z * d.z) < 0.01f) d.x = 1.0f;                 /* 0x459495 (0x4a94f8) */
        float was = p->yaw; p->yaw = atan2f(d.x, d.z); p->move_dir = d;
        if (wenv("WOODY_LOOKLOG") && fabsf(remainderf(p->yaw - was, 6.2831853f)) > 0.01f)
            printf("  LOOK facing %.1f -> %.1f (the view of the last update; stale in the entry frame) pos %.1f %.1f %.1f\n", was * 57.2958f, p->yaw * 57.2958f, p->pos.x, p->pos.y, p->pos.z);
    }
    Vec3 eye = { p->pos.x, p->pos.y + player_body_height(p) * p->inst->scale.y * 0.9f, p->pos.z };   /* 0x44c0a4: 0x4624c0 * 0.9 */
    const float k = 0.19634954f, cap = 0.31415927f;                         /* 0x4aa1e4 pi/16, 0x4aa1e0 pi/10 */
    int dx = p->look_dx, dy = p->look_dy;
    if (dx) { float a = (float)(dx < 0 ? (dx < -64 ? 64 : -dx) : (dx > 64 ? 64 : dx)) * dt * k; if (a > cap) a = cap; p->look_yaw += dx < 0 ? a : -a; }
    if (dy) { float a = (float)(dy < 0 ? (dy < -64 ? 64 : -dy) : (dy > 64 ? 64 : dy)) * dt * k; if (a > cap) a = cap; p->look_pitch += dy < 0 ? a : -a; }
    if (p->look_pitch > 1.2566371f) p->look_pitch = 1.2566371f;             /* 0x425cb7..0x425d28: +0x80 / +0x84 */
    if (p->look_pitch < -1.2566371f) p->look_pitch = -1.2566371f;
    cam->pos = eye; cam->yaw = p->look_yaw0 + p->look_yaw; cam->pitch = p->look_pitch; cam->fov_deg = CAM_FOV_Y; cam->letterbox = 0;
    s_look_fwd = (Vec3){ sinf(cam->yaw) * cosf(cam->pitch), sinf(cam->pitch), cosf(cam->yaw) * cosf(cam->pitch) };   /* -R.row1: the view's forward */
    if (wenv("WOODY_LOOKLOG") && (int)(p->play_time * 4) != (int)((p->play_time - dt) * 4)) printf("  LOOK view yaw %.1f pitch %.1f eye %.0f %.0f %.0f\n", cam->yaw * 57.2958f, cam->pitch * 57.2958f, eye.x, eye.y, eye.z);
}
/* 0x459c70, the Perso's half of the side view (runs while Perso+0x4ec, after 0x458bf0, before the Mover; 0x44b7be). Only while
 * +0x238 <= 0 (the move lock, 0x459c80): with no attack (+0x5b4), not climbing (+0x50c), not dead (state 2) and the camera in
 * mode index 5, action 0 held sets +0x4ed and clears +0x4ee, action 1 the reverse (0 wins); the flip byte p+0x20 is the OLD value
 * of the flag being cleared (a turn round, for one frame), else 0. On a flip the walking vector +0x500 is negated (0x459d18).
 * While the move lock runs the byte keeps its value: a lock that starts in the frame right after a flip leaves it at 1, and the
 * camera update then flips again every frame of the lock (a peck released one frame after the turn does it; the original's
 * blend then goes NaN, sv_ahead in main_engine.c avoids that; docs/CAMERA_SCRIPT.md 4.2). The height byte p+4 is the app's
 * behind_key. Then, lock or not and in every Perso state, the facing snaps to the walking vector (0x459def..0x459eaa): RampA's
 * dir M+0x34 = xzNormalize(+0x500) (|.| < 0.01 -> x = 1), M+0x1c = M+0x10 = it. There is no turn slerp in a side section: a
 * turn is a snap to -d, and what an attack, a climb start or a hit set as the facing earlier in the frame is overwritten here
 * (a later SetDir in the same frame, the climb 0x4650c3 or a ride, still wins for that frame). docs/CAMERA_SCRIPT.md 4.2.1. */
static void side_update(Player *p, const PlayerInput *in)
{
    if (!p->side_on) return;
    if (p->move_lock <= 0) {                                                    /* 0x459c80 */
        int flip = 0;
        if (!p->atk && !p->climb_sub && !p->dead_kind && p->cam_mode == 0x20) {
            if (in->left) { flip = p->side_r; p->side_l = 1; p->side_r = 0; }       /* 0x459cf1 */
            else if (in->right) { flip = p->side_l; p->side_r = 1; p->side_l = 0; } /* 0x459d84 */
        }
        if (flip) p->side_walk = (Vec3){ -p->side_walk.x, -p->side_walk.y, -p->side_walk.z };   /* 0x459d18: * -1.0 (0x4a9500) */
        p->side_flip = flip;                                                    /* 0x459d4e */
    }
    Vec3 w = { p->side_walk.x, 0, p->side_walk.z }; float l = sqrtf(w.x * w.x + w.z * w.z);   /* 0x459def: y = 0, xz length */
    if (l > 0) { w.x /= l; w.z /= l; }                                          /* 0x459e28: only when > 0 */
    if (sqrtf(w.x * w.x + w.z * w.z) < 0.01f) w.x = 1.0f;                       /* 0x459e6b: 0x4a94f8 = 0.01 */
    p->yaw = atan2f(w.x, w.z); p->move_dir = w;                                 /* 0x459e7e: M+0x1c (+0x3a4) = M+0x10 (+0x398) = M+0x34 (+0x3bc) */
}
/* the tail of the Perso's key steps (0x44b7a8..0x44b7ca): 0x44b980 look-around, 0x458bf0 special attack, then 0x459c70 side view.
 * They follow the attack controller 0x457a50, the pick-up 0x44ba70 and ducking 0x465b10 in every Perso state. */
static void perso_keys_tail(Player *p, const PlayerInput *in, float dt) { look_update(p, in); special_update(p, in, dt); side_update(p, in); }
/* message 30 [_, cs] (0x44cde9): LockMove(cs * 0.01, 0) = the longer of the two locks, and the idle record 1 (.ins 0, prio 6500).
 * No state test: it works in every Perso state. No level script sends it. */
void player_lock(Player *p, float t)
{
    if (t > p->move_lock) p->move_lock = t;
    p->ramp_phase = 0; p->speed = 0;                                        /* +0x238 > 0 zeroes the walk vector (0x44bc16) */
    anim_request(p, 1, 1.0f);
}
static void race_crouch(Player *p, const PlayerInput *in, float dt)
{
    if (p->race_start_t > 0) return;
    switch (p->race_crouch) {
    case 0: if (in->action && p->on_ground) { p->race_crouch = 1; race_request(p, 0x68); p->race_crouch_t = anim_len(p, 0x68, 0); } break;
    case 1: if ((p->race_crouch_t -= dt) <= 0) p->race_crouch = 2; break;
    case 2: race_request(p, 0x69); p->race_crouch_t = anim_len(p, 0x69, 0);
            if (!in->action) { race_request(p, 0x6a); p->race_crouch_t = anim_len(p, 0x6a, 0); p->race_crouch = 3; } break;
    case 3: if ((p->race_crouch_t -= dt) <= 0) p->race_crouch = 0; break;
    }
}
/* Race_CheckCrash 0x4567f0, after the collision: stuck for more than 5 frames (moved < 70 % of the wanted displacement),
 * or a wall straight ahead at both 40 and body height - 10 within 69 + 40 units. The original's rays also hit instances;
 * here they are world polygons only (a blocking instance still ends in the stuck test). */
static void race_check_crash(Player *p, Vec3 old_pos, Vec3 disp, float body_h)
{
    if (p->race_sub == 2) return;
    Vec3 mv = vsub(p->pos, old_pos); float moved = sqrtf(vdot(mv, mv)), want = sqrtf(vdot(disp, disp));
    if (want > 0 && moved / want < 0.7f) {                                 /* 0x4aa1d8; a frame without displacement is 0/0 in the original (counted as stuck), not here */
        if (++p->race_stuck > 5 && !wenv("WOODY_GOD")) { game_hit_star((Vec3){ p->pos.x, p->pos.y + 150.0f, p->pos.z }); p->race_sub = 2; p->race_crash_t = 0; rumble(p, 0.5f, 0.5f); puts("  RACE crash (stuck)"); }   /* Effect_Star 0x4750e0(inst.pos + (0,150,0)); rumble left out */
    } else p->race_stuck = 0;
    Vec3 U = p->ground_n, F = p->race_dir, fw = vcross(U, vcross(F, U));
    Vec3 end = { p->pos.x + fw.x * (P_RADIUS + 40.0f), p->pos.y + fw.y * (P_RADIUS + 40.0f), p->pos.z + fw.z * (P_RADIUS + 40.0f) };   /* 0x4ab294 */
    Vec3 n40 = { 0, 0, 0 }; int both = 1;
    const float hs[2] = { body_h - 10.0f, 40.0f };                        /* 0x4a9750 */
    for (int k = 0; k < 2; k++) {
        Vec3 a = { p->pos.x, p->pos.y + hs[k], p->pos.z }, b = { end.x, end.y + hs[k], end.z }, n, ni;
        float f = gel_ray_hit(p->gel, a, b, &n), fi;                     /* 0x4359b0: world polygons, then instance press nodes */
        if (player_ray_instances(p, NULL, a, b, &fi, &ni, NULL)) { f = fi; n = ni; }   /* an instance hit replaces the world hit (0x432f87) */
        if (f > 1.0f) both = 0; else if (k == 1) n40 = n;
    }
    if (both && p->race_sub != 2 && !wenv("WOODY_GOD")) {
        if (n40.x * n40.x + n40.z * n40.z > 1e-6f) p->yaw = atan2f(-n40.x, -n40.z);   /* Mover_SetFacing(-hitN) */
        p->race_sub = 2; p->race_crash_t = 0; game_hit_star((Vec3){ p->pos.x, p->pos.y + 150.0f, p->pos.z }); rumble(p, 0.5f, 0.5f); puts("  RACE crash (wall ahead)");
    }
}
/* 0x464c20: lean left/right on the ground, the jump set of 0x4642f0 in the air; nothing during the start anim or a crash */
static void race_anims(Player *p, const PlayerInput *in, float dt)
{
    if (p->race_sub == 2 || !p->board || p->race_start_t > 0) return;
    int js = p->jumper.state;
    if (js != 2) {
        int a = -1;
        if (js == 0 || js == 1) a = 0x6b; else if (js == 7) a = 0x6c;
        else if (js == 3 || (js == 4 && !p->on_ground)) { if (p->jumper.fell_off) a = 0x6e; else if (p->jumper.short_hop) a = 0x6d; }
        else if (js == 4 || js == 5) a = 0x6f;
        else if (js == 6) {                                                /* 0x4642f0 is shared with state 0: the long fall's landing and the dust */
            if (p->jumper.hard_fall) { a = 0x6f; lock_move(p, anim_len(p, 0x6f, 0)); game_bubble(p->inst, 1, 2.0f, 180.0f, 50.0f, NULL); }   /* 0x464470: LockMove (no effect in state 1) and the curse bubble */
            if (p->ground_kind == 2) game_land_dust((Vec3){ p->pos.x, p->pos.y + 30.0f, p->pos.z }, p->ground_n);   /* 0x464486 */
        }
        if (a >= 0) race_request(p, a);                                    /* 0x4643b6: B gets the same request */
        return;
    }
    if (p->race_crouch) { p->race_lean = 0; return; }
    if ((p->race_lean_t -= dt) > 0) return;
    int L = in->left, R = in->right && !L, a = 0;
    switch (p->race_lean) {
    case 0: if (L) { p->race_lean = 1; a = 0x5e; } else if (R) { p->race_lean = 3; a = 0x63; } break;
    case 1: if (L) { p->race_lean = 2; a = 0x5f; } else { p->race_lean = 0; a = 0x62; } break;
    case 2: if (!L) { p->race_lean = 1; a = 0x61; } break;
    case 3: if (R) { p->race_lean = 4; a = 0x64; } else { p->race_lean = 0; a = 0x67; } break;
    case 4: if (!R) { p->race_lean = 3; a = 0x66; } break;
    }
    if (a) { p->race_lean_t = anim_len(p, a, 0); race_request(p, a); }
}

/* render colour vt[26] 0x44cf50: while the invulnerability bonus runs (+0x704 > 0) the model flashes white, [0x5ac850] = 2 with
 * (255,255,255) = plus 255 on the lit vertex colour, clamped (0x43bdd9) = the texture at full MODULATE2X brightness. With 5 s
 * or more left: 0.1 s white in every 0.5 s (+0x708 accumulates dt, white while <= 0.1, -0.5 when it passes 0.5); in the last
 * 5 s every other frame (+0x70c counts 0, 1), normalised here to 60 fps so the flicker does not melt into grey at 300 fps.
 * Ordinary invulnerability (+0x270 after a respawn, +0x280 after a hit) has no blink at all (docs/PERSO_DEATH.md 6). */
static void bonus_blink(Player *p, float dt)
{
    int white = 0;
    if (p->bonus_inv > 0) {
        if (p->bonus_inv >= 5.0f) {                                        /* 0x4a9884 */
            p->bonus_inv_acc += dt;
            if (p->bonus_inv_acc >= 0.5f) { p->bonus_inv_acc -= 0.5f; white = 1; } else white = p->bonus_inv_acc <= 0.1f;
        } else white = ((int)(p->bonus_inv * P_REF_FPS) & 1) == 0;       /* +0x70c: white on every second frame */
    }
    p->inst->tint_mode = white ? 2 : 0; p->inst->tint_rgb[0] = p->inst->tint_rgb[1] = p->inst->tint_rgb[2] = 1.0f;
}

/* The segment test 0x4359b0(a, b, -1) as the sweep's head ray and the crush test use it (docs/PERSO_MOVE.md 6.6):
 * 0x497ed0 = the world half 0x497fb0 (the cells along a -> b; a polygon counts when a is on its front side and b strictly
 * behind it, gel_ray_front) and then vt[5] 0x432ab0 of every instance in the visited cells and the dynamic list
 * (ray_instances: any instance polygon on the segment replaces the world hit and the LAST one tested wins; a start inside a
 * press node is kind 3 and blocks every later polygon).
 * Kinds: 0 nothing, 1 world polygon, 2 instance polygon, 3 a inside a press node. *t = the fraction of a -> b. */
static int ray_4359b0(const Player *p, Vec3 a, Vec3 b, float *t, const Instance **inst_out)
{
    float tw = gel_ray_front(p->gel, a, b); int kind = tw < 1.0f ? 1 : 0; InsRayHit h;
    int k = ray_instances(p, NULL, a, b, kind ? tw : 2.0f, &h);
    if (k) { *t = h.t; if (inst_out) *inst_out = h.in; return k; }
    *t = kind ? tw : 2.0f; if (inst_out) *inst_out = NULL; return kind;
}
int player_ray_full(const Player *p, Vec3 a, Vec3 b, float *t) { return ray_4359b0(p, a, b, t, NULL); }

/* Crush test 0x462a40 (Perso_Update 0x44b87c, right after MoveCollide 0x4624f0 - and 0x4567f0 in state 1 - in the Perso states
 * 0, 1, 2, 3, 4 and 6; not in 5, 7, 8, 9, which skip MoveCollide). Nothing while dead (+0x26c). h = 0x4624c0 / +0x54 = the
 * unsquashed body height (193, 61 ducked, 160/81 in the race); the ray 0x4359b0 from feet + 1 to feet + h - 1 (start cell
 * 0x428ce0 must exist). Crushed when it hits (t < 1), he is on the ground (+0x22c, 0x44bcf0) and either an instance polygon
 * was hit whose animation is running (kind 2 and inst+0xa0 != 0, 0x462b40) or - world polygon or inside a press node - he is
 * standing on an instance (+0x298, 0x462b59): P+0x2e8 = max(free height, 2) / (h - 2), at most 1; under 0.3 (0x4aab98)
 * Kill(4) (vt[38], 0x462bed) and the scale is kept at 0.01 or more. Otherwise P+0x2e8 = 1. Orient 0x44bd00 copies P+0x2e8 into
 * the instance's z scale inst+0x54 every frame: the model is squashed flat (model z is up) and, since 0x4624c0 = P+0x118 *
 * inst+0x54, so is every body height of the next frame (the sweep's band, shots, the camera eye). */
static void crush_test(Player *p)
{
    if (p->dead_kind) return;
    float h = player_body_height(p) / (p->crush > 0 ? p->crush : 1.0f);
    Vec3 a = { p->pos.x, p->pos.y + 1.0f, p->pos.z }, b = { p->pos.x, p->pos.y + h - 1.0f, p->pos.z };
    if (gel_cell(p->gel, a) < 0) return;                                  /* 0x462ae3 */
    float t; const Instance *hi; int k = ray_4359b0(p, a, b, &t, &hi);
    if (k && t < 1.0f && p->on_ground && (k == 2 ? hi->a_speed != 0 : p->att_inst != NULL)) {
        float free = (b.y - a.y) * t; if (free < 2.0f) free = 2.0f;           /* 0x4a9870 */
        float s = free / (h - 2.0f); if (s > 1.0f) s = 1.0f;
        if (wenv("WOODY_CRUSHLOG")) printf("  CRUSH kind %d inst %u free %.1f of %.1f -> scale %.3f", k, hi ? hi->index : 0u, free, h - 2.0f, s), puts("");
        p->crush = s;
        if (s < 0.3f) { player_kill(p, 4); if (p->crush < 0.01f) p->crush = 0.01f; }
        return;
    }
    p->crush = 1.0f;
}

/* Perso_MoveCollide 0x4624f0 -> SweepCylinder 0x437180 (docs/PERSO_MOVE.md 6.1/6.2): actor push, then move in substeps of
 * at most 10 units; per substep one xz push-out vector of the body (body_push, 0x407000 / 0x408600) x0.9, landing clamp
 * when falling, ground clinging when walking; then the floor under feet + 43. Runs in states 0, 2, 3, 6, 7 after Perso_Move
 * and in state 4 (climbing) after 0x4651d0 (0x44b857). The GetHeight of the sweep (0x4372a2, 0x4373d5) probes from the
 * body centre, so while falling a ledge up to half the body height above the feet catches him (0x4373ec). */
static void move_collide(Player *p, Vec3 *dispp, float dt, int racing, const Instance **hit_inst_out, const InsNode **hit_node_out)
{
    const Instance *hit_inst = NULL; const InsNode *hit_node = NULL; int found;
    Vec3 np, disp, mc_carry = { 0, 0, 0 }, mc_pos = p->pos, mc_local = p->att_local, mc_world = p->att_world; const Instance *mc_att = p->att_inst;
    /* P+0x08 = 160 standing / 81 crouched for the race columns (0x462490), wall radius halved while crouched in state 1 (0x462517) */
    const float body_h = player_body_height(p), radius = racing && p->race_crouch ? P_RADIUS * 0.5f : P_RADIUS;
    actor_push(p, radius, body_h, dt, dispp); disp = *dispp;
    {
        const float half = body_h * 0.5f;
        Vec3 cur = { p->pos.x, p->pos.y + half, p->pos.z };
        Vec3 carry = attach_delta(p);                                 /* 0x436d20: the platform moved under the player */
        mc_carry = carry;
        Vec3 d = { disp.x + carry.x, disp.y + carry.y, disp.z + carry.z };
        int mode = d.y > 0.1f ? 3 : (d.y < -0.1f ? 2 : 0), wall = 0;
        int n = (int)(floorf(sqrtf(vdot(d, d)) / P_SUBSTEP) + 1.5f);
        d.x /= n; d.y /= n; d.z /= n;
        float margin = 5.0f;
        float gy = world_ground(p, cur, &found, &hit_inst, &hit_node);
        if (found && cur.y - half - 1.0f < gy) { margin = P_STEP + 1.0f; if (mode == 0) mode = 1; }
        for (; n > 0; n--) {
            Vec3 prev = cur;
            cur.x += d.x; cur.y += d.y; cur.z += d.z;
            float feet = cur.y - half; int hit = 0;
            /* band [feet + margin, feet + H]: up = feet + H - centre, down = centre - (feet + margin) ([0x53a350], [0x53a54c]) */
            Vec3 push = body_push(p, cur, radius, feet + body_h - cur.y, cur.y - (feet + margin), &hit);
            if (hit) { wall = 1; cur.x += push.x * 0.9f; cur.z += push.z * 0.9f; }
            if (hit && wenv("WOODY_PUSHLOG")) printf("sweep: contact at %.0f %.0f %.0f band %.0f..%.0f push %.1f %.1f", cur.x, feet, cur.z, margin, body_h, push.x, push.z), puts("");
            gy = world_ground(p, cur, &found, &hit_inst, &hit_node);
            if (!found) continue;
            if (mode == 2) { if (cur.y - half < gy) cur.y = gy + half; }
            else if (mode == 1) { float f = cur.y - half; if (f - gy < P_STEP && f > gy) cur.y = gy + half; }
            /* the head ray 0x43744f..0x43753d: [0x10] = (H - half) + d.y (0x4372ed; H, not the step - positive for every substep
             * of at most 10 units), so it runs every substep, standing or ducked: 0x4359b0 from the previous centre to the head
             * (cur.x, feet + H, cur.z). A hit (world or instance polygon) puts the head at the hit point, cur.y = hit.y - (H - half),
             * but never lower than gy + half: a ceiling stops a jump. If the previous centre is inside a press node (kind 3) it
             * casts again from (cur.x, gy + 0.1, cur.z) to the head (0x4374d1) and applies any hit the same way. */
            if (body_h - half + d.y > 0) {
                Vec3 head = { cur.x, cur.y + (body_h - half), cur.z }, from = prev; float t; const Instance *ri;
                int k = ray_4359b0(p, from, head, &t, &ri);
                if (k == 3) { from = (Vec3){ cur.x, gy + 0.1f, cur.z }; k = ray_4359b0(p, from, head, &t, &ri); }
                if (k) {
                    float y = from.y + (head.y - from.y) * t - (body_h - half);
                    if (wenv("WOODY_PUSHLOG")) printf("sweep: head ray kind %d inst %u t %.3f centre %.1f -> %.1f (floor %.1f)", k, ri ? ri->index : 0u, t, cur.y, y - half < gy ? gy + half : y, gy), puts("");
                    cur.y = y - half < gy ? gy + half : y;
                }
            }
        }
        p->wall_contact = wall;                                       /* P+0x2e0 ([0x53a554] = 3 after any hit) */
        np.x = cur.x; np.y = cur.y - half; np.z = cur.z;
    }
    /* ground probe from feet + 43 (0x436f00 -> GetHeight 0x435650): on the ground iff feet - ground < 1.0, which also
     * steps up onto floors up to 43 higher. Without any floor the original treats the probe point as ground; here the
     * player simply keeps falling and is held at the bottom of the world. */
    {
        Vec3 probe = { np.x, np.y + P_PROBE_Y, np.z };
        float gy = world_ground(p, probe, &found, &hit_inst, &hit_node);
        p->floor_is_hull = found && hit_node != NULL;
        if (found) p->floor_y = gy;
        if (found && np.y - gy < 1.0f) { p->on_ground = 1; np.y = gy; } else p->on_ground = 0;
        p->ground_n = p->on_ground ? g_ground_n : (Vec3){ 0, 1, 0 };            /* 0x45a110 -> Mover+0xd0 */
        p->race_floor_n = found && g_ground_n.y >= 0 ? g_ground_n : (Vec3){ 0, 1, 0 };   /* 0x4628e0: the floor query's normal, (0,1,0) if it points down */
        p->ground_kind = found ? ground_type(p, g_ground_mat) : 0;              /* 0x4628e0 -> Perso+0x308 */
        /* 0x436d80 / 0x436d10: the point remembered on the platform is the probe point itself (swept feet + 43, before the
         * snap to the floor; 0x436f54 passes the probe pointer), not the feet: on a tilting platform (W3C model 10) a point
         * 43 above the deck moves 43·ω further sideways than the deck, and that is what 0x436d20 carries him by */
        attach_store(p, p->on_ground ? hit_inst : NULL, hit_node, probe);
    }
    if (np.y < p->gel->bbox[2] - 2000.0f) { np = p->pos; player_kill(p, 7); }     /* below the world: "disappear" death (the original leaves this to script volumes) */
    p->pos = np;
    *hit_inst_out = hit_inst; *hit_node_out = hit_node;
    if (wenv("WOODY_MCLOG"))                                           /* testing: one line per MoveCollide, as tools/wverify.py --probe move */
        printf("MC local %.2f %.2f %.2f world %.2f %.2f %.2f | ", mc_local.x, mc_local.y, mc_local.z, mc_world.x, mc_world.y, mc_world.z),
        printf("MC pos %.2f %.2f %.2f disp %.2f %.2f %.2f att %d carry %.2f %.2f %.2f -> %.2f %.2f %.2f g %d inst %d n %.3f %.3f %.3f slide %d B %.1f\n",
               mc_pos.x, mc_pos.y, mc_pos.z, disp.x, disp.y, disp.z, mc_att ? (int)mc_att->index : -1, mc_carry.x, mc_carry.y, mc_carry.z,
               np.x, np.y, np.z, p->on_ground, hit_inst ? (int)hit_inst->index : -1, g_ground_n.x, g_ground_n.y, g_ground_n.z, p->sliding, p->slide_speed);
}

/* ---- Perso state 7: carried by an object's vector marker (docs/PERSO_STATE7.md) -------------------------------------
 * Message 1044 [inst] (0x4451a1 -> 0x44e140): SnapToGround 0x462990, +0x55c = inst, SetState(7), then the anim controller
 * A: Reset, Request(1), Tick(dt), Reset - logical anim 1 = .ins 0 (the idle pose) at speed 3, prio 6500, restarted.
 * Every frame 0x44e1c0 (also while +0x690 freezes the Perso): 0x42f6b0(inst, typecode 0, out, n 0) gives the marker's
 * two points A, B in world space; position +0x1f4 = A, Mover_SetDir(M, xz-normalize(B - A)) 0x459ff0 = facing (zero
 * -> (1,0,0)) and all three ramps reset (no walking, sliding or knockback). No MoveCollide, no volume test, no 0x462a40,
 * and 0x463e60 neither requests an animation nor ticks the controller in state 7 (table 0x463f14 slot 6 = 0x463f11), so
 * nothing replaces anim 1. Message 1045 (0x44e1a0) = +0x55c = 0 + SetState(0). No shipped level sends 1044 or 1045. */
static void follow_update(Player *p)                                       /* 0x44e1c0 */
{
    Vec3 a, d;
    if (game_inst_vector(p->follow, 0, &a, &d)) {
        float l = sqrtf(d.x * d.x + d.z * d.z);
        p->pos = a;                                                        /* +0x1f4 = A: the feet on the marker's first point */
        p->yaw = l > 0.01f ? atan2f(d.x, d.z) : atan2f(1.0f, 0.0f);        /* 0x459ff0: |dir| < 0.01 -> x = 1 */
        p->follow_nomark = 0;
    } else if (!p->follow_nomark) {                                        /* the original does not test the result and copies an uninitialised stack buffer */
        p->follow_nomark = 1; printf("  state 7: instance %u has no vector marker (typecode 0), position kept\n", p->follow->index);
    }
    p->move_dir = (Vec3){ sinf(p->yaw), 0, cosf(p->yaw) };                 /* RampA.dir = M+0x1c = M+0x10 */
    p->speed = 0; p->ramp_phase = 0; slide_reset(p); p->push_t = 0; p->push_speed = 0;   /* 0x467110 on RampB, RampC, RampA */
    p->vel = (Vec3){ 0, 0, 0 };                                            /* +0x204 stays 0: no displacement of his own */
    player_apply_transform(p);                                             /* Perso_Orient 0x44bd00 */
    if (wenv("WOODY_FOLLOWLOG")) printf("  state 7: pos %.0f %.0f %.0f facing %.0f\n", p->pos.x, p->pos.y, p->pos.z, p->yaw * 57.2958f);
}
void player_follow(Player *p, const Instance *obj)                         /* 0x44e140 */
{
    if (!obj || p->dead_kind) { puts("  1044 refused (no instance, or dead)"); return; }   /* port: the original has no test (NULL crashes in 0x42f6b0) */
    player_ground_snap(p);                                                 /* 0x462990: floor under feet + 43, onGround = 1, Jumper reset */
    p->ground_22c = 1;
    bomb_drop(p); p->throw_hold = 0; p->look = 0;                          /* SetState(7) 0x44c980: drops a carried bomb; out of state 3 without +0x268 */
    p->script_act = 0; p->use_root = 0; p->climb_sub = 0; p->ride = NULL;  /* whatever state he was in ends (0x44e140 tests none) */
    p->atk = 0; p->charge = 0; p->use_atk_disp = 0; p->target = NULL; p->has_target = 0;   /* SetState clears +0x50c, +0x5b4, +0x5cd, +0x5f0 */
    p->follow = obj; p->follow_nomark = 0;
    p->lanim = -1; anim_request(p, 1, 1.0f); p->inst->anim_time = 0; p->lanim = -1;   /* A->Reset (next start restarts), Request(1), Tick, Reset */
    printf("  PLAYER state 7: follows instance %u\n", obj->index);         /* the marker places him from the next Perso update on (0x44b8b4) */
}
void player_follow_end(Player *p)                                          /* 0x44e1a0 */
{
    if (!p->follow) return;                                                /* port: the original's SetState(0) would also end any other state */
    p->follow = NULL; p->lanim = -1;
    printf("  PLAYER state 7 ends at %.0f %.0f %.0f\n", p->pos.x, p->pos.y, p->pos.z);
}

void player_update(Player *p, const PlayerInput *in, float dt, EkoVM *vm, float cam_yaw)
{
    if (dt <= 0) return;
    g_rider = p->inst; g_rider_board = p->board;
    p->vy_corr = 0;
    p->play_time += dt;                                                   /* 0x453ca0: the accumulator Perso+0x710 runs in every state, it is the level time on the results screen */
    if (p->move_lock > 0) p->move_lock -= dt;
    if (p->invuln_respawn > 0) p->invuln_respawn -= dt;
    if (p->invuln_hit > 0) p->invuln_hit -= dt;
    if (p->soft_t > 0) p->soft_t -= dt;
    if (p->bonus_inv > 0) p->bonus_inv -= dt;                             /* 0x44b1fa */
    bonus_blink(p, dt);
    if (p->game_state == 0) return;                                      /* waiting for the respawn */
    air_move_start(p, in);                                               /* 0x465e50: pre-step of every Perso state, after 0x464ef0 (0x44b764) */
    if (!p->dead_kind && !p->race_char && (p->script_act || p->ride || p->climb_sub || p->follow)) duck_update(p, in, dt, 1, p->ride ? 1 : p->on_ground);   /* 0x465b10 also in the states 5 / 8 / 4, which return early below; before 0x44b980 as in 0x44b797 */
    /* fall damage 0x44b220: landing after more than 1500 fallen costs one heart */
    if (!p->dead_kind && p->jumper.state == 6 && p->atk == 0 && p->jumper.fallen >= J_HARD_FALL) {
        rumble(p, 0.8f, 0.5f);                                           /* P+0xcc, P+0xc8 */
        p->health -= 1.0f; printf("  PLAYER fall damage, health %.0f\n", p->health);
        if (p->health <= 0) { p->health = 0; player_kill(p, 8); }
    }
    if (!p->dead_kind && p->health <= 0) player_kill(p, 3);
    if (p->regrab > 0) p->regrab -= dt;
    if (p->hit_anim_t > 0) p->hit_anim_t -= dt;
    if (p->dead_kind) p->dead_T += dt;
    if (p->follow && (p->dead_kind || p->script_act || p->ride || p->climb_sub)) p->follow = NULL;   /* SetState(2 / 5 / 8 / 4) left state 7 (+0x55c itself stays in the original) */
    /* 0x44b980 / 0x458bf0 / 0x459c70 run in every Perso state, after the attack controller and ducking (0x44b7a8): here for the
     * states 5 / 8 / 4, which return below, and after the attack block for the others */
    if (!p->dead_kind && !p->race_char && (p->script_act || p->ride || p->climb_sub || p->follow)) perso_keys_tail(p, in, dt);
    if (!p->dead_kind && p->script_act) {                                  /* state 5, 0x44db50: the Perso stands still, the movement is in the root track of the animation */
        int door = p->script_act == 17 || p->script_act == 18;
        anim_request(p, p->script_log, 1.0f);
        if (p->script_carry) {                                             /* 0x44db76 (message 1043 only; no shipped script sends it): the instance sits on the camera-track point of
                                                                            * the running animation (0x42fa40, like the carried bomb) with the Perso's rotation (+0x28..+0x48 copied) */
            const Model *m = p->inst->model; int a = p->inst->anim; Vec3 at, tg;
            float ph = (uint32_t)a < m->nanims && m->anims[a].duration_s > 0 ? p->inst->anim_time / m->anims[a].duration_s : 0;
            if (ins_camera_eval(p->inst, a, ph, &at, &tg)) { Instance *c = p->script_carry; c->position = at; c->quat = p->inst->quat; mat4_from_trs(&c->world, c->position, c->quat, c->scale); }
        }
        if (p->script_act == 17 && !p->script_faded && p->script_t < 0.6f) { p->script_faded = 1; p->fade_req = 1; }
        p->script_t -= dt;
        if (!door) {                                                       /* 0x44e290 (docs/OBJECTS.md 1.5): pos stays put, the root track of the action carries the model */
            float rest = p->script_t > 0 ? p->script_t : 0, f = p->script_total > 0 ? (p->script_total - rest) / p->script_total : 1.0f;
            Vec3 q, fw;
            if (ins_root_at(p->inst, p->script_act, f, 1, &q, &fw)) {
                p->use_root = 1;
                p->root_pos = (Vec3){ p->pos.x + (q.x - p->pos.x) * f, p->pos.y + (q.y - p->pos.y) * f, p->pos.z + (q.z - p->pos.z) * f };
            }
        }
        if (p->script_t <= 0) {
            if (p->script_act == 18) {                                     /* he came out backwards: facing flips, ground snap, idle, follow camera */
                p->yaw += 3.14159265f; int found; float gy = player_ground_query(p, p->inst, (Vec3){ p->pos.x, p->pos.y + P_PROBE_Y, p->pos.z }, &found);
                if (found) { p->pos.y = gy; p->floor_y = gy; } p->lanim = -1; anim_request(p, 0, 1.0f);
                if (!p->side_on) p->cam_end_req = 1;                       /* 0x44dce9: 0x44e5a0 (follow camera behind the NEW facing, 0.5 s travelling) only outside the side view +0x4ec */
            } else if (!door) {                                            /* last frame of 0x44e290: he ends where the root track left him, facing -E.row2 */
                Vec3 q, fw;
                if (ins_root_at(p->inst, p->script_act, 1.0f, 1, &q, &fw)) { p->pos = q; if (fw.x * fw.x + fw.z * fw.z > 1e-6f) p->yaw = atan2f(fw.x, fw.z); }
                int found; float gy = player_ground_query(p, p->inst, (Vec3){ p->pos.x, p->pos.y + P_PROBE_Y, p->pos.z }, &found);
                if (found) { p->pos.y = gy; p->floor_y = gy; }             /* SnapToGround 0x462990 */
                p->use_root = 0; p->lanim = -1; anim_request(p, 0, 1.0f); p->cam_end_req = 1;
            } else lock_move(p, 0.3f);                                     /* 17: the pose is held until the teleport (message 26) resets the controller */
            p->script_act = 0; p->script_carry = NULL;
        }
        player_apply_transform(p); perso_mask200(p, vm, 1); return;       /* state 5: no 0x4624f0, +0x22c kept */
    }
    if (!p->dead_kind && p->ride) {                                        /* state 8, 0x4657f0: the Perso follows the rocket (seat marker + its full rotation); no move, no collision, no gravity */
        Quat rq = p->ride_q; int st = p->ride_state, can = 0, ending = 0;
        if (p->ride_t > 0) {                                               /* first 0.3 s he stands, then 0.4 s to the saddle */
            p->ride_t -= dt;
            if (p->ride_t < 0.4f) { float u = (0.4f - p->ride_t) / 0.4f; if (u > 1) u = 1; p->ride_cur = q_slerp(p->ride_q0, rq, u);
                p->pos = (Vec3){ p->ride_p0.x + (p->ride_seat.x - p->ride_p0.x) * u, p->ride_p0.y + (p->ride_seat.y - p->ride_p0.y) * u, p->ride_p0.z + (p->ride_seat.z - p->ride_p0.z) * u }; }
        } else { p->pos = p->ride_seat; p->ride_cur = rq; }
        switch (st) { case 1: anim_request(p, 0x3b, 1.0f); break; case 2: case 3: anim_request(p, 0x3c, 1.0f); break; case 4: anim_request(p, 0x3d, 1.0f); break;
                      case 5: anim_request(p, 0x3e, 1.0f); can = 1; break; case 6: case 7: can = 1; ending = 1; break; default: break; }
        int leave = ((in->jump && !p->ride_jprev) || (in->action && !p->ride_aprev)) && can;
        if (st == 0 || st == 9) leave = 1;                                 /* port deviation (ROCKET.md 0.4): the original hangs here when the rider survives the blast */
        p->ride_jprev = in->jump; p->ride_aprev = in->action;
        Quat u = q_unit(p->ride_cur); Vec3 nose = { -2 * (u.x * u.y - u.z * u.w), -(1 - 2 * (u.x * u.x + u.z * u.z)), -2 * (u.y * u.z + u.x * u.w) };   /* model -Y */
        if ((p->ride_t <= 0 || leave || ending) && nose.x * nose.x + nose.z * nose.z > 1e-4f) p->yaw = atan2f(nose.x, nose.z);
        if (leave) {                                                       /* plain fall, no carried speed, facing = flight direction */
            p->ride = NULL; jumper_reset(&p->jumper); jumper_force_fall(&p->jumper, 1); p->on_ground = 0; p->lanim = -1; p->speed = 0; p->ramp_phase = 0; p->action_prev = in->action;
            p->floor_y = p->pos.y; player_apply_transform(p); puts("  PLAYER leaves the rocket");
        } else { p->on_ground = 0; p->inst->position = p->pos; p->inst->quat = p->ride_cur; mat4_from_trs(&p->inst->world, p->pos, p->ride_cur, p->inst->scale); }
        player_volumes_y(p, vm, 20.0f);                                    /* 0x462760(p, 20.0) */
        perso_mask200(p, vm, 1); return;                                   /* state 8 skips 0x4624f0: +0x22c keeps the value from before the ride */
    }
    if (!p->dead_kind && p->follow) { follow_update(p); perso_mask200(p, vm, 1); return; }   /* state 7, 0x44e1c0: no 0x4624f0, no volumes, +0x22c kept */
    if (!p->dead_kind && p->climb_sub) {                                   /* state 4: 0x4651d0, then Perso_MoveCollide 0x4624f0 (0x44b834) */
        Vec3 d = climb_update(p, in, dt); const Instance *hi; const InsNode *hn;
        p->vel = (Vec3){ d.x / dt, d.y / dt, d.z / dt };
        move_collide(p, &d, dt, 0, &hi, &hn);
        crush_test(p);                                                     /* 0x44b87c */
        player_apply_transform(p); perso_mask200(p, vm, 0); player_volumes(p, vm); return;   /* state 4 runs 0x4624f0: +0x22c = its probe */
    }
    if (p->dead_kind) { p->climb_sub = 0; p->use_root = 0; }
    int racing = p->race_char && !p->dead_kind;                           /* Perso state 1: no attacks, no Mover (0x44b530) */
    if (!p->dead_kind && !racing && climb_try(p, in)) {                  /* 0x464ef0 (0x44b753) before 0x457a50 / 0x44ba70 */
        p->action_prev = p->climb_act_prev = in->action; perso_keys_tail(p, in, dt); player_apply_transform(p); perso_mask200(p, vm, 0); player_volumes(p, vm); return; }
    if (!p->dead_kind && !racing) { attack_update(p, in, dt);
        if (p->climb_sub) {                                                /* substate 8 ended in SetState(4): state 4 from now on, no jumper / move in this frame */
            p->action_prev = p->climb_act_prev = in->action; perso_keys_tail(p, in, dt); player_apply_transform(p); perso_mask200(p, vm, 0); player_volumes(p, vm); return; }
        attack_trigger(p, in, dt); p->steep_edge = 0; duck_update(p, in, dt, 0, p->on_ground); }
    perso_keys_tail(p, in, dt);                                           /* 0x44b980, 0x458bf0, 0x459c70 after 0x457a50 / 0x44ba70 / 0x465b10 (0x44b7a8) */
    Vec3 disp;
    if (racing) { race_crouch(p, in, dt); disp = race_ride(p, in, dt); }
    else if (air_move_tick(p, dt, &disp)) { }                              /* 0x465fe0 returned 1: the air dash / double jump owns +0x204, Perso_Move
                                                                            * stops there (no Mover, no Jumper, no +0x244) */
    else {
    /* Perso_Move 0x44bb20: no input (no walking, no jump) while locked or attacking; states 2 and 3 call it with arg 0 (0x44b8a3) */
    int allow = !(p->move_lock > 0 || p->atk != 0 || p->dead_kind || p->look);
    /* ... and while the knockback timer M+0xec (+0x474, 0.2 s from the hit, 0x45a140) runs (0x44bb48): no input either, but the Mover is
     * not stopped - RampA just gets no key and runs out as on a release (the knockback does not touch it) */
    int input = allow && !(p->push_t > 0);
    /* 0x45a850 loads RampA's times every frame: P+0x2c / P+0x30 (0.25 / 0.1 s), on slippery ground (Perso+0x308 == 1) P+0x34 / P+0x38 */
    const int ice = p->ground_kind == 1;
    const float acc_T = ice ? P_ICE_ACC_TIME : P_ACC_TIME, dec_T = ice ? P_ICE_DEC_TIME : P_DEC_TIME;
    /* input direction relative to the camera */
    float ix = input ? (float)(in->right - in->left) : 0, iz = input ? (float)(in->forward - in->back) : 0, stick = 1.0f;
    if (input && (in->ax != 0 || in->az != 0)) { ix = in->ax; iz = in->az; stick = sqrtf(ix * ix + iz * iz); if (stick > 1.0f) stick = 1.0f; }   /* 0x45a4b0: the deflection scales speed and turn */
    float len = sqrtf(ix * ix + iz * iz);
    if (p->side_on) {
        /* 0x45b292..0x45b2af: with flag 2 (allow) and Perso+0x4ec the Mover runs the side walk 0x45a7b0 instead of 0x45a4b0. It clears
         * flags 8/0x10/0x20 (0x45a1f0) and reads only Held(0) / Held(1) (the left / right keys, or the stick past its dead zone): the held
         * key must be the one the facing belongs to, (Held(0) && +0x4ed) || (Held(1) && +0x4ee) - the swap of both pairs for
         * CamMgr+0x61c == 1 (0x45a7eb) changes nothing. Then target M+0x44 = M+0x48 (= P+0x1c, 600, copied by 0x45b0a3) and flag 8, no
         * turn slow-down and no stick scaling; else target 0 and flag 0x10 (= no input: the ramp runs out). The direction is the facing
         * that 0x459c70 (side_update) snapped to +0x500 earlier this frame, so a turn reverses him at full speed. Up / down only move the
         * camera (p+4, 0x459db3); jump, attack, duck work as anywhere. */
        int key = allow && ((in->left && p->side_l) || (in->right && p->side_r));
        if (key) {
            p->ramp_target = P_WALK_SPEED;
            if (p->ramp_phase == 0 || p->ramp_phase == 3) { p->ramp_phase = 1; p->ramp_t = sqrtf(p->speed / P_WALK_SPEED) * acc_T; }   /* 0x45ad30: 0/3 -> 1 */
        } else if (!allow && !p->look) { p->ramp_phase = 0; p->speed = 0; }   /* flag 2 off: 0x45a1f0 + phase 0 (0x45b2b8) */
        else if (p->ramp_phase == 1 || p->ramp_phase == 2) { p->ramp_phase = 3; p->ramp_t = 0; p->ramp_v0 = p->speed; }
    } else if (len > 0) {
        ix /= len; iz /= len;
        float cs = cosf(cam_yaw), sn = sinf(cam_yaw);
        /* camera forward = (sin yaw, 0, cos yaw); camera right (right-handed, +x left when looking +z) = (-cos yaw, 0, sin yaw) */
        float wx = iz * sn + ix * -cs, wz = iz * cs + ix * sn;
        float want = atan2f(wx, wz);
        float d = want - p->yaw; while (d > 3.14159265f) d -= 6.2831853f; while (d < -3.14159265f) d += 6.2831853f;
        /* 0x45a4b0: turning slows down, target = (dot(new, old) + 1) / 2 * max; facing blends toward the wanted direction */
        p->ramp_target = (cosf(d) + 1.0f) * 0.5f * P_WALK_SPEED * stick;   /* * min(|stick|, 1) */
        p->yaw += d * (1.0f - powf(1.0f - P_TURN_BLEND * stick, dt * P_REF_FPS));
        if (p->ramp_phase == 0 || p->ramp_phase == 3) { p->ramp_phase = 1; p->ramp_t = sqrtf(p->speed / P_WALK_SPEED) * acc_T; }   /* 0x45ad30: 0/3 -> 1 */
    } else if (!allow && !p->look) { p->ramp_phase = 0; p->speed = 0; }   /* state 3 has no lock: the Mover runs out (0.1 s) as when the keys are let go */
    else if (p->ramp_phase == 1 || p->ramp_phase == 2) { p->ramp_phase = 3; p->ramp_t = 0; p->ramp_v0 = p->speed; }                  /* 2 -> 3 */
    /* RampA's direction 0x45a850 (docs/PERSO_MOVE.md 6.4): normally the facing M+0x10; on slippery ground (and a target >= 0,
     * always here) it keeps k of the old walking direction per frame, k = clamp(|v| / P+0x1c, 0, 0.95) * P+0x3c with |v| =
     * the Mover's speed of the previous frame (M+0xe0): dir = k normalize(dir) + (1 - k) facing, not normalised again
     * (0x45a9ad), so a reversal on ice first slows him down. Per frame in the original; k^(60 dt) here. No shipped level has
     * a slippery floor (no texture group with ground type 1 in any .tex): WOODY_ICE=1 makes every floor slippery (testing). */
    {
        Vec3 f = { sinf(p->yaw), 0, cosf(p->yaw) };
        if (ice) {
            float v = sqrtf(p->vel.x * p->vel.x + p->vel.z * p->vel.z) / P_WALK_SPEED, l = sqrtf(p->move_dir.x * p->move_dir.x + p->move_dir.z * p->move_dir.z);
            if (v < 0) v = 0; if (v > 0.95f) v = 0.95f;                  /* 0x4a9c9c */
            float k = powf(v * P_ICE_TURN, dt * P_REF_FPS);
            Vec3 a = l > 1e-6f ? (Vec3){ p->move_dir.x / l, 0, p->move_dir.z / l } : f;
            p->move_dir = (Vec3){ a.x * k + f.x * (1.0f - k), 0, a.z * k + f.z * (1.0f - k) };
        } else p->move_dir = f;
    }
    /* Ramp tick (0x4671d0); before it 0x45ae50: after a wall contact in the last sweep (Perso+0x2e0) 0x4672d0(RampA, 0.25) moves the
     * braking timer a quarter of the way to its end (per frame; normalised to 60 fps) */
    if (p->ramp_phase == 3 && p->wall_contact) p->ramp_t += (dec_T - p->ramp_t) * (1.0f - powf(0.75f, dt * P_REF_FPS));
    if (p->ramp_phase == 1) { p->ramp_t += dt; float k = p->ramp_t / acc_T; if (k >= 1.0f) { k = 1.0f; p->ramp_phase = 2; } p->speed = k * k * p->ramp_target; }
    else if (p->ramp_phase == 2) p->speed = p->ramp_target;
    else if (p->ramp_phase == 3) { p->ramp_t += dt; float k = p->ramp_t / dec_T; if (k >= 1.0f) { k = 1.0f; p->ramp_phase = 0; } p->speed = p->ramp_v0 * (1.0f - k * k); }
    else p->speed = 0;
    /* sliding, 0x45aa60 (docs/PERSO_FRAME.md 2.3, PERSO_MOVE.md 6.6): n = normalize(M+0xd0); on the ground with n.y < 0.71 and not
     * sliding yet (M+0xdc == 0): RampB target = max (600), start accelerating (0x467130), M+0xdc = 1 and RampB.dir = the xz part of
     * n x (n x up) = n.y (n.x, n.z), downhill - set once, it does not follow the slope while he slides. Sliding and n.y >= 0.71 or
     * in the air: on the ground target 0, start decelerating (0x467180), M+0xdc = 0, RampA reset to the facing (0x467110);
     * in the air the slide stays on and Mover flag 4 is set (0x45ac18), which the total 0x45ae80 reads below. RampB is ticked
     * every frame (0x4671d0(dt, 1)) and its direction normalised. */
    int slide_air = 0;                                                     /* Mover+0x108 flag 4 */
    {
        Vec3 n = p->ground_n; float l = sqrtf(vdot(n, n));
        if (l > 0) n = (Vec3){ n.x / l, n.y / l, n.z / l };
        if (n.y < P_SLIDE_NY && p->on_ground) {
            if (!p->sliding) {
                float v = p->slide_speed < 0 ? 0 : p->slide_speed;         /* 0x467130: the timer resumes at the current speed */
                v /= P_SLIDE_SPEED; if (v < 0) v = 0;
                p->slide_target = P_SLIDE_SPEED; p->slide_t1 = sqrtf(v) * P_SLIDE_TIME; p->slide_phase = 1;
                p->sliding = 1; p->slide_dir = (Vec3){ n.y * n.x, 0, n.y * n.z };
            }
        } else if (p->sliding) {
            if (p->on_ground) {
                float v = p->slide_speed < 0 ? 0 : p->slide_speed;         /* 0x467180 */
                v = (P_SLIDE_SPEED - v) / P_SLIDE_SPEED; if (v < 0) v = 0;
                p->slide_target = 0; p->slide_t2 = sqrtf(v) * P_SLIDE_TIME; p->slide_phase = 3; p->sliding = 0;
                /* RampA 0x467110 + dir = M+0x10: its phase 0 gives v = target at the next tick, the Mover phase (M+0xc) is kept,
                 * so a walk in progress goes on at its target speed and a braking one stops (the port has one phase for both) */
                if (p->ramp_phase == 1) p->ramp_phase = 2;
                else if (p->ramp_phase == 3) { p->ramp_phase = 0; p->speed = 0; }
            } else slide_air = 1;
        }
        if (p->slide_target < 0) p->slide_speed = p->slide_target;          /* 0x4671d0(dt, useMax = 1) */
        else if (p->slide_phase == 1) {
            p->slide_t1 += dt;
            if (p->slide_t1 >= P_SLIDE_TIME) { p->slide_phase = 2; p->slide_speed = p->slide_target; }
            else { float k = p->slide_t1 / P_SLIDE_TIME; p->slide_speed = k * k * p->slide_target; }
        } else if (p->slide_phase == 3) {
            p->slide_t2 += dt; float k = 1.0f;
            if (p->slide_t2 >= P_SLIDE_TIME) p->slide_phase = 0; else k = p->slide_t2 / P_SLIDE_TIME;
            p->slide_speed = P_SLIDE_SPEED - k * k * P_SLIDE_SPEED;
        } else p->slide_speed = p->slide_target;
        l = sqrtf(vdot(p->slide_dir, p->slide_dir));
        if (l > 0) p->slide_dir = (Vec3){ p->slide_dir.x / l, p->slide_dir.y / l, p->slide_dir.z / l };
    }
    /* vertical motion comes from the Jumper; air control is the unchanged Mover (docs/PERSO_JUMP.md 1.4) */
    if (p->dead_kind == 7) jumper_reset(&p->jumper);                       /* 0x4649bf: the water death never falls further */
    if (p->nograv_t > 0) { p->nograv_t -= dt; p->jumper.dy = 0; if (p->dead_kind == 1 && p->nograv_t <= 0) p->dead_cam_req = 1; }
    else if (p->atk != 6 && p->atk != 7) jumper_update(&p->jumper, in->jump, input, p->on_ground, p->pos.y - p->floor_y, dt);
    if (p->jumper.open_window) { p->jumper.open_window = 0; if (p->air_win < 0.5f) p->air_win = 0.5f; if (p->am_win < 0.5f) p->am_win = 0.5f; }   /* 0x463002: 0x457560(0.5) + 0x465e00(0.5) */
    /* displacement this frame: the attack's own, or Mover + Jumper; then disp.y += dt * (+0x244) */
    disp = p->use_atk_disp ? p->atk_disp : (Vec3){ p->move_dir.x * p->speed * dt, p->jumper.dy, p->move_dir.z * p->speed * dt };
    int hlock = p->move_lock > 0;                                          /* 0x44bc16: ANY LockMove (+0x238 > 0: ducking, hard landing, special attack, bomb pick-up/throw,
                                                                            * message 30, pecks) zeroes the horizontal vector h = walk + slide (RampB) + knockback (RampC); the
                                                                            * jumper's vertical and the attack's own displacement (+0x5bc) are not touched (docs/PERSO_DUCK.md 2.2) */
    if (hlock && !p->use_atk_disp) disp.x = disp.z = 0;
    if (p->push_t > 0 || p->push_speed > 0) {                              /* RampC 0x45acb0: 500 u/s, 0.1 s up, 0.5 s out */
        if (p->push_t > 0) { p->push_t -= dt; p->push_speed += 500.0f / 0.1f * dt; if (p->push_speed > 500.0f) p->push_speed = 500.0f; }
        else { p->push_speed -= 500.0f / 0.5f * dt; if (p->push_speed < 0) p->push_speed = 0; }
        if (!p->use_atk_disp && !hlock && !slide_air) { disp.x += p->push_dir.x * p->push_speed * dt; disp.z += p->push_dir.z * p->push_speed * dt; }
    }
    /* the total 0x45ae80: walk + slide + knockback; with flag 4 (sliding, in the air) only the walk vector a, times the cosine
     * between it and the slide vector b clamped to >= 0 (0x440070 = a.b / |a||b|; 0/0 = NaN fails the test and gives 0), so a
     * slide that carries him off an edge stops dead in the air and goes on (decelerating) when he lands */
    if (!p->use_atk_disp && !hlock) {
        if (slide_air) {
            float bx = p->slide_dir.x * p->slide_speed, bz = p->slide_dir.z * p->slide_speed;
            float la = sqrtf(disp.x * disp.x + disp.z * disp.z), lb = sqrtf(bx * bx + bz * bz);
            float c = la > 0 && lb > 0 ? (disp.x * bx + disp.z * bz) / (la * lb) : 0;
            if (c < 0) c = 0;
            disp.x *= c; disp.z *= c;
        } else if (p->slide_speed != 0) { disp.x += p->slide_dir.x * p->slide_speed * dt; disp.z += p->slide_dir.z * p->slide_speed * dt; }
    }
    disp.y += dt * p->vy_corr;
    if (p->side_on) {                                                      /* 0x44bcd8 -> 0x459eb0, the last step of Perso_Move (so not in the states 1/4/5/7/8) */
        float m = sqrtf(disp.x * disp.x + disp.y * disp.y + disp.z * disp.z);   /* |disp|, 3D (the jumper's dy counts) */
        float e = p->side_n.x * p->pos.x + p->side_n.y * p->pos.y + p->side_n.z * p->pos.z + p->side_pd;   /* signed distance of the feet (+0x1f4) */
        if (e > m) e = m; if (e < -m) e = -m;                              /* 0x459f38 / 0x459f53: at most |disp| per frame */
        disp.x -= e * p->side_n.x; disp.y -= e * p->side_n.y; disp.z -= e * p->side_n.z;   /* 0x459faf: disp += -e n */
    }
    }
    p->vel = (Vec3){ disp.x / dt, disp.y / dt, disp.z / dt };
    if (disp.y < 0) p->jumper.fallen -= disp.y;                                      /* 0x44b914: fallen height accumulates */

    const Instance *hit_inst = NULL; const InsNode *hit_node = NULL;
    Vec3 old_pos = p->pos;
    const float body_h = player_body_height(p);
    move_collide(p, &disp, dt, racing, &hit_inst, &hit_node);
    if (racing) race_check_crash(p, old_pos, disp, body_h);
    crush_test(p);                                                        /* 0x44b87c: after MoveCollide (and 0x4567f0 in state 1) */
    check_steep(p);                                                       /* 0x44b2e0, after the collision and before Orient */
    if (racing) {                                                         /* 0x44bd30: +0x210 = +0x210 * 0.9 + M+0xd0 * 0.1 per frame (normalised to 60 fps), not renormalised */
        float k = 1.0f - powf(0.9f, dt * P_REF_FPS); Vec3 n = p->race_floor_n;
        p->race_upf = (Vec3){ p->race_upf.x + (n.x - p->race_upf.x) * k, p->race_upf.y + (n.y - p->race_upf.y) * k, p->race_upf.z + (n.z - p->race_upf.z) * k };
    }
    player_apply_transform(p);
    /* animations, Perso_AnimState 0x463e60 (docs/PERSO_MOVE.md 4.3, PERSO_JUMP.md 4): attack sub-state first, then
     * ground by Mover phase (0x463f40) or air by Jumper state (0x4642f0), the idle variations 0x59/0x5a by 0x464500. */
    {
        static const int atk_anim[12] = { -1, 0xb, 0xb, 0xc, 0xc, -1, 0xd, 0xe, 0xf, 0x10, 0x11, 0x12 };
        int js = p->jumper.state, want = p->lanim; float rate = 1.0f;
        int landing = (p->lanim == 8 || p->lanim == 0xa) && p->lanim_sub == 0;
        /* 0x464630 (state 0): nothing at all while +0x5b4 (attack) or +0x694 (ducking) is set - the idle count neither runs nor
         * resets. Otherwise 0x463f40 on the ground answers "idle" only in Mover phase 0, and then the count 0x464500 runs, also
         * under a hit or the special attack (their anims outrank its 0 / 0x59 / 0x5a); anything else resets it (0x46467e), and so
         * does a held action key 0,3,2,1,6,4,10,5 (0x44cc30) or +0x550 = the root position in use (0x464690). */
        int idle_w = 0;
        if (!p->dead_kind && !racing && !p->look && !p->idle_hold) {
            if (p->state6) idle_reset(p);                                  /* state 6 (0x4646b0) has no idle variations */
            else if (!p->atk && !p->duck) {
                if (js == 2 && p->ramp_phase == 0) idle_w = idle_anim(p, dt); else idle_reset(p);
                if (in->forward || in->back || in->left || in->right || in->jump || in->action || p->use_root) idle_reset(p);
            }
        }
        if (p->dead_kind && p->race_char) {                                /* 0x464a00, table 0x464b48: every frame on both controllers, kinds 4/5 (and 9) request nothing */
            int k = p->dead_kind; want = k == 8 ? 0x71 : k == 2 ? 0x72 : k == 6 ? 0x73 : k == 3 ? 0x74 : k == 1 ? 0x75 : k == 7 ? 0x77 : p->lanim;
            if (k == 8 || k == 2 || k == 6 || k == 3 || k == 1 || k == 7) board_request(p, k == 1 ? 0x76 : want);   /* kind 1: 0x76 (.ins 39, one-shot) on the board, 0x75 (39 -> 40 loop) on the rider */
            else board_tick(p);
            /* kind 1: 0x41fb50(cam, inst.pos + (0,100,0), p) every frame while T - dt <= L(0x75) (0x464ad4), not only when the hang
             * ends: the camera sets off towards the point above him at 100 u/s at once. He hangs still, so asking on the first frame
             * (and again when the hang ends, as for Woody) is the same travel; asking every frame would restart the port's transition
             * with t = 0 each time and freeze it */
            if (k == 1 && p->dead_T <= dt) p->dead_cam_req = 1;
        }
        else if (racing) want = -2;                                        /* 0x464c20 below */
        else if (p->dead_kind) {                                           /* 0x464790 */
            int k = p->dead_kind; want = p->lanim;
            if (k == 1) want = p->dead_T <= anim_len(p, 0x2f, 0) ? 0x2f : 9;
            else if (k == 2 || k == 9) want = 0x30;
            else if (k == 6) want = 0x2b; else if (k == 7) want = 0x2a; else if (k == 8) want = 0x2e;
            else if (p->dead_T <= dt) { p->dead_ground = p->on_ground; want = p->on_ground ? (p->duck == 2 ? 0x2c : 0x26) : 0x25; }   /* 0x4647e7: lying down -> 0x2c, once */
            else if (!(p->dead_ground && p->duck == 2) && p->dead_T >= anim_len(p, p->dead_ground ? 0x26 : 0x25, 0)) want = 0x29;
        }
        else if (p->hit_anim_t > 0) want = p->hit_anim;
        else if (p->special_st) want = 0x13;                               /* priority 5500: only deaths and scripted actions (6000) beat it */
        else if (p->duck) want = p->duck_anim;                             /* 0x464630 does nothing while he ducks: no landing, idle or carry anims */
        else if (p->look) want = 0;                                        /* state 3: 0x463e77 requests anim 0 (he is faded out anyway) */
        else if (p->state6) {                                              /* state 6: 0x4646b0 (docs/BOMB_CARRY.md 1.4); also without a bomb after the look-around bug */
            if (js == 2 || p->on_ground) {
                if (p->bsub <= 1) want = 0x45;                             /* pick-up */
                else if (p->bsub == 4) want = 0x43;                        /* ground throw */
                else if (p->bsub <= 3 && p->ramp_phase == 1) want = 0x41;
                else if (p->bsub <= 3 && p->ramp_phase == 2) { want = 0x42; rate = p->speed / P_WALK_SPEED; if (rate < 0.5f) rate = 0.5f; if (rate > 1.0f) rate = 1.0f; }
                else want = 0x40;
            }
            else if (p->bsub == 6) want = 0x44;                            /* air throw */
            else if (js == 0 || js == 1 || js == 7) want = 0x47;
            else if (js == 5) want = 0x4b;
            else if (js == 3 || js == 4) want = p->jumper.fell_off ? 0x49 : 0x4a;
            else if (js == 6) want = p->jumper.hard_fall ? 0x4c : 0x4a;
        }
        else if (p->throw_hold > 0) { p->throw_hold -= dt; want = p->lanim; }   /* after the release: 0x43 / 0x44 play out */
        else if (p->atk) { if (atk_anim[p->atk] >= 0) want = atk_anim[p->atk]; }
        else if (js == 2) {
            if (p->ramp_phase == 1) want = 2;
            else if (p->ramp_phase == 2) { want = 3; rate = p->speed / P_WALK_SPEED; if (rate < 0.5f) rate = 0.5f; if (rate > 1.0f) rate = 1.0f; }   /* 0x436c20 */
            else if (p->ramp_phase == 0) { if (!landing) want = idle_w; }  /* the landing (prio 1500) outranks idle (1100) */
        }
        else if (js == 0 || js == 1) want = 4;
        else if (js == 7) want = 5;
        else if (js == 3 || js == 4) { if (p->jumper.fell_off) want = 7; else if (p->jumper.short_hop) want = 6; }
        else if (js == 5) want = 9;
        else if (js == 6) {
            if (p->jumper.hard_fall) { want = 0xa; lock_move(p, anim_len(p, 0xa, 0)); game_bubble(p->inst, 1, 2.0f, 180.0f, 50.0f, NULL); }   /* hard landing blocks movement; 0x464470: he curses */
            else if (p->ramp_phase != 2) want = 8;
            if (p->ground_kind == 2) game_land_dust((Vec3){ p->pos.x, p->pos.y + 30.0f, p->pos.z }, p->ground_n);   /* 0x464486: 0x476140(&pos + (0,30,0), &normal, 3, 0.25, 1.5) */
        }
        if (want == -2) { race_anims(p, in, dt); if (p->lanim >= 0) anim_request(p, p->lanim, 1.0f); board_tick(p); }   /* the controller Ticks run every frame (0x463e60): they walk the chain 0x5d -> anim 0 etc. */
        else anim_request(p, want, rate);
        carry_frame(p, dt);                                                /* 0x463530 (frame step 16) */
        /* footsteps (docs/FOOTSTEPS.md): in the walk cycle (logical animation 3) 0x463f40 puts a foot down when the
         * fraction of the cycle passes 0.38 (0x4ab278) and 0.9 (0x4a94b8) and calls the effect 0x47cba0 for it. */
        {
            const Model *pm = p->inst->model; float u = -1.0f;
            if (p->lanim == 3 && p->on_ground && (uint32_t)p->inst->anim < pm->nanims) {
                float dur = pm->anims[p->inst->anim].duration_s;
                if (dur > 0) { u = fmodf(p->inst->anim_time / dur, 1.0f); if (u < 0) u += 1.0f; }
            }
            if (u >= 0 && p->step_u >= 0) {
                Vec3 f = { sinf(p->yaw), 0, cosf(p->yaw) };                     /* the walk direction is the facing (Mover+0x10) */
                int kind = p->ground_kind == 2 ? 3 : 2;                         /* 0x464231: dust ground gets kind 3 */
                if (phase_passed(p->step_u, u, 0.38f)) game_footstep(p->pos, p->ground_n, f, 1, kind);   /* 0x464244: the right foot */
                if (phase_passed(p->step_u, u, 0.90f)) game_footstep(p->pos, p->ground_n, f, 0, kind);   /* 0x464289: the left */
            }
            p->step_u = u;
        }
    }

    /* world_collision events: standing on a press node with type code 1 (0x436f00, docs/EVENTS.md 3.2/3.3).
     * Moving from collision A straight onto B sends only Press(B), as in the original. */
    if (vm) {
        uint32_t col = 0xffffffffu;
        if (p->on_ground && hit_node && hit_node->kind == 1 && hit_node->type_code == 1) {
            const Model *hm = hit_inst->model; uint32_t k = hm->nvolume_nodes + hit_node->sub_index;
            if (hm->ncollision_ids && k < hit_inst->nids) col = hit_inst->ids[k];
        }
        if (col != 0xffffffffu) {
            if (p->cur_col == col) eko_col_perso_in(vm, col, p->inst->id);
            else { eko_col_perso_press(vm, col, p->inst->id); p->cur_col = col; p->events_sent++; printf("  COL press 0x%x (inst %u)\n", col, hit_inst->index); }
        } else if (p->cur_col != 0xffffffffu) {
            eko_col_perso_unpress(vm, p->cur_col, p->inst->id); p->events_sent++; printf("  COL unpress 0x%x\n", p->cur_col); p->cur_col = 0xffffffffu;
        }
    }
    perso_mask200(p, vm, 0);                                              /* msgmask 0x200 = onGround +0x22c */

    player_volumes(p, vm);
}

/* ---- follow camera, mode 1 (docs/CAMERA.md 0.1 / 3: 0x424760 -> 0x422790 -> 0x4231e0) ------------------- */
/* the camera's line of sight: ray 0x4359b0 (world polygons, then instance press nodes) filtered by 0x422140, which lets
 * the hit through when it is an instance of category 7 (type word 0x27 = class 80, the storm with shelter zones, 0x451b28).
 * The hit 0x4359b0 reports is the instance one whenever an instance polygon lies on the segment (the last one tested,
 * ray_instances), even behind a nearer wall */
static int cam_ray_blocked(const Player *p, Vec3 a, Vec3 b)
{
    float fw = gel_ray_frac(p->gel, a, b), tw = gel_ray_front(p->gel, a, b); InsRayHit h;
    if (!ray_instances(p, NULL, a, b, tw < 1.0f ? tw : 2.0f, &h)) return fw <= 1.0f;
    return h.kind != 2 || h.in->type != 80;                              /* 0x422140: only hit kind 2 on category 7 lets it through */
}
/* state 2 "FIND" (0x423ab0): the target went out of sight, so the camera walks the trail the target left behind.
 * The trail starts as {P, Tprev, T} (0x4229de); whenever the last crumb loses sight of T, the point where the target was a
 * frame ago becomes a new crumb, so the crumbs sit at the corners the player went round. The tip always follows T.
 * The camera moves along it at 0.04 of a segment per frame (not dt-scaled in the original; normalised to 60 fps here),
 * whatever the segment's length, and goes back to the normal state as soon as it sees T again. */
static Vec3 camera_breadcrumbs(Player *p, Vec3 T, float dt)
{
    Vec3 *pad = p->cam_pad;
    if (cam_ray_blocked(p, pad[p->cam_n - 1], T)) {
        /* 0x423b62: the target cannot even see its own previous position. The original gives up (state 0, P = T + (1,1,100))
         * but carries on below and overwrites P, so the only effect is that the next frame starts a fresh trail. */
        if (cam_ray_blocked(p, p->cam_tprev, T)) p->cam_state = 0;
        if (p->cam_n < 99) pad[p->cam_n++] = p->cam_tprev;          /* no bound in the original: 100 crumbs end at the counter +0x7a0 */
    }
    pad[p->cam_n] = T;
    if (p->cam_u >= 1.0f) { p->cam_u = 0; p->cam_seg++; }
    p->cam_u += 0.04f * dt * P_REF_FPS; if (p->cam_u > 1.0f) p->cam_u = 1.0f;   /* 0x4aa1cc */
    if (p->cam_seg >= p->cam_n) { p->cam_seg = p->cam_n - 1; p->cam_u = 1.0f; }
    Vec3 a = pad[p->cam_seg], b = pad[p->cam_seg + 1], P = { a.x + (b.x - a.x) * p->cam_u, a.y + (b.y - a.y) * p->cam_u, a.z + (b.z - a.z) * p->cam_u };
    if (!cam_ray_blocked(p, P, T)) p->cam_state = 0;
    return P;
}
/* 0x439c50(out, P, -1, N, 35.0, 0): the path P -> N in n equal steps; after every step the sphere of radius [0x4b3118] = 40
 * (set by 0x434820) is pushed out of the world and the instance press nodes by 0x407340, on all three axes. n = floor(|N-P| / 35)
 * + 1, but it reaches the loop through an inline fistp of n + 0.5 into [0x5ac8ac] (not the truncating _ftol 0x499580 the other
 * sweeps use), so under the FPU's round-to-nearest-even an odd n becomes n + 1 while the step stays |N-P| / n: the sweep goes one
 * step PAST N, and a move under 35 units is swept twice (verified live, tools/wverify.py --probe fpu: control word 0x007F =
 * nearest-even, 24-bit precision; W1A standing still: fistp 1.5 -> 2). Returns whether any step touched something ([0x4c4bd0]). */
static int camera_sweep(const Player *p, Vec3 P, Vec3 N, Vec3 *out)
{
    Vec3 d = vsub(N, P); float k = floorf(sqrtf(vdot(d, d)) / 35.0f) + 1.0f; int n = (int)nearbyintf(k + 0.5f), hit = 0;
    d.x /= k; d.y /= k; d.z /= k; *out = P;
    for (; n > 0; n--) {
        out->x += d.x; out->y += d.y; out->z += d.z;
        Vec3 push = player_sphere_push(p, NULL, *out, CAM_RADIUS);
        if (g_sphere_contact) { hit = 1; out->x += push.x; out->y += push.y; out->z += push.z; }
    }
    return hit;
}
/* Center_BehindArc 0x423ed0, only in behind mode (F = the player's facing, the camera is pulled to T - F * dist):
 * (1) the player stands still (|T - Tprev|xz < 0.5), outside the reset loop (+0x9c8) and the camera is within 10 of the
 *     distance: a step that would carry it across the line straight behind him (((T - P) x F).y changes sign) is dropped;
 * (2) a step that crosses the ring dist +- 5 around T (from inside dist - 5 to outside dist + 5 or the other way) is cut
 *     where it meets the circle of radius dist: |P + s*move - T|xz = dist, the root with the smaller |s|. */
static int g_cam_resetting;                                              /* C+0x9c8: inside the 100 pre-simulation steps of 0x424200 */
static void camera_behind_arc(const Player *p, Vec3 T, Vec3 P, Vec3 F, Vec3 *mv)
{
    float dist = p->cam_dist, ax = T.x - P.x, az = T.z - P.z, d0 = sqrtf(ax * ax + az * az);
    float ux = T.x - p->cam_tprev.x, uz = T.z - p->cam_tprev.z;
    if (sqrtf(ux * ux + uz * uz) < 0.5f && !g_cam_resetting && fabsf(d0 - dist) < 10.0f) {
        float bx = T.x - (P.x + mv->x), bz = T.z - (P.z + mv->z), ca = az * F.x - ax * F.z, cb = bz * F.x - bx * F.z;
        if ((ca < 0 && cb > 0) || (ca > 0 && cb < 0)) *mv = (Vec3){ 0, 0, 0 };
    }
    float bx = T.x - (P.x + mv->x), bz = T.z - (P.z + mv->z), d1 = sqrtf(bx * bx + bz * bz), lo = dist - 5.0f, hi = dist + 5.0f;
    if ((d0 < lo && hi < d1) || (d0 > hi && d1 < lo)) {
        float A = mv->x * mv->x + mv->z * mv->z, B = 2.0f * ((P.x - T.x) * mv->x + (P.z - T.z) * mv->z);
        float C = (P.x - T.x) * (P.x - T.x) + (P.z - T.z) * (P.z - T.z) - dist * dist, D = B * B - 4.0f * A * C;
        if (A > 1e-12f && D >= 0) {                                      /* a crossing always has a root; the guard is the port's */
            float sq = sqrtf(D), s1 = (-B - sq) / (2.0f * A), s2 = (sq - B) / (2.0f * A), s = fabsf(s2) <= fabsf(s1) ? s2 : s1;
            mv->x *= s; mv->y *= s; mv->z *= s;
        }
    }
}
static void camera_step(Player *p, float dt, int behind, int quick, int collide)
{
    Vec3 look = { sinf(p->yaw), 0, cosf(p->yaw) };
    Vec3 T = { p->pos.x, p->pos.y + CAM_TARGET_Y, p->pos.z }, L = { p->pos.x, p->pos.y + CAM_LOOK_Y, p->pos.z };
    Vec3 P = p->cam_pos, mv = { 0, 0, 0 };
    int js = p->jumper.state, rising = js == 0 || js == 1 || js == 7, falling = js == 3 || js == 4;
    if (!collide) p->cam_state = 0;
    else if (p->cam_state != 2 && cam_ray_blocked(p, P, T)) {           /* 0x4229b8: the target is hidden at the start of the frame */
        p->cam_state = 2; p->cam_n = 2; p->cam_seg = 0; p->cam_u = 0;
        p->cam_pad[0] = P; p->cam_pad[1] = p->cam_tprev; p->cam_pad[2] = T;
        if (wenv("WOODY_CAMLOG")) {
            float fi = 2.0f; Vec3 n; const Instance *hi = NULL; player_ray_instances(p, NULL, P, T, &fi, &n, &hi);
            printf("  CAM target hidden: breadcrumbs from %.0f %.0f %.0f (world %.2f, instance %d model %d type %d at %.2f)\n", P.x, P.y, P.z, gel_ray_frac(p->gel, P, T),
                   hi ? (int)hi->index : -1, hi ? (int)(hi->model - p->ins->models) : -1, hi ? hi->type : -1, fi);
        }
    }
    if (p->cam_state == 2) {                                            /* no distance, height or collision step in this state */
        int n0 = p->cam_n;
        p->cam_pos = camera_breadcrumbs(p, T, dt); p->cam_tprev = T;
        if (wenv("WOODY_CAMLOG") && (p->cam_n != n0 || p->cam_state != 2)) printf("  CAM crumbs %d seg %d u %.2f%s\n", p->cam_n, p->cam_seg, p->cam_u, p->cam_state != 2 ? " -> target in sight" : "");
        goto drop;
    }
    if (behind) {
        float k = dt * (quick ? 7.0f : 3.0f);
        mv.x = (T.x - look.x * p->cam_dist - P.x) * k; mv.z = (T.z - look.z * p->cam_dist - P.z) * k;
    } else {                                                            /* 0x4242d0: C+0x7e0 = C+0x7e4 = the distance of message 680 (400), dead zone 100 */
        float vx = T.x - P.x, vz = T.z - P.z, d = sqrtf(vx * vx + vz * vz), dmax = p->cam_dist, dmin = dmax - 100.0f;
        if (d > 1e-3f) {
            vx /= d; vz /= d;
            float lx = L.x - P.x, lz = L.z - P.z, ll = sqrtf(lx * lx + lz * lz);
            float k = (ll > 1e-3f && (lx * -look.x + lz * -look.z) / ll > 0.7f) ? 10.0f : 1.0f;   /* player walks toward the camera */
            if (d < dmin) { float st = (dmax / d) * dt * k * 66.6667f; if (d + st > dmin) st = dmin - d; mv.x = -vx * st; mv.z = -vz * st; }
            else if (d > dmax) { float st = (d / dmax) * dt * 433.333f; if (d - st < dmax) st = d - dmax; mv.x = vx * st; mv.z = vz * st; }
        }
        if (p->cam_orbit_x != 0) {                                      /* PORT EXTRA: the right stick turns the camera round T (stick right = view turns
                                                                         * right); a move like any other, so the sweep and the veto below still apply */
            float a = p->cam_orbit_x * CAM_ORBIT_RATE * dt, c = cosf(a), s = sinf(a), ox = P.x + mv.x - T.x, oz = P.z + mv.z - T.z;
            mv.x = T.x + ox * c - oz * s - P.x; mv.z = T.z + ox * s + oz * c - P.z;
        }
    }
    if (behind) camera_behind_arc(p, T, P, look, &mv);
    if (rising) mv.y = (P.y - T.y < 300.0f) ? T.y - p->cam_tprev.y : 0;
    else mv.y = (T.y + p->cam_height + p->cam_lift - P.y) * 6.0f * dt;
    Vec3 N = { P.x + mv.x, P.y + mv.y, P.z + mv.z };
    if (collide) {
        /* Center_Collide 0x422e30: the sphere r = 40 swept from P to P + move (0x439c50); on contact the correction corr = where
         * the sweep ended - (P + move). Then the veto 0x423a40: a step after which the camera no longer sees T is refused -
         * move = 0, and SubCenter 0x422f10 sweeps again with that zero move, so corr keeps only the push-out of the spot the
         * camera stands on (0 when it touches nothing). P += move + corr. The out-of-world tests around it (0x40aba0 = -1)
         * can never fire: the kd descent 0x40ab60 always ends in a leaf (docs/CAMERA.md 3.6). */
        Vec3 out;
        if (camera_sweep(p, P, N, &out)) N = out;
        if (cam_ray_blocked(p, N, T)) {
            if (wenv("WOODY_CAMLOG") && wenv("WOODY_CAMLOG")[0] == '2') printf("  CAM blind move refused at %.0f %.0f %.0f\n", N.x, N.y, N.z);
            N = camera_sweep(p, P, P, &out) ? out : P;
        }
    }
    p->cam_pos = N; p->cam_tprev = T;
drop:                                                                   /* the look point (0x4223b0) runs in every state */
    if (rising || falling) { p->cam_drop += 450.0f * dt; if (p->cam_drop > 150.0f) p->cam_drop = 150.0f; }
    else p->cam_drop *= powf(0.94f, dt * P_REF_FPS);
}

/* SetMode(0, 0) = message 500 "camera behind the player" (0x41e450 -> 0x4247f0): P = pos - look + (0,100,0), then
 * one step of 0.1 s and 100 of 0.04 s in behind mode, so the camera settles on its resting point straight away. */
void player_camera_reset(Player *p)
{
    p->cam_init = 1; p->cam_state = 0; p->cam_tprev = (Vec3){ p->pos.x, p->pos.y + CAM_TARGET_Y, p->pos.z };   /* 0x422350: state 0 */
    p->cam_pos = (Vec3){ p->pos.x - sinf(p->yaw), p->pos.y + 100.0f, p->pos.z - cosf(p->yaw) };
    camera_step(p, 0.1f, 1, 0, 1);
    g_cam_resetting = 1; for (int i = 0; i < 100; i++) camera_step(p, 0.04f, 1, 0, 1); g_cam_resetting = 0;   /* 0x424200: Center_Step collides during the pre-simulation too */
}

void player_camera(Player *p, FreeCamera *cam, float dt, int behind_key)
{
    if (!p->cam_init) player_camera_reset(p);
    /* action 0xa: a tap pulls the camera behind the player for 0.5 s at 7*dt, holding it at 3*dt */
    if (behind_key && !p->cam_behind_prev) p->cam_quick_t = 0.5f;
    p->cam_behind_prev = behind_key; if (p->cam_quick_t > 0) p->cam_quick_t -= dt;
    /* PORT EXTRA: right stick up / down lowers / raises the camera (stick up = look up), kept until "camera behind" */
    if (behind_key) p->cam_lift = 0;
    else { p->cam_lift += p->cam_orbit_y * CAM_LIFT_RATE * dt; if (p->cam_lift < CAM_LIFT_MIN) p->cam_lift = CAM_LIFT_MIN; if (p->cam_lift > CAM_LIFT_MAX) p->cam_lift = CAM_LIFT_MAX; }
    /* 0x4591ec: Perso states 1, 4 (climbing) and 8 force behind mode at the slow rate; the target is the root position during the climb-over */
    Vec3 keep = p->pos; if (p->use_root) p->pos = p->root_pos;
    camera_step(p, dt, behind_key || p->cam_quick_t > 0 || p->climb_sub || p->ride || (p->race_char && !p->dead_kind), p->cam_quick_t > 0, !p->ride);   /* port choice: no camera collision during the ride, the rocket flies through walls and the veto would strand the camera */
    cam->pos = p->cam_pos; cam->fov_deg = CAM_FOV_Y;
    Vec3 to = { p->pos.x - cam->pos.x, p->pos.y + CAM_LOOK_Y - p->cam_drop - cam->pos.y, p->pos.z - cam->pos.z };
    float h = sqrtf(to.x * to.x + to.z * to.z);
    cam->yaw = atan2f(to.x, to.z); cam->pitch = atan2f(to.y, h); p->cam_yaw = cam->yaw;
    p->pos = keep;
}

/* scripted Perso action (message 1040, 0x44dda0): only the effect on control is ported - the running attack is dropped
 * and the player stands still for t seconds (17 = walk into a door: the level change follows) */
static Quat q_unit(Quat q) { float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w); if (n < 1e-9f) return (Quat){ 0, 0, 0, 1 }; return (Quat){ q.x / n, q.y / n, q.z / n, q.w / n }; }
Quat q_slerp(Quat a, Quat b, float u)
{
    a = q_unit(a); b = q_unit(b); float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) { d = -d; b = (Quat){ -b.x, -b.y, -b.z, -b.w }; }
    float ka = 1 - u, kb = u; if (d < 0.9995f) { float th = acosf(d), s = sinf(th); ka = sinf((1 - u) * th) / s; kb = sinf(u * th) / s; }
    return q_unit((Quat){ a.x * ka + b.x * kb, a.y * ka + b.y * kb, a.z * ka + b.z * kb, a.w * ka + b.w * kb });
}
int player_mount(Player *p, Instance *obj)                              /* 0x465740 */
{
    if (!player_state_free(p) || !p->on_ground) return 0;               /* 0x465740: only in state 0 and on the ground */
    p->ride = obj; p->ride_state = 1; p->atk = 0; p->charge = 0; p->use_atk_disp = 0; p->has_target = 0; p->speed = 0; p->ramp_phase = 0; p->push_t = 0; p->att_inst = NULL;
    p->ride_p0 = p->pos; p->ride_q0 = p->ride_cur = p->inst->quat; p->ride_t = 0.7f; p->ride_jprev = p->ride_aprev = 1; p->lanim = -1;
    printf("  PLAYER mounts instance %u", obj->index), puts("");
    return 1;
}
void player_script_action(Player *p, int act, int have, Vec3 p0, Vec3 dir)
{
    if (p->dead_kind) return;                                       /* 0x44dda0 refuses state 2 (dead) */
    bomb_drop(p); p->throw_hold = 0; p->look = 0;                   /* SetState(5); out of state 3 without +0x268 (docs/PERSO_LOOK.md 4) */
    int lg = log_from_raw(act);                                     /* the action number is the raw .ins animation (docs/CINEMATIC.md 6) */
    if (lg < 0) { printf("  scripted action %d: no logical record, standing still", act), puts(""); player_script_hold(p, 2.0f); return; }
    p->atk = 0; p->charge = 0; p->use_atk_disp = 0; p->climb_sub = 0; p->use_root = 0; p->speed = 0; p->ramp_phase = 0; p->push_t = 0; p->push_speed = 0; slide_reset(p);
    if (have) { p->pos = p0; if (dir.x * dir.x + dir.z * dir.z > 1e-6f) p->yaw = atan2f(dir.x, dir.z); }   /* on P0 of the door vector (typecode 5), facing P1; no ground snap */
    jumper_reset(&p->jumper); p->on_ground = 1; p->floor_y = p->pos.y;
    if (have) p->ground_22c = 1;                                    /* 0x44dede: onGround = 1 only with a vector (msgmask 0x200) */
    p->script_act = act; p->script_log = lg; p->lanim = -1; anim_request(p, lg, 1.0f); p->script_total = p->script_t = anim_len(p, lg, 0); p->script_faded = 0; p->script_carry = NULL;   /* +0x554 = arg 3 (0x44dde1) */
    if (act == 18) p->fade_req = 2;   /* fade in 0.5 s on the first frame (0x44dc2b). The camera is NOT cut here: the tail of 0x44dda0
                                       * puts it on the animation's own camera track (message 1040 in main_engine.c) and a cut back to the
                                       * follow camera would undo that one frame later. Coming out of the door ends with 0x44e5a0, a 0.5 s
                                       * blend back to it - that is cam_end_req, raised when the action runs out. */
    player_apply_transform(p);
}
/* message 1041 [inst, act] (0x445481 -> 0x44e040(act, &inst.pos)); no shipped script sends it. Facing = the xz direction to `at`
 * (Mover_SetDir 0x459ff0, left alone when the distance is 0), ground snap 0x462990, the action's record requested after a Reset,
 * +0x53c = +0x538 = 0 (length 0), +0x554 = 0, +0x540 = act, SetState(5). Unlike 0x44dda0 it neither moves him, nor drops the side
 * view, nor leaves the volumes, nor cuts to a camera track. With the length 0 the next state-5 frame (0x44db50) ends it at once:
 * idle, and for anything but 17/18 the end of 0x44e290 = he stands where the action's root motion ends, facing -E.row2 */
void player_face_action(Player *p, int act, Vec3 at)
{
    if (p->dead_kind) return;
    int lg = log_from_raw(act); if (lg < 0) { printf("  1041: action %d has no logical record", act), puts(""); return; }
    float dx = at.x - p->pos.x, dz = at.z - p->pos.z;
    if (dx * dx + dz * dz > 0) p->yaw = atan2f(dx, dz);
    bomb_drop(p); p->throw_hold = 0; p->look = 0;                       /* SetState(5) */
    p->atk = 0; p->charge = 0; p->use_atk_disp = 0; p->climb_sub = 0; p->use_root = 0; p->speed = 0; p->ramp_phase = 0; p->push_t = 0; p->push_speed = 0; slide_reset(p);
    player_ground_snap(p);
    p->script_act = act; p->script_log = lg; p->lanim = -1; anim_request(p, lg, 1.0f); p->script_total = p->script_t = 0; p->script_faded = 0; p->script_carry = NULL;
    player_apply_transform(p);
}
/* 0x462990: the floor under feet + 43 (GetHeight 0x435650) becomes his height, onGround = 1, Jumper reset. The level start
 * (0x44a6a0, from the Game ctor 0x445850 and 0x445930) and every respawn / teleport (0x44a650) end with it, so the player
 * never starts in the air: without it the .ins position, which floats a few units above the floor, is a fall. */
void player_ground_snap(Player *p)
{
    int found; float gy = player_ground_query(p, p->inst, (Vec3){ p->pos.x, p->pos.y + P_PROBE_Y, p->pos.z }, &found);
    p->pos.y = found ? gy : p->pos.y + P_PROBE_Y;                      /* pos.y = [0x53a568] always: with nothing below it still holds the probe height
                                                                         * (traced: the W2B boss intro ends 53 under the floor, NotFound -> feet + 43,
                                                                         * and the next frame's step-up puts him on the floor 10 higher) */
    p->floor_y = p->pos.y; jumper_reset(&p->jumper); p->on_ground = 1; p->ring_a = 0; p->ring_ground_t = 1.0f; player_apply_transform(p);   /* +0x588 = 0, +0x580 = 1.0 */
}

void player_teleport(Player *p, Vec3 pos, int have_dir, Vec3 dir)       /* 0x44ce11 -> 0x44a650: SetPos + ground snap 0x462990, anim controllers reset, camera cut 0x458f90 */
{
    bomb_drop(p);
    for (uint32_t v = 0; v < p->nvol; v++) p->inside[v] = 0;
    if (!p->script_act) {                                               /* 0x44a650 does nothing in state 5 */
        p->pos = pos; if (have_dir && dir.x * dir.x + dir.z * dir.z > 1e-6f) p->yaw = atan2f(dir.x, dir.z);
        p->vel = (Vec3){ 0, 0, 0 }; p->speed = 0; p->ramp_phase = 0; p->att_inst = NULL; p->lanim = -1; p->step_u = -1.0f;
        p->board_lanim = -1;                                            /* 0x44a688: B->Reset() too */
        player_ground_snap(p);
    }
    /* 0x458f90 (the camera cut that sits outside that test) is in the message-26 handler: it has to run in script order, because
     * what the rest of the tick does with the camera has to win over it. The follow camera still seats itself one frame later,
     * on the position and facing a door action gives him (mode 1 of message 26 carries no direction), because SetMode(0, 0)
     * only clears cam_init and player_camera() re-seats on the next camera update - which runs after the tick. */
}
/* the Perso half of message 1088 (0x459960, first call; a repeat while +0x4ec is set only re-enters camera mode 5), docs/CAMERA_SCRIPT.md
 * 4.2.1. d = xzNormalize(B - A) of the marker (type 0, n 0), (1, 0, 0) when shorter than 0.01. */
void player_side_start(Player *p, Vec3 a, Vec3 d, int v)
{
    Vec3 from = p->pos;
    p->yaw = atan2f(d.x, d.z); p->move_dir = d;                        /* 0x459a48..0x459ad3: RampA dir M+0x34 = d, M+0x1c = M+0x10 = d: the facing */
    if (!p->script_act) {                                               /* 0x459ad8 -> 0x44a650(p, &A): nothing in state 5 */
        p->pos = a; p->lanim = -1; p->step_u = -1.0f; p->board_lanim = -1;   /* feet onto the marker's FIRST point, A->Reset (+0x494), B->Reset (+0x498) */
        player_ground_snap(p);                                          /* 0x44a678 */
    }
    player_ground_snap(p);                                              /* 0x459adf: 0x462990 again, outside the state-5 test */
    p->side_l = v == 1; p->side_r = v != 1;                             /* 0x459ae9: v == 1 -> +0x4ed (the left key walks along d), else +0x4ee */
    p->side_n = (Vec3){ -d.z, 0, d.x };                                 /* 0x459bbf..0x459c13: +0x4f0 = normalize(d x (0,1,0)) (0x41af10 = this x arg, 0x4239f0) */
    p->side_pd = -(p->side_n.x * a.x + p->side_n.z * a.z);              /* 0x459c09..0x459c4f: +0x4fc = -n.A */
    p->side_walk = d;                                                   /* 0x459c52: +0x500 = d */
    printf("  PLAYER side view: marker A %.0f %.0f %.0f d %.3f %.3f, v %d; feet %.0f %.0f %.0f -> %.0f %.0f %.0f%s\n", a.x, a.y, a.z, d.x, d.z, v,
           from.x, from.y, from.z, p->pos.x, p->pos.y, p->pos.z, p->script_act ? " (state 5: not moved)" : "");
}
void player_script_hold(Player *p, float t) { p->atk = 0; p->charge = 0; p->use_atk_disp = 0; lock_move(p, t); }

void player_free(Player *p) { g_rider = g_rider_board = NULL;   /* they point into the level's instances; the next level's load runs floor queries before player_update */
                              free(p->inside); free(p->vol_inst); free(p->vol_node); free(p->vol_id); }
