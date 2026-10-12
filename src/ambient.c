/* ambient.c - instance class 90, the "environment instance": modes 0 (butterflies), 1 (motes) and 2 (rain). docs/AMBIENT.md.
 *
 * The class (ctor 0x403d2c: 0x150 bytes, base 0x42e1a0, vtable 0x4a90ac; think vt[3] 0x472560, vt[0x70/4] 0x472b30,
 * vt[0x74/4] 0x472b40, dtor 0x472e30) owns no geometry of its own: everything is measured on its FIRST VOLUME NODE
 * (model+0x4c[0], 1-based, node record model+0x68 + 0x90*(i-1), world matrix [0x509adc]+0xa0 row inst+0x5c+i-1).
 * The box is taken over the node's own points in NODE-LOCAL space (0x47285b / 0x472bde) and the author treats local z as
 * "up" (the 3ds Max axis): the rain falls along local -z from the plane z = maxZ - minZ, the motes start on z = minZ.
 *
 * Every particle is a record of the one effect pool of the game ([0x5e823c]+0xdb8, 2000 x 0x50 B, bump allocator, a full
 * pool drops the new record; docs/PARTICLES.md 0, AMBIENT.md 5): the records are FxRec kind FX_AMB of main_engine.c's pool,
 * shared with every other effect, and run by its driver (0x470c70) through ambient_fx_run. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ambient.h"
#include "player.h"
#include "hud.h"
#include "crtrand.h"

typedef struct {
    Instance *in;
    int mode;                  /* +0xfc (1501) */
    int count, live;           /* +0x100 wanted (1502 / 1504), +0x108 alive: a particle that dies gives its place back */
    int off, fill;             /* +0x120 (1511; 1502 clears it via 0x472b30), +0x121 = "the stream starts empty": motes spawn at a random age */
    unsigned stamp;            /* +0x104 = the frame counter [[0x509adc]] of the last think; a mote / butterfly dies on a frame its owner did not think */
    float floor_y;             /* +0x10c: world y of the local minY (0x472911); the butterflies land on it (0x47d6dc) */
    float rgb[3], life0;       /* +0x110..0x118 = rgb / 255, +0x11c = arg * 0.01: the base lifetime in s */
    int force; float interval, acc, zr; int rot, grid_ok;   /* +0x14c, +0x140, +0x13c, +0x148, +0x138, +0x124 != NULL */
    float grid[400][3];        /* +0x124: 5 x 5 cells x 16 start points on the top of the box (0x472b40) */
    int logged;
} Amb;

enum { K_BUTTERFLY = 1, K_MOTE, K_DROP, K_RIPPLE };

#define AMB_MAX 32
static Amb g_amb[AMB_MAX]; static int g_namb;
static unsigned g_frame;
static int g_flylog; static float g_flyt;                                        /* WOODY_FLYLOG: the butterflies once a second */

void ambient_reset(void) { g_namb = 0; g_flyt = 0; }                           /* the pool itself is emptied with the level (0x470d50) */

static Amb *amb_of(Instance *in, int create)
{
    for (int i = 0; i < g_namb; i++) if (g_amb[i].in == in) return &g_amb[i];
    if (!create || g_namb >= AMB_MAX) return NULL;
    Amb *a = &g_amb[g_namb++]; memset(a, 0, sizeof *a); a->in = in; a->interval = 0.2f; a->stamp = ~0u; return a;
}

