/* water.c - instance class 60, the water volume (docs/WATER.md). Size 0x158, vtable 0x4a9194: Init 0x473120 builds the
 * surface grid, Draw 0x4738c0 wobbles and draws it, Update 0x4747f0 drops wakes and drowns the player, handler 0x474a40
 * ignores 29/33/35. The script gives it its parameters with message 1506 right after SetTypeInstance 60; without them
 * the original only complains ("Vous n'avez pas initialisé les paramètres du volume d'eau").
 *
 * The model of a water volume (W2A model 25) is a box: mesh node, collision box, press node. The box itself is never
 * drawn and never collides (Init sets inst+8 |= 0x40); what the player sees is a grid laid over the top face of the mesh
 * node, drawn twice with the interior vertices circling in opposite directions. */
#include "plat.h"
#include <GL/gl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "water.h"
#include "player.h"
#include "hud.h"
#include "crtrand.h"

#define MAXV 400                                                   /* 0x4734ca: more grid vertices -> "Un volume d'eau a trop de face" */

typedef struct {
    Instance *inst;
    int ready, too_big;                                            /* +0x14c: 1506 has arrived; the grid did not fit */
    float cell, amp, alpha; int subdiv;                            /* +0x13c (then min'ed with both edges), +0x144, +0x148, +0x140 */
    int group;                                                     /* texture group of the first textured polygon (+0x110) */
    uint32_t node;                                                 /* the mesh node: the first entry of the model's mesh list (S+0x34) */
    Mat4 M;                                                        /* 0x42f7e0(0, 0, M, 1): that node posed at animation 0, time 0, in the world */
    int nc, nr;                                                    /* +0x114 vertices per row, +0x118 rows */
    float len1, len2;                                              /* +0x11c, +0x120: the two edges of the top face */
    Vec3 v[MAXV]; float uv[MAXV][2], ph[MAXV];                     /* +0xfc, +0x100, +0x10c (phase in 1/512 turns) */
    Vec3 centre;                                                   /* +0x130 */
    float wake_t;                                                  /* +0x154 */
} Water;

typedef struct { Vec3 pos, dir; float age, acc; int on; } Wake;   /* emitter 0x472ec0: 1 s, a mark every 0.1 s */
typedef struct { Vec3 pos; float age; int on; } Mark;             /* 0x473050: 0.5 s */

static Water g_w[16]; static int g_nw;
static Wake g_wake[64]; static Mark g_mark[512];
static float g_cos[512], g_spec[128];                              /* [0x5e823c]: cos(i 2pi/512), and at +0x900 0.5 cos^8 (0x402520) */