/* the first volume node: 0-based index, or -1 ("une instance d'environnement n'a pas de volume", 0x4725c5 / 0x47284e) */
static int amb_node(const Instance *in)
{
    const Model *m = in->model;
    if (!m || !m->nvolume_nodes) return -1;
    uint32_t ni = m->volume_nodes[0] - 1; if (ni >= m->nnodes || !m->nodes[ni].npoints) return -1;
    return (int)ni;
}
/* 0x47285b..0x47290f: node-local aabb of the node's points (pivot-relative, as the loader leaves them) */
static void amb_box(const Instance *in, int ni, float lo[3], float hi[3])
{
    const Model *m = in->model; const InsNode *n = &m->nodes[ni];
    lo[0] = lo[1] = lo[2] = 1e30f; hi[0] = hi[1] = hi[2] = -1e30f;
    for (uint32_t k = 0; k < n->npoints; k++) {
        Vec3 p = m->points[n->point_base + k].pos; float v[3] = { p.x - n->pivot.x, p.y - n->pivot.y, p.z - n->pivot.z };
        for (int q = 0; q < 3; q++) { if (v[q] < lo[q]) lo[q] = v[q]; if (v[q] > hi[q]) hi[q] = v[q]; }
    }
}
/* the loader's polygon plane (0x4280c2, same as render_gl.c poly_plane): longest (R-Q)x(R-P) over consecutive triples */
static void amb_plane(const Model *m, const InsNode *n, const InsPoly *p, float out[4])
{
    float best = 0, nx = 1, ny = 0, nz = 0; const InsPoint *rb = NULL;
    for (uint32_t i = 0; i < p->nverts; i++) {
        const InsPoint *P = &m->points[p->indices[i]], *Q = &m->points[p->indices[(i + 1) % p->nverts]], *R = &m->points[p->indices[(i + 2) % p->nverts]];
        float ux = R->pos.x - Q->pos.x, uy = R->pos.y - Q->pos.y, uz = R->pos.z - Q->pos.z;
        float vx = R->pos.x - P->pos.x, vy = R->pos.y - P->pos.y, vz = R->pos.z - P->pos.z;
        float cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx, l = sqrtf(cx * cx + cy * cy + cz * cz);
        if ((!rb || best < l) && l > 0.01f) { best = l; nx = cx / l; ny = cy / l; nz = cz / l; rb = R; }
    }
    out[0] = nx; out[1] = ny; out[2] = nz; out[3] = rb ? -(nx * (rb->pos.x - n->pivot.x) + ny * (rb->pos.y - n->pivot.y) + nz * (rb->pos.z - n->pivot.z)) : 0;
}
/* the original's 3x4 node matrix has rows = the images of the local axes (+0x00, +0x0c, +0x18) and the origin at +0x24;
 * the port's Mat4 is column-major, so row k is column k here */
static Vec3 m_point(const Mat4 *M, const float p[3]) { return mat4_apply(M, (Vec3){ p[0], p[1], p[2] }); }
static Vec3 m_dir(const Mat4 *M, const float d[3])
{
    const float *o = M->m;
    return (Vec3){ o[0] * d[0] + o[4] * d[1] + o[8] * d[2], o[1] * d[0] + o[5] * d[1] + o[9] * d[2], o[2] * d[0] + o[6] * d[1] + o[10] * d[2] };
}
static void v_norm(Vec3 *v)                                                      /* the normalisations of 0x47e050 / 0x47d440 / 0x47e230: skipped when |v| <= 0 */
{
    float l = sqrtf(v->x * v->x + v->y * v->y + v->z * v->z);
    if (l > 0) { v->x /= l; v->y /= l; v->z /= l; }
}
static AmbFx *fx_new(int kind, int owner)
{
    AmbFx *f = game_fx_amb_new(); if (!f) return NULL;                            /* 0x47e062 / 0x47e172 / 0x47e241 / 0x47e321: count >= 2000 -> nothing */
    f->kind = kind; f->owner = owner; return f;
}

/* 0x472b40 (vt[0x74/4], called by 1503): 25 cells over the x/y extent of the box, 16 random start points each, all on the
 * local plane z = maxZ - minZ; the rain direction (0.05, 0.05, -0.8) in local space; the spawn interval from the force */
static void amb_rain_init(Amb *a)
{
    int ni = amb_node(a->in); if (ni < 0) return;
    float lo[3], hi[3]; amb_box(a->in, ni, lo, hi);
    float zr = hi[2] - lo[2], dx = (hi[0] - lo[0]) * 0.2f, dy = (hi[1] - lo[1]) * 0.2f;   /* 0x4aab94 = 0.2 */
    int k = 0;
    for (int row = 0; row < 5; row++) for (int col = 0; col < 5; col++) for (int j = 0; j < 16; j++, k++) {
        a->grid[k][0] = (crt_rnd() + (float)col) * dx + lo[0];
        a->grid[k][1] = (crt_rnd() + (float)row) * dy + lo[1];
        a->grid[k][2] = zr;
    }
    for (int j = 0; j < 400; j++) crt_rnd();                                       /* +0x128: 400 floats rnd*45 + 90, never read again */
    a->zr = zr; a->rot = 0; a->acc = 0;
    switch (a->force) {                                                          /* jump table 0x472e18 */
    case 1: a->interval = 0.2f; break; case 2: a->interval = 0.15f; break; case 3: a->interval = 0.1f; break; case 4: a->interval = 0.05f; break;
    default: printf("ambient: La force du vent doit etre comprise entre 1 et 4 (%d)\n", a->force); break;   /* 0x4b7ad4; the interval stays */
    }
    a->grid_ok = 1;
}

void ambient_msg(Instance *in, int id, const uint32_t *args, int nargs)
{
    if (!in) return;
    Amb *a = amb_of(in, 1); if (!a) return;
#define AI(i) ((i) < nargs ? (int32_t)args[i] : 0)
    switch (id) {
    case 1501: a->mode = AI(1); break;                                           /* 0x46cd07 */
    case 1502:                                                                   /* 0x46cd2e: 0x4abc5c = 1/255, 0x4aa0ac = 0.01 */
        a->rgb[0] = AI(1) / 255.0f; a->rgb[1] = AI(2) / 255.0f; a->rgb[2] = AI(3) / 255.0f; a->life0 = AI(4) * 0.01f;
        a->live = 0; a->count = AI(5);
        a->off = 0; a->fill = 0;                                                 /* vt[0x70/4] = 0x472b30 */
        break;
    case 1503: a->force = AI(1); amb_rain_init(a); break;                        /* 0x46cda0 -> vt[0x74/4] = 0x472b40 */
    case 1504: a->count = AI(1); a->live = 0; break;                             /* 0x46cdcc */
    case 1511: a->off = AI(1) != 0; break;                                       /* 0x46cf44 */
    }
#undef AI
}

/* 0x47e050: a butterfly at w. Phase rnd*10 (0x4a9750), life 1e6 (0x49742400), direction (rnd*2-1) per axis, normalised
 * when not zero, image 0x10035 - ftol(rnd * -3.99) = bank 0 image 53..56 (0x4abd98). +0x24 = 0; +0x2c (the state) is
 * NOT written: the record keeps what the last record in that pool slot left there (the port starts at 0, AMBIENT.md 6.2) */
static void amb_butterfly(int ai, Vec3 w)
{
    AmbFx *f = fx_new(K_BUTTERFLY, ai); if (!f) return;
    f->age = crt_rnd() * 10.0f; f->life = 1e6f; f->pos = w;
    f->dir.x = crt_rnd() * 2.0f - 1.0f; f->dir.y = crt_rnd() * 2.0f - 1.0f; f->dir.z = crt_rnd() * 2.0f - 1.0f;
    f->wander = 0;
    f->img = (int)(crt_rnd() * 3.99f);                                             /* 0..3 = images 53..56 */
    v_norm(&f->dir);
}

/* 0x4727d3, modes 0 and 1: keep `count` butterflies / motes alive. x anywhere across the box; mode 1 puts z on the bottom
 * of the box, mode 0 draws it over the local Y extent from minZ (0x472953), and y is drawn over the local Z extent from
 * minY (0x47296a): the y/z slip of the original, kept (harmless for the shipped 400-unit cubes). A point in front of the
 * node's first polygon is mirrored through it (0x472985; the loop never advances past polygon 0). The world point is M p.
 * Every mote moves along the world image of the local normal of the polygon that faces most along local +z (0x472a47). */