static void tables(void)
{
    if (g_cos[0] != 0) return;
    for (int i = 0; i < 512; i++) g_cos[i] = (float)cos(i * 0.012271846644580364);   /* 0x4a902c = 2 pi / 512 */
    for (int i = 0; i < 128; i++) g_spec[i] = (float)pow(g_cos[i], 8.0) * 0.5f;       /* i / 128 * 128 = i, ^8.0 (0x4a9018), * 0.5 */
}
static Vec3 vsub(Vec3 a, Vec3 b) { return (Vec3){ a.x - b.x, a.y - b.y, a.z - b.z }; }
static float vlen(Vec3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
static Water *water_of(const Instance *in) { for (int i = 0; i < g_nw; i++) if (g_w[i].inst == in) return &g_w[i]; return NULL; }

static const TexFile *g_tex;                                      /* the level's .tex: which texture group the surface takes */
void water_reset(const TexFile *tex) { g_nw = 0; g_tex = tex; memset(g_wake, 0, sizeof g_wake); memset(g_mark, 0, sizeof g_mark); }
void water_add(Instance *in)
{
    if (!in || water_of(in) || g_nw >= 16) return;
    Water *w = &g_w[g_nw++]; memset(w, 0, sizeof *w); w->inst = in; w->group = -1;
}

/* the winding normal of a node polygon, as the loader builds poly+8 (0x4280c2: the longest n = (R-Q) x (R-P) over the
 * consecutive triples), in node space */
static Vec3 poly_normal(const Model *m, const InsPoly *p)
{
    Vec3 best = { 0, 0, 0 }; float bl = 0.01f;
    for (uint32_t i = 0; i < p->nverts; i++) {
        Vec3 P = m->points[p->indices[i]].pos, Q = m->points[p->indices[(i + 1) % p->nverts]].pos, R = m->points[p->indices[(i + 2) % p->nverts]].pos;
        Vec3 u = vsub(R, Q), v = vsub(R, P), c = { u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
        float l = vlen(c); if (l > bl) { bl = l; best = (Vec3){ c.x / l, c.y / l, c.z / l }; }
    }
    return best;
}

static Vec3 wpt(const Water *w, int i)                            /* model point i through M (the node's pivot is 0 in every water box) */
{
    const Model *m = w->inst->model; Vec3 p = m->points[i].pos, piv = m->nodes[w->node].pivot;
    p.x -= piv.x; p.y -= piv.y; p.z -= piv.z; return mat4_apply(&w->M, p);
}

/* Init 0x473120: the grid over the top face. The corners are the first three indices of every polygon of the mesh node
 * whose normal has z >= 0.1 in node space (the boxes are modelled z up and turned upright by the instance), four at
 * most; 0x4742f0 half sorts them, and the edges run from corner 0 to corner 1 and from corner 0 to corner 3. */
static void build(Water *w, const TexFile *tex)
{
    Instance *in = w->inst; const Model *m = in->model; w->ready = 0;
    if (!m->nmesh_nodes || m->mesh_nodes[0] < 1 || m->mesh_nodes[0] > m->nnodes) return;
    w->node = m->mesh_nodes[0] - 1;                               /* the node lists are 1-based (0x47316e: -0x90) */
    const InsNode *n = &m->nodes[w->node];
    int c[4] = { -1, -1, -1, -1 };
    for (uint32_t k = 0; k < n->npolys; k++) {
        const InsPoly *p = &n->polys[k];
        if (poly_normal(m, p).z >= 0.1f)
            for (uint32_t q = 0; q < 3 && q < p->nverts; q++) {
                for (int s = 0; s < 4; s++) { if (c[s] == (int)p->indices[q]) break; if (c[s] < 0) { c[s] = (int)p->indices[q]; break; } }
            }
        if (w->group < 0 && !(p->material & 0x8000) && tex && p->material < tex->nmaterials) w->group = (int)tex->materials[p->material].group;
    }
    if (c[0] < 0 || c[1] < 0 || c[3] < 0) { printf("water: instance %u has no top face", in->index), puts(""); return; }
    /* 0x4742f0(c, 4), literally: its passes only ever look at the first three entries */
    #define PX(i) m->points[c[i]].pos
    for (int i = 0; i < 3; i++) if (PX(i).x < PX(0).x && PX(i).y > PX(0).y) { int t = c[i]; c[i] = c[0]; c[0] = t; }
    for (int i = 1; i < 3; i++) if (PX(i).x > PX(1).x && PX(i).y > PX(1).y) { int t = c[i]; c[i] = c[1]; c[1] = t; }
    #undef PX
    /* 0x42f7e0(0, 0, M, 1): the track of the first plain top-level node at animation 0, phase 0, times the instance
     * matrix. The boxes carry their real placement in that track: W2A 183 sits 340 lower than its .ins position says */
    if (!in->node_world) return;
    ins_pose(in, 0, 0.0f); w->M = in->node_world[w->node];
    Vec3 A = wpt(w, c[0]), e1 = vsub(wpt(w, c[1]), A), e2 = vsub(wpt(w, c[3]), A);
    w->len1 = vlen(e1); w->len2 = vlen(e2);
    if (w->len1 < w->cell) w->cell = w->len1;
    if (w->len2 < w->cell) w->cell = w->len2;
    if (w->cell <= 0) return;
    int n1 = (int)lrintf(w->subdiv * w->len1 / w->cell), n2 = (int)lrintf(w->subdiv * w->len2 / w->cell);   /* fistp: round to nearest */
    if (n1 == 0) n1 = 1;
    if (n2 == 0) n2 = 1;
    w->nc = n1 + 1; w->nr = n2 + 1;
    if (w->nc * w->nr > MAXV) { w->too_big = 1; printf("water: instance %u: Un volume d'eau a trop de face (%d x %d)", in->index, w->nc, w->nr), puts(""); return; }
    for (int j = 0; j < w->nr; j++) for (int i = 0; i < w->nc; i++) {
        float s = (float)i / (float)(w->nc - 1), t = (float)j / (float)(w->nr - 1); int k = j * w->nc + i;
        w->v[k] = (Vec3){ A.x + e1.x * s + e2.x * t, A.y + e1.y * s + e2.y * t + 10.0f, A.z + e1.z * s + e2.z * t };   /* 0x4a9750: 10 above the top face */
        w->uv[k][0] = s * w->len1 / w->cell; w->uv[k][1] = t * w->len2 / w->cell;   /* the texture repeats every `cell` units */
        w->ph[k] = (float)(int)(crt_rnd() * 512.0f);
    }
    w->centre = (Vec3){ w->v[0].x + e2.x * 0.5f + e1.x * 0.5f, w->v[0].y + e2.y * 0.5f + e1.y * 0.5f, w->v[0].z + e2.z * 0.5f + e1.z * 0.5f };
    in->noncollide = 1;                                            /* 0x473876: inst+8 |= 0x40 */
    w->wake_t = crt_rnd() * 5.0f;                                    /* 0x4a9884 */
    w->ready = 1;
    if (wenv("WOODY_WATERLOG")) printf("water: instance %u grid %d x %d, edges %.0f %.0f, cell %.0f, amp %.1f, alpha %.2f, group %d, centre %.0f %.0f %.0f",
                                         in->index, w->nc, w->nr, w->len1, w->len2, w->cell, w->amp, w->alpha, w->group, w->centre.x, w->centre.y, w->centre.z), puts("");
}

void water_param(Instance *in, int32_t cell, int32_t subdiv, int32_t amp, int32_t alpha)
{
    Water *w = water_of(in); if (!w) { water_add(in); w = water_of(in); if (!w) return; }
    tables();
    w->cell = cell * 0.01f; w->subdiv = subdiv; w->amp = amp * 0.01f; w->alpha = alpha * 0.01f;   /* 0x474650..0x474680 */
    w->group = -1; w->too_big = 0;
    build(w, g_tex);
}

/* is p inside the convex mesh node of the volume (0x4746e0: every plane of the node, in node space, has p behind it;
 * the node matrix there is the live one, the port keeps the one from Init since no water box is ever animated) */
static int inside(const Water *w, Vec3 p)
{
    const Instance *in = w->inst; const Model *m = in->model; const InsNode *n = &m->nodes[w->node];
    if (!n->npolys) return 0;
    Vec3 cen = { 0, 0, 0 };
    for (uint32_t k = 0; k < n->npoints; k++) { Vec3 q = wpt(w, (int)(n->point_base + k)); cen.x += q.x; cen.y += q.y; cen.z += q.z; }
    cen.x /= n->npoints; cen.y /= n->npoints; cen.z /= n->npoints;
    for (uint32_t f = 0; f < n->npolys; f++) {
        const InsPoly *pl = &n->polys[f]; if (pl->nverts < 3) continue;
        Vec3 a = wpt(w, (int)pl->indices[0]), b = wpt(w, (int)pl->indices[1]), c = wpt(w, (int)pl->indices[2]);
        Vec3 u = vsub(b, a), v = vsub(c, a), nn = { u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
        Vec3 dc = vsub(cen, a), dp = vsub(p, a);
        float sc = nn.x * dc.x + nn.y * dc.y + nn.z * dc.z, sp = nn.x * dp.x + nn.y * dp.y + nn.z * dp.z;
        if (fabsf(sc) < 1e-6f) continue;
        if ((sc > 0) != (sp > 0) && sp != 0) return 0;
    }
    return 1;
}

void water_update(float dt, struct Player *pl)
{
    tables();
    for (int i = 0; i < g_nw; i++) {
        Water *w = &g_w[i]; if (!w->ready) continue;
        int N = w->nc * w->nr;
        /* Draw 0x4741d6 advances every phase by ftol((rand 30 + 250) dt) each drawn frame, about half a turn per second;
         * the port keeps the fraction so the speed does not depend on the frame rate */
        for (int k = 0; k < N; k++) { w->ph[k] += (crt_rnd() * 30.0f + 250.0f) * dt; if (w->ph[k] >= 512.0f) w->ph[k] -= 512.0f; }
        if (!w->inst->visible || !game_enemy_thinks(w->inst)) continue;   /* the Update vtbl[3] 0x4747f0 runs from 0x42b400 only for the
                                                                         * instances of this frame's list world+0x64 (INSTANCE.md 4.1) */
        if ((w->wake_t -= dt) < 0) {                               /* 0x4747fb: a wake from a random point of the surface */
            Vec3 v0 = w->v[0], d1 = vsub(w->v[1], v0), d2 = vsub(w->v[w->nc], v0);   /* the first triangle: 0, 1, nc */
            float l1 = vlen(d1), l2 = vlen(d2);
            if (l1 > 0) { d1.x /= l1; d1.z /= l1; d1.y /= l1; }
            if (l2 > 0) { d2.x /= l2; d2.z /= l2; d2.y /= l2; }
            float a = crt_rnd() * w->len1, b = crt_rnd() * w->len2;
            Vec3 p = { d2.x * b + d1.x * a + v0.x, v0.y, d2.z * b + d1.z * a + v0.z };
            int ang = (int)(crt_rnd() * 512.0f);                     /* 0x499580 truncates */
            for (int e = 0; e < 64; e++) if (!g_wake[e].on) {
                g_wake[e] = (Wake){ p, { g_cos[ang & 511], 0, -g_cos[(ang + 128) & 511] }, 0, 0, 1 }; break; }
            w->wake_t += crt_rnd() * 2.0f;
        }
        if (pl && pl->inst) {                                      /* 0x4749ca: the player's position + 120 in the box = Kill(7) */
            Vec3 p = { pl->pos.x, pl->pos.y + 120.0f, pl->pos.z };   /* 0x4abcb4 */
            if (inside(w, p)) player_kill(pl, 7);                  /* 0x44d160: vtbl[0x98](7); the Perso ignores it while it already drowns */
        }
    }
    for (int e = 0; e < 64; e++) {                                 /* 0x472f50 */
        Wake *k = &g_wake[e]; if (!k->on) continue;
        k->age += dt; k->acc += dt;
        if (k->age >= 1.0f) { k->on = 0; continue; }
        while (k->acc > 0.1f) {
            k->pos.x += k->dir.x * 0.1f * 300.0f; k->pos.z += k->dir.z * 0.1f * 300.0f;   /* 0x4a986c = 300 u/s */
            for (int q = 0; q < 512; q++) if (!g_mark[q].on) { g_mark[q] = (Mark){ k->pos, 0, 1 }; break; }
            k->acc -= 0.1f;
        }
    }
    for (int q = 0; q < 512; q++) if (g_mark[q].on && (g_mark[q].age += dt) >= 0.5f) g_mark[q].on = 0;
    static float logt; if (wenv("WOODY_WATERLOG") && (logt += dt) >= 1.0f) { int nw = 0, nm = 0; logt = 0;
        for (int e = 0; e < 64; e++) nw += g_wake[e].on;
        for (int q = 0; q < 512; q++) nm += g_mark[q].on;
        printf("water: %d wakes, %d marks", nw, nm), puts(""); }
}

/* 0x473050: a flat additive sprite (flags 0x12: own colour, not a billboard -> lying on the normal (0, 1, 0)), bank 0
 * image 0x3a, colour 0.8, alpha (1 - p) 0.7, half diagonal 10 + 70 p over its 0.5 s */
void water_fx_draw(void)
{
    static const float grey[3] = { 0.8f, 0.8f, 0.8f }, up[3] = { 0, 1, 0 };
    for (int q = 0; q < 512; q++) {
        Mark *k = &g_mark[q]; if (!k->on) continue;
        float p = k->age / 0.5f; if (p >= 1.0f) continue;
        hud_world_fx_plane(0x3a, &k->pos.x, up, p * 70.0f + 10.0f, grey, (1.0f - p) * 0.7f);
    }
}

#ifndef GL_COMBINE_ARB
#define GL_COMBINE_ARB 0x8570
#define GL_COMBINE_RGB_ARB 0x8571
#define GL_COMBINE_ALPHA_ARB 0x8572
#define GL_RGB_SCALE_ARB 0x8573
#endif

void water_draw(const TexFile *tex, Vec3 eye)
{
    tables();
    static int combine = -1;
    if (combine < 0) { const char *ext = (const char *)glGetString(GL_EXTENSIONS), *ver = (const char *)glGetString(GL_VERSION);
                       combine = (ext && strstr(ext, "GL_ARB_texture_env_combine")) || (ver && (ver[0] > '1' || (ver[0] == '1' && ver[2] >= '3'))); }
    /* 0x42b460 links every polygon IN FRONT of its texture's list for the bucket (0x42b4c5..0x42b4d3) and the flush walks
     * that list from its head, so the water triangles come out in the reverse of their submission: the volumes in the reverse
     * of their Draw order (the instance list world+0x64, 0x42b380), each with layer A (submitted second) before layer B and
     * every layer from its last triangle to its first */
    static int ord[64]; int no = 0;
    { Instance *const *lst; uint32_t n = game_instance_list(&lst);
      if (lst) { for (uint32_t li = n; li-- > 0 && no < 64; ) for (int i = 0; i < g_nw; i++) if (g_w[i].inst == lst[li]) { ord[no++] = i; break; } }
      for (int i = g_nw; i-- > 0 && no < 64; ) { int seen = 0; for (int q = 0; q < no; q++) seen |= ord[q] == i; if (!seen) ord[no++] = i; } }   /* no list, or drawn outside it */
    for (int oi = 0; oi < no; oi++) {
        Water *w = &g_w[ord[oi]]; Instance *in = w->inst;
        if (!w->ready || !in->visible || !in->drawn || w->group < 0 || (uint32_t)w->group >= tex->ngroups) continue;
        int N = w->nc * w->nr;
        /* 0x474128: a point behind the water as seen from the camera, a quarter of the edge sum up */
        float dx = w->centre.x - eye.x, dz = w->centre.z - eye.z, l = sqrtf(dx * dx + dz * dz), L = w->len1 + w->len2;
        if (l > 0) { dx /= l; dz /= l; }
        Vec3 sun = { w->centre.x + L * dx, w->centre.y + L * 0.25f, w->centre.z + L * dz };
        static float col[MAXV];
        for (int k = 0; k < N; k++) {                              /* 0x474490: highlight where the sun's reflection meets the eye */
            Vec3 a = vsub(eye, w->v[k]), b = vsub(w->v[k], sun); float la = vlen(a), lb = vlen(b), val = 1.0f;
            if (la > 0) { a.x /= la; a.y /= la; a.z /= la; }
            if (lb > 0) { b.x /= lb; b.y /= lb; b.z /= lb; }
            Vec3 r = { b.x, -b.y, b.z };                          /* mirrored in the water plane */
            if (a.x * r.x + a.y * r.y + a.z * r.z >= 0) {
                Vec3 c = { a.y * r.z - a.z * r.y, a.z * r.x - a.x * r.z, a.x * r.y - a.y * r.x }; val = vlen(c);
            }
            int idx = (int)lrintf(val * 127.0f); if (idx < 0) idx = 0; if (idx > 127) idx = 127;
            col[k] = g_spec[idx] + 0.4f;                           /* 0x4aa394 */
        }
        /* the two layers: B (5 lower, UV + (0.23, 0.85), the vertices circling the other way) is submitted first, then A
         * (0x473a58..), so A is drawn first (see above). Only the interior vertices move; the border rows and columns stay put
         * so the surface keeps its outline */
        glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, tex->groups[w->group].gl_frames ? tex->groups[w->group].gl_frames[0] : tex->groups[w->group].gl_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_FALSE); glDisable(GL_ALPHA_TEST);
        float k2 = 1.0f;
        if (combine) { glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_ARB); glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_MODULATE);
                       glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA_ARB, GL_MODULATE); glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_ARB, 2.0f); }
        else { glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE); k2 = 2.0f; }
        glBegin(GL_TRIANGLES);
        for (int layer = 1; layer >= 0; layer--) {
            float sgn = layer == 0 ? -1.0f : 1.0f, dy = layer == 0 ? -5.0f : 0.0f, du = layer == 0 ? 0.23f : 0.0f, dv = layer == 0 ? 0.85f : 0.0f;
            for (int j = w->nr - 2; j >= 0; j--) for (int i2 = w->nc - 2; i2 >= 0; i2--) {
                /* the cell's two triangles (k, k+1, k+nc) and (k+nc, k+1, k+nc+1) (+0x108), the second one first */
                int t[6] = { (j + 1) * w->nc + i2, j * w->nc + i2 + 1, (j + 1) * w->nc + i2 + 1, j * w->nc + i2, j * w->nc + i2 + 1, (j + 1) * w->nc + i2 };
                for (int q = 0; q < 6; q++) {
                    int k = t[q]; Vec3 p = w->v[k];
                    int edge = k < w->nc || k >= (w->nr - 1) * w->nc || k % w->nc == 0 || (k + 1) % w->nc == 0;
                    if (!edge) { int ph = (int)w->ph[k]; p.x += sgn * g_cos[ph & 511] * w->amp; p.z -= sgn * g_cos[(ph + 128) & 511] * w->amp; }
                    float c = col[k] * k2; if (c > 1.0f) c = 1.0f;
                    glColor4f(c, c, c, w->alpha);
                    glTexCoord2f(w->uv[k][0] + du, w->uv[k][1] + dv);
                    glVertex3f(p.x, p.y + dy, p.z);
                }
            }
        }
        glEnd();
    }
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE); glColor4f(1, 1, 1, 1);
    glDepthMask(GL_TRUE); glDisable(GL_BLEND); glEnable(GL_ALPHA_TEST);
}