static void amb_think_spawn(Amb *a, int ai)
{
    if (a->off && !wenv("WOODY_AMBON")) return;                                /* 0x4727d3; WOODY_AMBON=1 (test hook) ignores 1511 and hiding */
    if (a->live == 0) a->fill = 1;
    if (a->live >= a->count) { a->fill = 0; return; }
    int ni = amb_node(a->in); if (ni < 0 || !a->in->node_world) return;
    const Model *m = a->in->model; const InsNode *n = &m->nodes[ni]; const Mat4 *M = &a->in->node_world[ni];
    float lo[3], hi[3]; amb_box(a->in, ni, lo, hi);
    float zr = hi[2] - lo[2], pl0[4] = { 0, 0, 0, -1 }, up[3] = { 0, 0, 1 };
    a->floor_y = lo[1] * M->m[5] + M->m[13];                                     /* 0x472911: minY * M[4] (row 1 .y) + M[10] (origin .y) */
    if (n->npolys) {
        amb_plane(m, n, &n->polys[0], pl0);
        if (a->mode == 1) {
            float best = -2;
            for (uint32_t k = 0; k < n->npolys; k++) { float pl[4]; amb_plane(m, n, &n->polys[k], pl); if (k == 0 || pl[2] > best) { best = pl[2]; up[0] = pl[0]; up[1] = pl[1]; up[2] = pl[2]; } }
        }
    }
    Vec3 N = m_dir(M, up);                                                       /* not normalised: a scaled node scales the speed */
    if (wenv("WOODY_AMBLOG") && !a->logged) {
        a->logged = 1;
        printf("ambient: inst %u mode %d count %d life %.2f rgb %.2f %.2f %.2f box x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f up %.2f %.2f %.2f world %.0f %.0f %.0f floor %.0f\n",
               a->in->index, a->mode, a->count, a->life0, a->rgb[0], a->rgb[1], a->rgb[2], lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], N.x, N.y, N.z, M->m[12], M->m[13], M->m[14], a->floor_y);
    }
    while (a->live < a->count) {
        float p[3];
        p[0] = crt_rnd() * (hi[0] - lo[0]) + lo[0];
        p[2] = a->mode == 1 ? lo[2] : crt_rnd() * (hi[1] - lo[1]) + lo[2];         /* 0x472936: mode 1 = minZ; else the Y extent from minZ */
        p[1] = crt_rnd() * zr + lo[1];
        if (n->npolys) {
            float d = p[0] * pl0[0] + p[1] * pl0[1] + p[2] * pl0[2] + pl0[3];
            if (d > 0) for (int q = 0; q < 3; q++) p[q] -= 2 * d * pl0[q];
        }
        Vec3 w = m_point(M, p);
        if (a->mode == 0) amb_butterfly(ai, w);                                  /* 0x472ae8 -> 0x47e050 */
        else {
            AmbFx *f = fx_new(K_MOTE, ai);                                       /* 0x47e160 */
            if (f) {
                float s = a->fill ? crt_rnd() : 0.0f;
                f->life = crt_rnd() * 2.0f + a->life0; f->age = f->life * s;      /* the first fill starts spread over the lifetime */
                f->dir = N;
                f->speed = crt_rnd() * 20.0f + 40.0f;                              /* 0x4a9994 = 20, 0x4ab294 = 40 */
                f->pos = (Vec3){ w.x + N.x * f->age * f->speed, w.y + N.y * f->age * f->speed, w.z + N.z * f->age * f->speed };
            }
        }
        a->live++;                                                               /* counted even when the pool was full */
    }
    a->fill = 0;                                                                 /* 0x472b13 */
}

/* 0x472596, mode 2: every `interval` s one drop per cell, from start point `rot` of that cell; rot then steps 1..16 */
static void amb_think_rain(Amb *a, int ai, float dt)
{
    int ni = amb_node(a->in); if (ni < 0 || !a->in->node_world || !a->grid_ok || a->interval <= 0) return;
    const Mat4 *M = &a->in->node_world[ni];
    a->acc += dt;
    if (wenv("WOODY_AMBLOG") && !a->logged) {
        float lo[3], hi[3]; amb_box(a->in, ni, lo, hi); a->logged = 1;
        static const float dl[3] = { 0.05f, 0.05f, -0.8f }; Vec3 d = m_dir(M, dl);
        printf("ambient: inst %u mode 2 force %d interval %.2f box x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f fall %.2f %.2f %.2f world %.0f %.0f %.0f\n",
               a->in->index, a->force, a->interval, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], d.x, d.y, d.z, M->m[12], M->m[13], M->m[14]);
    }
    while (a->acc > a->interval) {
        a->acc -= a->interval;
        static const float dl[3] = { 0.05f, 0.05f, -0.8f };                     /* +0x12c..0x134 */
        Vec3 d = m_dir(M, dl); v_norm(&d);
        const float *o = M->m; float len = sqrtf(o[8] * o[8] + o[9] * o[9] + o[10] * o[10]) * a->zr;   /* |row 2| * height */
        for (int c = 0; c < 25; c++) {
            Vec3 w = m_point(M, a->grid[(a->rot + 16 * c) % 400]);
            AmbFx *f = fx_new(K_DROP, ai);                                       /* 0x47e230 (normalises D once more) */
            if (f) { f->age = 0; f->life = 1.0f; f->pos = w; f->dir = d; v_norm(&f->dir); f->fallen = 0; f->len = len; f->speed = 4000.0f; }   /* 0x457a0000 */
        }
        a->rot = (a->rot + (int)(crt_rnd() * 15.0f + 1.0f)) & 15;                 /* 0x4a9864 = 15 */
    }
}

void ambient_update(float dt)
{
    g_frame++;
    g_flylog = 0;
    if (wenv("WOODY_FLYLOG") && (int)(g_flyt + dt) != (int)g_flyt) { g_flylog = 1; printf("  FLY t %.0f\n", g_flyt + dt); }
    g_flyt += dt;
    for (int i = 0; i < g_namb; i++) {                                           /* think 0x472560 (vt[3]): only for the instances of this frame's list world+0x64 (0x42b400) */
        Amb *a = &g_amb[i];
        if (a->in->type != 90) continue;
        if ((!a->in->visible || !game_enemy_thinks(a->in)) && !wenv("WOODY_AMBON")) continue;   /* hidden = out of the world (0x407850); a stationary volume also drops out with its sphere off screen (0x42a8b4) */
        a->stamp = g_frame;                                                      /* +0x104 (0x472576) */
        if (a->mode == 0 || a->mode == 1) amb_think_spawn(a, i);
        else if (a->mode == 2) amb_think_rain(a, i, dt);
    }
}

/* 0x4300c0: is the point inside one of the instance's volume nodes */
static int amb_inside(const Instance *in, Vec3 p)
{
    const Model *m = in->model; if (!m) return 0;
    for (uint32_t k = 0; k < m->nvolume_nodes; k++) { uint32_t ni = m->volume_nodes[k] - 1; if (ni < m->nnodes && volume_contains(in, ni, p)) return 1; }
    return 0;
}

/* 0x47d440: a butterfly. One rnd a frame decides the state changes (0 -> 1 with r <= 0.001, 0x4a94c4; 2 -> 0 with
 * r <= 0.008, 0x4abd94, turning towards the instance origin); landed (2) it neither moves nor tests the volume and flaps
 * at F = 166.667 (0x47d767) instead of 1000. Flying, every 0.3 s (0x4aab98; the timer also runs while landed, so a
 * butterfly that takes off after a long rest re-steers every frame until it has caught up) the course gets a random push
 * of up to 3.5 (0x4abd90) along the sign it already has in x and z, and up (0.7) / down (-0.5) at random or -0.8 while
 * coming down; then pos += 100 dt dir (0x4a9010). Outside the volume (0x4300c0) it turns back to the instance origin;
 * coming down, at or under the floor (+0x10c) outside the volume it lands: state 2, dir = (dx, 0, dz) to the origin. */
static int amb_butterfly_run(AmbFx *f, Amb *a, float dt)
{
    const Instance *in = a->in;
    f->age += dt; f->wander += dt;
    float r = crt_rnd(), k = dt * 60.0f;                                           /* the original rolls once per frame; normalised to 60 Hz here */
    if (dt > 0) {
        if (f->state == 0 && r <= 0.001f * k) f->state = 1;
        if (f->state == 2 && r <= 0.008f * k) {
            f->state = 0;
            f->dir = (Vec3){ in->position.x - f->pos.x, in->position.y - f->pos.y, in->position.z - f->pos.z }; v_norm(&f->dir);
        }
    }
    float F = 1000.0f;
    if (f->state == 2) F = 166.667f;                                             /* 0x4326aaab */
    else {
        if (f->wander > 0.3f) {
            f->wander -= 0.3f;
            float sx = f->dir.x < 0 ? -1.0f : 1.0f, ky, sz;
            if (f->state == 1) ky = -0.8f; else ky = crt_rnd() * 2.0f - 1.0f > 0 ? 0.7f : -0.5f;
            sz = f->dir.z < 0 ? -1.0f : 1.0f;
            float ax = crt_rnd() * sx * 3.5f, ay = crt_rnd() * ky * 3.5f, az = crt_rnd() * sz * 3.5f;
            f->dir.x += ax; f->dir.y += ay; f->dir.z += az; v_norm(&f->dir);
        }
        f->pos.x += dt * f->dir.x * 100.0f; f->pos.y += dt * f->dir.y * 100.0f; f->pos.z += dt * f->dir.z * 100.0f;
        if (!amb_inside(in, f->pos)) {
            if (f->state == 0 || (f->state == 1 && f->pos.y > a->floor_y)) {
                f->dir = (Vec3){ in->position.x - f->pos.x, in->position.y - f->pos.y, in->position.z - f->pos.z }; v_norm(&f->dir);
            } else if (f->state == 1) {                                          /* 0x47d74f */
                f->state = 2;
                f->dir = (Vec3){ in->position.x - f->pos.x, 0, in->position.z - f->pos.z }; v_norm(&f->dir);
            }
        }
    }
    if (a->stamp != g_frame) { a->live--; return 0; }                            /* 0x47dc77: the owner did not think, gone at once */
    if (g_flylog) printf("    fly inst %u [%.0f %.0f %.0f s%d]\n", in->index, f->pos.x, f->pos.y, f->pos.z, f->state);
    /* 0x47d78b: not a sprite but two squares hinged along V = (dir.x, 0.5, dir.z) normalised; B0/B1 an orthonormal pair
     * across it (0x46d320). Hinge angle from the cosine table: 255 - fistp(128 cos^2(F t + 128)) in 1/512 turn, i.e.
     * theta = 134.30 + 45.00 cos(2 pi (2F/512) t) degrees (3.906 Hz flying, 0.651 Hz landed). Wing 1 U = cos B0 + sin B1,
     * wing 2 U' = -cos B0 + sin B1; each 21.2132 (30 cos 45) half-extents, centre 20.2757 along U, plus a vertical bob
     * 3 sin(2 pi (2F/512) t) (0x47da2c). Mode 0x12, UV set 4, flags 0x68: alpha blended with colour key (TITLE.md 3.1) */
    Vec3 V = { f->dir.x, 0.5f, f->dir.z };
    float l = sqrtf(V.x * V.x + V.y * V.y + V.z * V.z); if (l < 1e-6f) return 1;
    V.x /= l; V.y /= l; V.z /= l;
    Vec3 B0 = { V.z, 0, -V.x };
    l = sqrtf(B0.x * B0.x + B0.z * B0.z); if (l < 1e-6f) { B0 = (Vec3){ 1, 0, 0 }; l = 1; }
    B0.x /= l; B0.z /= l;
    Vec3 B1 = { V.y * B0.z - V.z * B0.y, V.z * B0.x - V.x * B0.z, V.x * B0.y - V.y * B0.x };
    float w = 2 * 3.14159265f * (2.0f * F / 512.0f) * f->age;
    float th = (134.30f + 45.0f * cosf(w)) * 3.14159265f / 180.0f, c = cosf(th), sn = sinf(th), bob = 3.0f * sinf(w);
    for (int q = 0; q < 2; q++) {
        float cc = q ? -c : c;
        Vec3 U = { cc * B0.x + sn * B1.x, cc * B0.y + sn * B1.y, cc * B0.z + sn * B1.z };
        float ctr[3] = { f->pos.x + 20.2757f * U.x, f->pos.y + 20.2757f * U.y + bob, f->pos.z + 20.2757f * U.z };
        hud_world_wing(f->img, ctr, &U.x, &V.x, 21.2132f);
    }
    return 1;
}

int ambient_fx_run(AmbFx *f, float dt, const float *eye)
{
    if (f->owner < 0 || f->owner >= g_namb) return 0;
    Amb *a = &g_amb[f->owner];
    if (f->kind == K_BUTTERFLY) return amb_butterfly_run(f, a, dt);
    if (f->kind == K_MOTE) {                                                     /* 0x47dca0 */
        if (a->stamp != g_frame) { a->live--; return 0; }                        /* the owner did not think: gone at once */
        if ((f->age += dt) >= f->life) { a->live--; return 0; }                  /* its place is refilled next think */
        f->pos.x += dt * f->dir.x * f->speed; f->pos.y += dt * f->dir.y * f->speed; f->pos.z += dt * f->dir.z * f->speed;
        /* flags 7 (camera facing, own colour, rotation, additive), mode 0x12, size 25, image 29 (0x1001d), rotation
         * (int)(age * 250) in 1/512 turn; alpha 1 until u = 0.7, then down to 0 at u = 1 (0x4aa1d8, 0x4abd30 = 3.33) */
        float u = f->age / f->life, al = u < 0.7f ? 1.0f : 1.0f - (u - 0.7f) * 3.33f;
        hud_world_spr(29, &f->pos.x, 25.0f, (int)lrintf(f->age * 250.0f), a->rgb, al, 7, NULL, 0);
        return 1;
    }
    if (f->kind == K_DROP) {                                                     /* 0x47de10 */
        static const float grey[3] = { 0.4f, 0.4f, 0.4f };
        f->age += dt;
        float s = dt * f->speed * f->dir.y, nf = f->fallen - s;                  /* +0x24 counts the height fallen */
        if (nf < f->len) {
            f->fallen = nf;
            f->pos.x += dt * f->speed * f->dir.x; f->pos.y += s; f->pos.z += dt * f->speed * f->dir.z;
            if (f->age > 5.0f) return 0;                                         /* port guard: a drop that never falls (dir.y >= 0) would live for ever */
            /* line 0x471a10(0xe00): from 250 behind (alpha 0) to the drop (alpha 0.5), grey 0.4, half width 4, image 57 */
            if (eye) {
                float p0[3] = { f->pos.x - f->dir.x * 250.0f, f->pos.y - f->dir.y * 250.0f, f->pos.z - f->dir.z * 250.0f };
                hud_world_streak(57, p0, &f->pos.x, eye, 4.0f, grey, 0.0f, 0.5f);
            }
            return 1;
        }
        float t = (f->fallen - f->len) / s * dt;                                 /* 0x47df30: the rest of the step down to the bottom, a ripple there */
        f->pos.x += t * f->dir.x * f->speed; f->pos.y += t * f->dir.y * f->speed; f->pos.z += t * f->dir.z * f->speed;
        Vec3 p = f->pos; int ow = f->owner;
        AmbFx *r = fx_new(K_RIPPLE, ow);                                         /* 0x47e310: age 0, life 0.2 */
        if (r) { r->age = 0; r->life = 0.2f; r->pos = p; }
        return 0;
    }
    if (f->kind == K_RIPPLE) {                                                   /* 0x47df80 */
        static const float white[3] = { 1, 1, 1 }, up[3] = { 0, 1, 0 };
        float u = (f->age += dt) / f->life;
        if (u >= 1.0f) return 0;
        /* flags 2 (plane with normal (0,1,0), own colour, additive), image 3 = byte-identical to 58 (hud slot 0x3a),
         * size u*30 + 5, alpha 0.5 - 0.5u */
        hud_world_spr(0x3a, &f->pos.x, u * 30.0f + 5.0f, 0, white, 0.5f - 0.5f * u, 2, up, 0);
        return 1;
    }
    return 0;
}
