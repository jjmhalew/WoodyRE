/* blackbox.c - the BlackBox mini game (App state 3, level 25 \Data\BlackBox, docs/BLACKBOX.md): a one-screen 2D platformer in
 * the style of the arcade Donkey Kong, drawn over the frozen 3D level. Woody climbs from the floor of the jungle to the potion at
 * the top right while Buzz Buzzard shoots, rolls rocks, drops dynamite and swoops; the potion blows up the cage of Knothead and
 * Splinter. Three rounds (speed 1.0 / 1.4 / 1.7, 3 / 2 / 1 lives), then the credits.
 * The object 0x484420 (0xc0780 B at app+0xe4): +0 the mask object 0x488790 (8 background refs + \Game\mask.bin), +0xc0034 Woody
 * (0x485a50), +0xc00bc Buzz (0x486d60), +0xc0144 the potion, +0xc01a8 the cage, +0xc020c Splinter, +0xc0278 Knothead, +0xc02e4
 * the pool of 10 projectiles / effects (0x485330), +0xc06fc the lives icon, +0xc0760.. the round state. Coordinates are the
 * 640x480 virtual screen, y down. */
#include "blackbox.h"
#include "hud.h"
#include "audio.h"
#include "texpack.h"
#include "crtrand.h"
#include "plat.h"
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* the animation records of the 13 sprite types, from the exe (0x4b7f10, 58 x 101 dwords: frames of 6 ints up to -1, +0x190 the image ref 0x0101xxxx) */
static const short k_frm[253][6] = {   /* table 0x4b7f10: {u, v, w, h, hot x, hot y} per frame */
    {0,0,29,27,11,26}, {30,0,27,29,11,28}, {58,0,21,28,9,27}, {80,0,21,27,14,26}, {0,30,25,26,17,25}, {0,0,26,28,8,27}, {27,0,25,28,8,27}, {53,0,23,28,8,27},
    {77,0,24,28,6,27}, {0,29,26,28,8,27}, {27,29,24,28,9,27}, {52,29,24,28,8,27}, {77,29,25,28,7,27}, {0,0,21,29,13,28}, {22,0,20,29,11,28}, {0,0,26,17,11,16},
    {27,0,23,17,10,16}, {51,0,24,17,11,16}, {76,0,27,17,14,16}, {0,0,27,22,12,21}, {28,0,26,20,11,19}, {55,0,28,17,11,16}, {0,0,24,39,10,38}, {25,0,17,39,4,32},
    {43,0,23,39,5,36}, {67,0,26,39,9,38}, {0,0,28,39,13,38}, {29,0,16,39,7,28}, {46,0,28,39,6,38}, {75,0,25,27,12,26}, {0,40,27,27,12,26}, {0,0,16,36,9,32},
    {17,0,16,35,8,34}, {34,0,16,35,8,34}, {51,0,14,35,7,34}, {66,0,14,35,8,32}, {81,0,15,33,9,32}, {97,0,15,33,8,32}, {0,37,15,33,8,31}, {0,0,14,30,8,29},
    {15,0,18,29,11,28}, {34,0,21,28,11,27}, {0,0,23,31,15,30}, {24,0,28,25,19,24}, {0,0,23,24,12,23}, {24,0,25,24,13,23}, {50,0,25,24,13,23}, {76,0,25,24,13,23},
    {0,25,25,24,13,23}, {0,0,36,39,1,38}, {37,0,39,39,1,38}, {77,0,31,37,2,36}, {0,0,32,29,18,28}, {33,0,39,29,12,28}, {73,0,40,29,13,28}, {0,0,38,36,21,34},
    {39,0,31,37,16,35}, {71,0,32,41,16,39}, {0,42,38,36,21,34}, {39,42,31,37,16,35}, {71,42,32,41,16,39}, {0,0,37,27,21,25}, {38,0,34,42,16,40}, {73,0,40,52,17,50},
    {0,53,38,59,15,57}, {0,0,30,42,16,40}, {31,0,38,37,21,35}, {70,0,30,40,17,38}, {0,43,33,40,18,38}, {34,43,38,37,21,35}, {73,43,33,38,17,36}, {0,0,34,40,18,38},
    {35,0,34,40,20,38}, {70,0,37,40,19,38}, {0,0,19,33,12,30}, {20,0,33,37,15,34}, {54,0,34,37,15,35}, {0,0,34,35,18,20}, {35,0,34,34,18,21}, {70,0,34,35,18,24},
    {0,36,34,36,18,22}, {35,36,34,35,18,20}, {70,36,34,34,18,21}, {0,0,34,37,15,18}, {0,0,34,37,15,18}, {35,0,34,36,14,19}, {70,0,34,35,15,20}, {0,38,35,36,17,19},
    {36,38,34,35,18,20}, {71,38,37,34,18,19}, {0,76,36,34,19,20}, {37,76,35,34,20,19}, {73,76,36,35,19,18}, {0,0,34,37,17,35}, {0,0,34,37,16,35}, {0,0,20,24,12,22},
    {21,0,20,24,12,22}, {42,0,19,23,11,21}, {62,0,20,23,9,21}, {83,0,23,23,9,21}, {0,25,24,23,8,21}, {25,25,21,23,9,21}, {47,25,18,23,11,21}, {66,25,19,22,11,20},
    {0,0,19,24,11,23}, {20,0,18,24,10,23}, {39,0,18,24,10,23}, {58,0,17,23,9,22}, {76,0,17,23,9,22}, {94,0,20,21,12,20}, {0,0,16,23,10,22}, {17,0,19,23,9,22},
    {0,0,16,17,8,16}, {17,0,20,17,7,16}, {38,0,23,17,7,16}, {62,0,25,17,7,16}, {88,0,21,17,7,16}, {0,18,17,17,7,16}, {18,18,19,17,10,16}, {38,18,23,17,14,16},
    {0,0,18,21,9,20}, {19,0,20,17,11,16}, {40,0,20,17,11,16}, {61,0,20,16,11,15}, {0,0,17,25,11,24}, {18,0,19,25,12,23}, {38,0,18,25,13,24}, {57,0,20,25,15,24},
    {0,0,19,27,14,26}, {20,0,16,33,11,32}, {37,0,17,30,9,29}, {55,0,20,22,12,21}, {76,0,19,20,12,19}, {0,0,14,33,7,32}, {15,0,14,34,6,33}, {30,0,13,34,6,33},
    {44,0,14,34,7,33}, {59,0,14,33,8,32}, {74,0,15,32,9,31}, {90,0,14,32,8,31}, {105,0,14,32,7,31}, {0,0,13,31,6,30}, {14,0,18,31,5,30}, {33,0,18,31,5,30},
    {52,0,15,31,5,30}, {0,0,21,22,14,21}, {22,0,24,22,16,21}, {0,0,22,20,15,19}, {23,0,21,20,15,19}, {45,0,21,20,15,19}, {0,0,23,17,6,16}, {24,0,22,17,5,16},
    {47,0,23,17,6,16}, {71,0,22,17,5,16}, {94,0,22,17,6,16}, {0,18,23,17,7,16}, {0,0,22,22,11,16}, {23,0,20,22,9,21}, {44,0,19,22,7,21}, {64,0,17,22,7,21},
    {82,0,17,21,9,20}, {100,0,21,22,10,21}, {0,0,20,23,12,21}, {21,0,20,23,11,21}, {42,0,20,23,13,21}, {63,0,21,20,13,18}, {0,0,21,27,13,26}, {22,0,22,27,14,26},
    {45,0,20,27,12,26}, {66,0,20,27,12,26}, {87,0,21,27,13,26}, {0,28,19,27,11,26}, {0,0,16,23,10,22}, {17,0,19,23,9,22}, {0,0,18,17,10,16}, {19,0,20,17,7,16},
    {40,0,23,17,7,16}, {64,0,26,17,7,16}, {91,0,24,17,7,16}, {0,18,17,17,7,16}, {18,18,20,17,11,16}, {39,18,23,17,14,16}, {0,0,20,22,11,21}, {21,0,21,18,12,17},
    {43,0,20,18,11,17}, {64,0,20,17,11,16}, {0,0,18,33,11,32}, {19,0,19,33,12,32}, {39,0,18,33,13,32}, {58,0,20,33,15,32}, {0,0,20,33,15,32}, {21,0,16,33,11,32},
    {38,0,15,31,7,30}, {54,0,18,23,10,22}, {73,0,25,19,18,18}, {0,0,15,34,8,33}, {16,0,16,34,8,33}, {33,0,16,34,8,33}, {50,0,14,34,7,33}, {65,0,14,34,8,33},
    {80,0,15,34,9,33}, {96,0,14,34,8,33}, {111,0,15,34,8,33}, {0,0,13,31,7,30}, {14,0,18,31,13,30}, {33,0,18,31,13,30}, {52,0,14,31,9,30}, {0,0,21,23,14,22},
    {22,0,24,23,16,22}, {0,0,22,20,15,19}, {23,0,21,20,15,19}, {45,0,21,20,15,19}, {0,0,22,17,6,16}, {23,0,22,17,5,16}, {46,0,23,17,6,16}, {70,0,22,17,5,16},
    {93,0,22,17,6,16}, {0,18,23,17,7,16}, {0,0,25,22,14,21}, {26,0,20,22,9,21}, {47,0,20,22,8,21}, {68,0,22,22,8,21}, {91,0,22,22,10,21}, {0,23,22,22,11,21},
    {0,0,20,23,2,22}, {21,0,20,24,2,23}, {42,0,25,21,7,19}, {0,0,68,72,34,37}, {0,0,15,28,6,16}, {16,0,14,30,6,19}, {0,0,10,9,5,7}, {11,0,14,19,7,18},
    {26,0,25,28,13,27}, {52,0,28,37,14,36}, {81,0,34,44,17,43}, {0,45,34,45,17,44}, {0,0,31,31,16,15}, {32,0,31,31,16,15}, {64,0,31,31,16,15}, {0,32,31,31,16,15},
    {32,32,31,31,16,15}, {64,32,31,31,16,15}, {0,64,31,31,16,15}, {32,64,31,31,16,15}, {0,0,22,23,11,13}, {23,0,26,25,12,15}, {0,0,27,27,12,24}, {28,0,26,27,11,24},
    {55,0,26,26,12,23}, {0,0,10,9,5,4}, {11,0,9,10,5,6}, {0,0,23,17,1,16}, {24,0,24,13,2,12},
};
static const struct { unsigned short first; unsigned char n, img; } k_rec[58] = {   /* per record: first frame in k_frm, count, level-bank image (+0x190) */
    {0,5,8}, {5,8,9}, {13,2,10}, {15,4,11}, {19,3,12}, {22,4,13}, {26,5,14}, {31,8,15}, {39,3,16}, {42,2,17},
    {44,5,18}, {49,3,19}, {52,3,20}, {55,6,21}, {61,4,22}, {65,6,23}, {71,3,24}, {74,3,25}, {77,6,26}, {83,1,27},
    {84,9,28}, {93,1,29}, {94,1,30}, {95,9,31}, {104,6,32}, {110,2,33}, {112,8,34}, {120,4,35}, {124,4,36}, {128,5,37},
    {133,8,38}, {141,4,39}, {145,2,40}, {147,3,41}, {150,6,42}, {156,6,43}, {162,4,44}, {166,6,45}, {172,2,46}, {174,8,47},
    {182,4,48}, {186,4,49}, {190,5,50}, {195,8,51}, {203,4,52}, {207,2,53}, {209,3,54}, {212,6,55}, {218,6,56}, {224,3,57},
    {227,1,58}, {228,2,59}, {230,6,60}, {236,8,61}, {244,2,62}, {246,3,63}, {249,2,64}, {251,2,65},
};
static const unsigned char k_type_rec[13] = { 0, 13, 23, 36, 49, 50, 51, 52, 53, 54, 55, 56, 57 };   /* 0x4b7ea8: first record of each sprite type */

/* ---------------------------------------------------------------- the sprite base class (0x488180, 0x64 B) */
typedef struct {
    int rec0, type, state;          /* +0x00 first record of the type (0x4b7ea8), +0x08, +0x0c = animation = record rec0 + state */
    float x, y, z;                  /* +0x10; z = the ladder centre the last mask move met under the top row, 0 = none (0x488b80) */
    float vx, vy;                   /* +0x1c this frame's displacement */
    float dur, t;                   /* +0x24 animation length (0x488370), +0x28 animation time */
    int scale, frame, nframes, first;   /* +0x2c = 1, +0x30 frame as a dword offset (6 per frame), +0x34, +0x38 = k_frm index */
    int bb[4];                      /* +0x3c box around the position: left, top, right, bottom */
    int img;                        /* +0x4c level bank image */
    float base;                     /* +0x50 the y a jump / fall started from (0 = none) */
    int dir, pp;                    /* +0x54 facing (-1 = mirrored), +0x58 ping-pong direction of the idle loops */
    float jv, jd;                   /* +0x5c / +0x60 the falling integrator 0x4884b0 (25 px/s, +98 px/s^2) */
} Spr;
typedef struct { Spr s; int climb; uint8_t long_fall; int lives, lives_max; float invul; uint8_t blink; float spd, dt; } Hero;   /* 0x485a50, 0x88 B */
typedef struct { Spr s; int phase; float px, py; uint8_t armed; float sx0; uint8_t fwd, low, swoop; float spd, dt; } Buzz;    /* 0x486d60, 0x88 B */
typedef struct { Spr s; float spd, dt; } Kid;                                                                                  /* 0x487c90, 0x6c B */
typedef struct { Spr s; int on; } Slot;                                                                                        /* 0x68 B */

static struct {
    int ok;
    GLuint tex[66]; int tw[66], th[66];
    uint8_t *mask;                  /* +0x20: 4 blocks of 256 x 256 RGB, the first byte decides: 0 solid, 0x7f ladder, else free */
    Hero p; Buzz b; Spr potion, cage, icon; Kid sp, kn;
    Slot pool[10]; float pool_spd, pool_dt;   /* +0xc02e4 + 0x410 / 0x414 */
    uint8_t cage_open;              /* +0xc0760 */
    int level;                      /* +0xc0764: round 1..3 */
    float dt, timer, times[3], end_t;   /* +0xc0768, +0xc076c, +0xc0770.., +0xc077c */
    uint8_t steps, steps_on, step_cur; float step_t;   /* the footstep source Woody+0x84 (0x468e40 / 0x468e50), the port's chain clock and link */
} B;

/* ---------------------------------------------------------------- drawing */
static GLuint upload(const uint8_t *d, int w, int h, int bpp)   /* 0x480780: BGRA, bottom row first; 24 bit = opaque */
{
    uint8_t *px = malloc((size_t)w * h * 4); GLuint t;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const uint8_t *s = d + ((size_t)(h - 1 - y) * w + x) * 4; uint8_t *o = px + ((size_t)y * w + x) * 4;
        o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = bpp == 24 ? 255 : s[3];
    }
    glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);   /* GL_CLAMP_TO_EDGE: GL_CLAMP would mix the black border into the tile seams */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    uint64_t hash = tp_hash('I', px, (uint32_t)(w * h * 4), w, h);     /* port extra: a texture pack's PNG (texpack.c) */
    if (!tp_replace(hash, TP_ASIS)) { tp_dump(hash, px, w, h); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px); }
    free(px); return t;
}
/* RectVirtual 0x480a10 with the colour 0xfe808080 (0x488140) and flag 8 (alpha blend): destination x, y, w, h (w < 0 = mirrored),
 * source rectangle in texels. The original adds half a texel to D3D's integer pixel centres, which is GL's own convention. */
static void rect(int img, float x, float y, float w, float h, float sx, float sy, float sw, float sh)
{
    if (img < 0 || img >= 66 || !B.tex[img]) return;
    float W = (float)B.tw[img], H = (float)B.th[img], u0 = sx / W, u1 = (sx + sw) / W, v0 = sy / H, v1 = (sy + sh) / H;
    if (w < 0) { x += w; w = -w; float k = u0; u0 = u1; u1 = k; }
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, B.tex[img]); glColor4f(1, 1, 1, 254.0f / 255.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x, y); glTexCoord2f(u0, v1); glVertex2f(x, y + h);
    glTexCoord2f(u1, v1); glVertex2f(x + w, y + h); glTexCoord2f(u1, v0); glVertex2f(x + w, y);
    glEnd();
}
static void bg_draw(void)          /* 0x488880: images 0..3 (256 wide, source 255) and 4..7 (the right column, 128) */
{
    static const int k[8][2] = { {0,-16}, {256,-16}, {0,240}, {256,240}, {512,-16}, {512,112}, {512,240}, {512,368} };
    for (int i = 0; i < 8; i++) { float s = i < 4 ? 256.0f : 128.0f; rect(i, (float)k[i][0], (float)k[i][1], s, s, 0, 0, s - 1, s - 1); }
}
static const short *cur(const Spr *s) { return k_frm[s->first + s->frame / 6]; }
/* 0x488610: the frame's screen box against the play area 64..576 x 40..460. 0 = nothing of it inside; c = the clip in texture
 * space {left, top, right, bottom} (left and right swap for a mirrored sprite) */
static int clip(const Spr *s, int c[4])
{
    const short *f = cur(s); int X = (int)s->x, Y = (int)s->y, k = s->scale, left, right, top = Y - f[5] * k, bottom = f[3] * k + top;
    if (s->dir == 1) { left = X - f[4] * k; right = f[2] * k + left; } else { right = X + f[4] * k; left = right - f[2] * k; }
    c[0] = c[1] = c[2] = c[3] = 0;
    int t_in = top > 40, b_in = bottom < 460, l_in = left > 64, r_in = right < 576;
    if (t_in && b_in && l_in && r_in) return 1;
    if (!l_in && right < 64) return 0;
    if (!r_in && left > 576) return 0;
    if (!t_in && bottom < 40) return 0;
    if (!b_in && top > 460) return 0;
    if (!l_in) c[s->dir == 1 ? 0 : 2] = 64 - left;
    if (!r_in) c[s->dir == 1 ? 2 : 0] = right - 576;
    if (!t_in) c[1] = 40 - top;
    if (!b_in) c[3] = bottom - 460;
    return 1;
}
static void spr_draw(const Spr *s)   /* 0x4884e0 */
{
    int c[4]; if (!s->nframes || !clip(s, c)) return;
    const short *f = cur(s); int k = s->scale, X = (int)s->x, Y = (int)s->y;
    rect(s->img, (float)(X - s->dir * (k * f[4] - c[0])), (float)(Y - k * f[5] + c[1]), (float)(s->dir * (k * f[2] - c[2] - c[0])), (float)(k * f[3] - c[3] - c[1]),
         (float)(f[0] + c[0]), (float)(f[1] + c[1]), (float)(f[2] - c[2] - c[0]), (float)(f[3] - c[3] - c[1]));
}

/* ---------------------------------------------------------------- base class */
static void spr_anim(Spr *s, int st)  /* 0x4882a0 -> 0x488220: the frame list of record rec0 + state */
{
    int r = s->rec0 + st; s->state = st;
    if (r < 0 || r >= 58) { s->nframes = 0; return; }
    s->first = k_rec[r].first; s->nframes = k_rec[r].n; s->img = k_rec[r].img;
}
static void spr_ctor(Spr *s, int type)   /* 0x488180 */
{
    memset(s, 0, sizeof *s); s->rec0 = k_type_rec[type]; s->type = type; s->scale = 1; s->dir = 1; s->pp = 1; s->jv = 25.0f; spr_anim(s, 0);
}
static void spr_retype(Spr *s, int type) { s->rec0 = k_type_rec[type]; s->type = type; spr_anim(s, 0); s->frame = 0; s->t = 0; }   /* 0x4881e0 */
static void spr_dur(Spr *s, float d) { s->dur = d; s->t = 0; }                                                                       /* 0x488370 */
static void jump_reset(Spr *s) { s->jv = 25.0f; s->jd = 0; }                                                                        /* 0x4884a0 */
static float jump_step(Spr *s, float spd, float dt)   /* 0x4884b0: v += 98 t, d += v t (t = spd * dt); the displacement */
{
    float t = spd * dt, old = s->jd; s->jv += t * 98.0f; s->jd += t * s->jv; return s->jd - old;
}
static int anim_frame(const Spr *s) { return (int)((float)s->nframes * (s->t / s->dur)) * 6; }   /* ftol(n * t / dur) * 6 */
static void frame_wrap(Spr *s) { int n = s->nframes * 6; if (n) s->frame %= n; }
static void pp_frame(Spr *s) { if (s->pp == -1) s->frame = s->nframes * 6 - s->frame - 6; }   /* the backward half of an idle loop, for the draw */
static void base_set_state(Spr *s, int st)   /* 0x488010 (Knothead, Splinter) */
{
    if (s->pp == -1) s->pp = 1;
    spr_anim(s, st);
    switch (st) { case 0: spr_dur(s, 1.2f); break; case 1: spr_dur(s, 0.6f); break; case 5: spr_dur(s, 0.55f); break;
                  case 6: spr_dur(s, 15.0f); break; case 11: spr_dur(s, 0.8f); break; case 12: spr_dur(s, 1.0f); break; }
    s->frame = 0;
}
static void set_bb(Spr *s, int l, int t, int r, int b) { s->bb[0] = l; s->bb[1] = t; s->bb[2] = r; s->bb[3] = b; }
static void set_pos(Spr *s, float x, float y) { s->x = x; s->y = y; s->z = 0; }

/* ---------------------------------------------------------------- the mask (0x488790) */
static int mask_at(int x, int y)    /* 0x488e50: 1 solid, 2 ladder, 0 free. Quadrants of 256 x 256 cover 65..576 x -15..496 */
{
    int blk, lx, ly;
    if (x <= 320) { lx = x - 65; if (y <= 240) { blk = 0; ly = y + 15; } else { blk = 2; ly = y - 241; } }
    else { lx = x - 321; if (y <= 240) { blk = 1; ly = y + 15; } else { blk = 3; ly = y - 241; } }
    if (lx < 0 || lx > 255 || ly < 0 || ly > 255) return 1;   /* port: the original reads whatever lies next to the block */
    uint8_t v = B.mask[blk * 0x30000 + (ly * 256 + lx) * 3];
    return v == 0 ? 1 : v == 0x7f ? 2 : 0;
}
static float ladder_centre(int x, int y)   /* 0x488f00: the middle of the ladder at x, or of the nearest one along the row (0x488fa0) */
{
    int a = 0, b = 0, c = 0;
    if (mask_at(x, y) == 2) {
        int e = x; do { a++; e--; } while (mask_at(e, y) == 2 && a < 600);
        e = x; do { b++; e++; } while (mask_at(e, y) == 2 && b < 600);
        return (float)((a + b) / 2 - a + x);
    }
    int fl = 0, fr = 0, xl = x, xr = x;
    for (int n = 0; n < 600; n++) {     /* port: bounded; the original only gets here when the row has a ladder */
        if (fr) break;
        if (mask_at(xl, y) == 2) fl = 1; else { a++; xl--; }
        if (mask_at(xr, y) == 2) fr = 1; else { b++; xr++; }
        if (fl) break;
    }
    if (fl) { if (mask_at(x - a, y) == 2) { int e = x; do { c++; e--; } while (mask_at(e - a, y) == 2 && c < 600); } return (float)(x - c / 2 - a); }
    if (fr) { int e = b + x; if (mask_at(e, y) == 2) do { c++; e++; } while (mask_at(e, y) == 2 && c < 600); return (float)(c / 2 + b + x); }
    return (float)x;
}
/* 0x488b80: move s by (vx, vy) against the mask. The box is tested at the new place: its top and bottom rows, then (only if
 * those are clear) its left and right columns; anything solid refuses the whole move, except that a blocked side lets the y part
 * through when flag is 0 and vy != 0, a blocked bottom lets an upward and a blocked top a downward y part through. Leaving
 * 65..576 x 40..440 is a refusal too. z = the centre of a ladder met on the top row, else 0. */
static void mask_move(Spr *s, float vx, float vy, int *hit, int flag)
{
    const int *bb = s->bb; float nx = s->x + vx, ny = s->y + vy; int ix = (int)nx, iy = (int)ny;
    int left = ix - bb[0], top = iy - bb[1], right = left + bb[0] + bb[2], hgt = bb[1] + bb[3];
    int ladder = 0, side = 0, bot = 0, topb = 0; float lc = 0;
    *hit = 0;
    if (nx - bb[0] < 65.0f || nx + bb[2] > 576.0f || ny - bb[1] < 40.0f || ny + bb[3] > 440.0f) *hit = 1;
    else {
        for (int x = left; x < right; x++) {
            int r = mask_at(x, top);
            if (r == 1) { topb = 1; *hit = 1; break; }
            if (r == 2 && !ladder) { ladder = 1; lc = ladder_centre(ix, top); }
            if (mask_at(x, top + hgt) == 1) { bot = 1; *hit = 1; break; }
        }
        if (!*hit) for (int y = top; y < top + hgt; y++) if (mask_at(left, y) == 1 || mask_at(right, y) == 1) { *hit = 1; side = 1; break; }
    }
    if (!*hit) { s->x += vx; s->y += vy; }
    else if ((side && vy != 0 && !flag) || (bot && vy < 0) || (topb && vy > 0)) { s->y += vy; *hit = 0; }
    s->z = ladder ? lc : 0;
}

/* ---------------------------------------------------------------- Woody (0x485a50) */
#define P B.p
static float hop(float t) { float a = t - 0.05f; return a * (140.0f - a * 140.0f) + 7.0f; }   /* 0x486b80: the rise of a jump, 7..42 px */
/* the next fields (+0x10) of SoundFx 25..28 in the static table (0x4661a0): 25 -> 26 -> 27 -> 28 -> 25 at start-up, and never
 * reset, so the shuffles of every walking frame of every round add up for the whole process */
static uint8_t g_step_next[4] = { 1, 2, 3, 0 };
static void steps_shuffle(void)     /* 0x468b40(25, 4): four times swap the next fields of 25 + rand() % 4 and 25 + rand() % 4 */
{
    for (int k = 0; k < 4; k++) { int i = crt_mod(4), j = crt_mod(4); uint8_t t = g_step_next[i]; g_step_next[i] = g_step_next[j]; g_step_next[j] = t; }
}
static void steps_tick(void)        /* 0x468e50(src, 0, 25, fx, -1): the footstep chain from 25 on, one link every 0.3 s */
{
    if (B.steps && !B.steps_on) { B.steps_on = 1; B.step_t = 0; B.step_cur = 0; audio_fx(25, NULL, NULL); }
    else if (B.steps && B.dt > 0 && (B.step_t += B.dt) >= 0.3f) { B.step_t -= 0.3f; B.step_cur = g_step_next[B.step_cur]; audio_fx(25 + B.step_cur, NULL, NULL); }
    if (!B.steps && B.steps_on) { for (int i = 25; i <= 28; i++) audio_fx_stop(i, NULL, 0); B.steps_on = 0; }
    B.steps = 0;
}
static void hero_set_state(int st)  /* 0x4866d0 */
{
    Spr *s = &P.s;
    if (s->pp == -1) s->pp = 1;
    spr_anim(s, st);
    switch (st) {
    case 0: case 3: case 8: spr_dur(s, 1.2f); break;
    case 1: spr_dur(s, 0.6f); break;
    case 2: spr_dur(s, 0.2f); break;
    case 4: audio_fx(32, NULL, NULL); spr_dur(s, 0.15f); break;            /* ducking */
    case 5: audio_fx(21 + crt_mod(4), NULL, NULL); spr_dur(s, 0.55f); break;   /* jump (0x48672e) */
    case 6: spr_dur(s, 15.0f); break;                                      /* falling: the frame comes from 0x486c10 */
    case 7: case 12: spr_dur(s, 1.0f); break;
    case 9: audio_fx(34 + crt_mod(3), NULL, NULL); spr_dur(s, 0.4f); break;   /* hit (0x486780) */
    case 10: audio_fx(33, NULL, NULL); spr_dur(s, 1.5f); break;            /* last life gone */
    case 11: spr_dur(s, 0.8f); break;                                      /* holds up the potion */
    }
    s->frame = 0;
}
static int hero_crouched(void) { return P.s.state == 3 || (P.s.state == 4 && P.s.frame / 6 == P.s.nframes - 1); }   /* 0x486cf0 */
static int jump_frame(void) { float t = P.s.t; return t > 0.45f ? 3 : t > 0.35f ? 2 : t > 0.05f ? 1 : 0; }   /* 0x486bb0 */
static int fall_frame(void)         /* 0x486c10: 0 short fall, 1 fallen further than a jump rises, 2..4 the hard landing */
{
    if (P.s.base != 0) return P.s.base + hop(0.55f) >= P.s.y ? 0 : 1;
    return P.s.t > 14.9f ? 4 : P.s.t > 14.8f ? 3 : 2;
}
static void hero_anim_end(void)     /* 0x486370 */
{
    Spr *s = &P.s; int st = s->state;
    if (s->dur > s->t) return;
    switch (st) {
    case 4: hero_set_state(3); break;                                     /* down -> crouched */
    case 2: case 6: case 12: case 11: hero_set_state(0); break;
    case 9: jump_reset(s); if (P.lives == 0) hero_set_state(10); else { hero_set_state(6); s->base = s->y; } break;
    case 10: hero_set_state(0); set_pos(s, 85, 430); if (s->dir == -1) s->dir = 1; P.lives = P.lives_max; break;   /* back to the start, lives refilled */
    case 5: hero_set_state(6); s->base = s->y; s->vy = 0; break;
    case 1: case 7: s->t -= s->dur; break;
    case 0: case 3: case 8: s->pp = -s->pp; s->t -= s->dur; break;
    }
    s->frame = 0;
}
static void hero_state_vel(void)    /* 0x486560 */
{
    Spr *s = &P.s; float k = P.dt * P.spd;
    switch (s->state) {
    case 5: s->y = s->base; if (s->t > 0.05f) s->vy = -hop(s->t); break;  /* the jump is a height over its start, not a speed */
    case 1: B.steps = 1; steps_shuffle(); s->vx = s->dir * k * 80.0f; s->vy = 0; break;   /* 0x48662f: the shuffle, every walking frame */
    case 2: s->vx = s->vy = 0; break;
    case 7: s->vx = 0; s->vy = P.climb * k * -50.0f; break;
    }
    steps_tick();
}
static void hero_air(void) { P.s.vx = P.s.dir * P.dt * P.spd * 75.0f; }   /* 0x486ca0 */
static void hero_climb_start(int dir)   /* 0x486141 / 0x486239 with a ladder under the head */
{
    Spr *s = &P.s; int st = s->state;
    if (st == 0 || st == 1) s->y -= 2.0f;
    P.climb = dir;
    if (st == 7) return;
    s->x = s->z; s->z = 0; s->vx = 0; s->base = 0; hero_set_state(7);
}
static void hero_input(const int *k)   /* 0x485e70: actions 0 left, 1 right, 2 up, 3 down, 5 duck, 4 jump, 6 attack */
{
    Spr *s = &P.s; int st = s->state, L = k[0], R = k[1], U = k[2], D = k[3], DK = k[5], J = k[4], A = k[6];
    if (L || R) {                                                        /* both: right (0x485fa3) */
        int want = R ? 1 : -1;
        if (s->dir != want) { s->dir = -s->dir; if (st != 5 && st != 6 && st != 4 && st != 7 && st != 8) hero_set_state(2); }
        else if (st == 0 || ((st == 3 || st == 4) && !D)) hero_set_state(1);
        else if (st == 5 || st == 6) hero_air();
        else if (st == 8) { s->base = s->y; hero_set_state(5); }            /* off the ladder with a jump */
        else if (st == 7 && !U && !D) { hero_set_state(8); P.climb = 0; }
    } else if (!U && !D && !DK && !J) {
        if (A || st == 0 || st == 5 || st == 6) return;
        if (s->state == 7) { hero_set_state(8); P.climb = 0; return; }
        if (s->state != 8) hero_set_state(0);
        s->vx = s->vy = 0; return;
    }
    if (J) { st = s->state; if (st == 5 || st == 6) return; s->base = s->y; hero_set_state(5); return; }   /* from anywhere, even a ladder */
    if (U) { if (s->z != 0) hero_climb_start(1); return; }
    if (D || DK) {
        st = s->state;
        if (s->z == 0) { if (st == 0 || st == 1) { hero_set_state(4); s->vx = s->vy = 0; } return; }
        hero_climb_start(-1);
    }
}
static void hero_bbox(void)         /* 0x485db0: fixed sides and top per character, the bottom from the frame */
{
    static const int k[4][3] = { {8, 26, 11}, {8, 26, 11}, {9, 20, 8}, {10, 19, 8} };   /* type 0 Woody, 2 Knothead, 3 Splinter */
    Spr *s = &P.s; const short *f = cur(s); int t = s->type >= 0 && s->type < 4 ? s->type : 0;
    set_bb(s, k[t][0] * s->scale, k[t][1] * s->scale, k[t][2] * s->scale, s->scale * (f[3] - f[5]));
}
static void hero_frame(int frozen, const int *keys)   /* 0x485ac0 (frozen = +0xc0760: the cage is open, no more input) */
{
    Spr *s = &P.s;
    s->vx = s->vy = 0;
    pp_frame(s);
    if (P.invul == 0 || (P.blink = !P.blink)) spr_draw(s);                /* 0x485d30: every other frame while hit */
    s->t += P.spd * P.dt;
    if (s->state == 5) s->frame = jump_frame() * 6;
    else if (s->state == 6) { int old = s->frame; s->frame = fall_frame() * 6; if (old == 0 && s->frame == 6) P.long_fall = 1; }
    else s->frame = anim_frame(s);
    frame_wrap(s);
    hero_bbox();
    hero_anim_end();
    hero_state_vel();
    if (s->state != 9 && s->state != 10 && s->state != 11 && !frozen && P.dt != 0) hero_input(keys);
    if (s->state == 6 && s->t >= 14.7f) s->vx = s->vy = 0;                /* the hard landing */
    if (P.invul > 0) P.invul -= P.spd * P.dt; else P.invul = 0;
}
static void hero_land(int hit, float oldy)   /* 0x486860, after the falling step */
{
    Spr *s = &P.s; int st = s->state;
    if (!hit) {
        if (st == 6 || st == 5 || st == 7 || st == 8) return;
        s->base = oldy;
        if (st != 9 && st != 10) hero_set_state(6);                       /* walked off an edge */
        return;
    }
    if (st != 6 || s->base == 0) { jump_reset(s); return; }
    if (s->base + hop(0.55f) >= s->y) { hero_set_state(0); s->vx = s->vy = 0; }
    else if (P.long_fall && s->type == 0) { s->t = 0; s->frame = 0; s->base += 10.0f; return; }   /* the long pose is 10 px deeper and began this frame */
    else { if (s->type == 0) s->y += 10.0f; s->t = 14.7f; s->frame += 6; }                   /* hard landing: frames 2..4 for 0.3 s */
    s->base = 0; jump_reset(s);
    audio_fx(29 + crt_mod(3), NULL, NULL);                                  /* 0x486a09 */
}
static void hero_bump(void)         /* 0x486aa0: a jump that hits something falls from where it was the frame before */
{
    Spr *s = &P.s; float k = P.dt * P.spd;
    if (s->state != 5 || !(s->t > 0.05f)) return;
    s->y = s->t - k > 0.05f ? s->base - hop(s->t - k) : s->base;
    hero_set_state(6); s->base = s->y;
}
static void hero_hurt(void)         /* 0x484898 / 0x484923 */
{
    hero_set_state(9); P.lives--; P.invul = 1.5f;                         /* 0x485d80 */
}
static void hero_lives(int level) { P.lives_max = level == 1 ? 3 : level == 2 ? 2 : level == 3 ? 1 : P.lives_max; P.lives = P.lives_max; }   /* 0x485d40 */

/* ---------------------------------------------------------------- Knothead and Splinter (0x487c90) */
static void kid_random(Spr *s)      /* 0x4880b0 */
{
    switch (crt_mod(4)) {                                                 /* 0x4880b5 */ case 0: base_set_state(s, 1); break; case 1: base_set_state(s, 5); s->base = s->y; break;
                          case 2: base_set_state(s, 12); break; default: base_set_state(s, 11); break; }
}
static void kid_frame(Kid *k)       /* 0x487ce0 */
{
    Spr *s = &k->s; float m = k->dt * k->spd, vx = 0, vy = 0;
    pp_frame(s); spr_draw(s);
    if (s->state != 6) s->t += m;
    s->frame = anim_frame(s); frame_wrap(s);
    if (s->dur <= s->t) {                                                 /* 0x487e00 */
        if (s->state == 0) { s->pp = -s->pp; s->t -= s->dur; }
        else if (s->state == 1 || s->state == 12 || s->state == 11) kid_random(s);
        else if (s->state == 5) base_set_state(s, 6);
    }
    switch (s->state) {                                                   /* 0x487e90 */
    case 1: case 11: case 12: vx = s->dir * m * 80.0f; break;
    case 5: vy = s->dir * m * -80.0f; break;
    case 6: vy = s->dir * m * 80.0f; if (s->base < s->y + vy) { s->base = 0; kid_random(s); } break;
    }
    if (((s->x > 490.0f && s->type == 3) || (s->x > 515.0f && s->type == 2)) && s->state != 0) base_set_state(s, 0);   /* far enough right */
    s->vx = vx; s->vy = vy; set_pos(s, s->x + vx, s->y + vy);
}

/* ---------------------------------------------------------------- the pool (0x485330): shots, rocks, dynamite, explosions, stars */
enum { T_DYNAMITE = 6, T_BOOM = 8, T_STAR = 9, T_ROCK = 10, T_SHOT = 11 };
static Spr *pool_alloc(void)        /* 0x485450: the first free slot (the original has no bound check) */
{
    for (int i = 0; i < 10; i++) if (!B.pool[i].on) { B.pool[i].on = 1; return &B.pool[i].s; }
    return NULL;
}
static void pool_free(Spr *s) { ((Slot *)s)->on = 0; }                 /* 0x485480 */
static int pool_empty(void) { for (int i = 0; i < 10; i++) if (B.pool[i].on) return 0; return 1; }   /* 0x485490 */
static void pool_move(float py)     /* 0x4854b0 */
{
    for (int i = 0; i < 10; i++) {
        Spr *s = &B.pool[i].s; int hit;
        if (!B.pool[i].on) continue;
        mask_move(s, s->vx, s->vy, &hit, 0);
        if (hit) {
            if (s->type == T_SHOT) { if (s->frame == 0) s->frame = 6; else if (s->frame == 6) pool_free(s); }   /* the flash, then gone */
            else if (s->type == T_ROCK) { s->vx = s->vy = 0; set_pos(s, s->x + 10.0f, s->y - 10.0f); set_bb(s, 0, 0, 0, 0); spr_retype(s, T_STAR); spr_dur(s, 0.2f); audio_fx(37, NULL, NULL); }
            else if (s->type == T_DYNAMITE) set_pos(s, s->x + s->vx, s->y + s->vy);   /* dynamite falls through everything */
        }
        if ((py - 10.0f < s->y || s->y > 265.0f) && s->type == T_DYNAMITE) {  /* below Woody's head or the upper floor: it goes off */
            s->vx = s->vy = 0; spr_retype(s, T_BOOM); spr_dur(s, 0.8f); set_bb(s, 14, 10, 12, 12); audio_fx(6, NULL, NULL);
        }
    }
}
static void pool_update(float py)   /* 0x485370 */
{
    float m = B.pool_dt * B.pool_spd;
    pool_move(py);
    for (int i = 0; i < 10; i++) {
        Spr *s = &B.pool[i].s; if (!B.pool[i].on) continue;
        spr_draw(s); s->t += m;
        if (s->type != T_SHOT) s->frame = anim_frame(s);
        frame_wrap(s);
    }
    for (int i = 0; i < 10; i++) {      /* 0x485990 */
        Spr *s = &B.pool[i].s; if (!B.pool[i].on || s->dur > s->t) continue;
        if (s->type == T_ROCK || s->type == T_DYNAMITE) s->t -= s->dur; else if (s->type == T_BOOM || s->type == T_STAR) pool_free(s);
    }
    for (int i = 0; i < 10; i++) {      /* 0x485870 */
        Spr *s = &B.pool[i].s; if (!B.pool[i].on) continue;
        if (s->type == T_DYNAMITE) { s->vx = 0; s->vy = jump_step(s, B.pool_spd, B.pool_dt); }
        else if (s->type == T_ROCK) { if (s->x > 102.0f) { s->vx = m * 60.0f; s->vy = 0; } else if (s->t > 0.2f) set_pos(s, 107.0f, s->y); }   /* out of Buzz's hands, then rolling */
        else if (s->type == T_SHOT && s->frame == 0) { s->vx = m * -120.0f; s->vy = 0; }
    }
}
static int overlap(const int a[4], const int b[4]) { return !(b[0] > a[2] || b[2] < a[0] || b[1] > a[3] || b[3] < a[1]); }
static void box_of(const Spr *s, int out[4]) { int X = (int)s->x, Y = (int)s->y; out[0] = X - s->bb[0]; out[1] = Y - s->bb[1]; out[2] = X + s->bb[2]; out[3] = Y + s->bb[3]; }
static int pool_hits(int crouch)    /* 0x485720: shots fly over a crouching Woody, lit dynamite and the fading explosion are harmless */
{
    int w[4], o[4]; box_of(&P.s, w);
    for (int i = 0; i < 10; i++) {
        const Spr *s = &B.pool[i].s; if (!B.pool[i].on) continue;
        box_of(s, o); if (!overlap(w, o)) continue;
        if ((crouch && s->type == T_SHOT) || s->type == T_DYNAMITE || (s->type == T_BOOM && s->frame > 12)) return 0;
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- Buzz (0x486d60) */
#define Z B.b
static void buzz_set_state(int st)  /* 0x487460 */
{
    static const float k[10] = { 1.0f, 0.8f, 1.0f, 0.8f, 0.6f, 1.2f, 1.4f, 1.6f, 1.8f, 0.8f };
    Spr *s = &Z.s;
    if (s->pp == -1) s->pp = 1;
    spr_anim(s, st);
    if (st == 4) Z.armed = 1;
    if (st >= 0 && st < 10) spr_dur(s, k[st]);
    s->frame = 0;
}
static void buzz_ai(void)           /* 0x487500: where Woody is decides Buzz's attack (+0x64) */
{
    Spr *s = &Z.s; int st = s->state; float px = Z.px, py = Z.py;
    if (px > 130 && px < 495 && py > 355) {                               /* the ground floor: he walks in from the right and shoots */
        if (Z.phase == 1) return;
        Z.phase = 1; buzz_set_state(2); if (s->dir == 1) s->dir = -1; set_pos(s, 598, 435);
    } else if (py < 390 && px > 175 && py > 265) {                        /* the middle floor: from the left, throws rocks */
        if (Z.phase == 2) return;
        Z.phase = 2; buzz_set_state(2); if (s->dir == -1) s->dir = 1; set_pos(s, 42, 353);
    } else if (py < 265 && py > 150 && px > 120 && px < 520 && st != 7) { /* the upper floor: hops along above him with dynamite */
        if (Z.phase == 3) return;
        buzz_set_state(5); if (s->dir == 1) s->dir = -1;
        if (Z.phase != 0) set_pos(s, 598, 94);
        Z.phase = 3;
    } else if (py < 150 && px < 460 && px > 140) {                        /* the floating rocks: swoops down on him */
        if (Z.phase == 4 || Z.phase != -1) return;
        Z.phase = 4; buzz_set_state(6); if (s->dir == -1) s->dir = 1;
        if (!Z.fwd) Z.fwd = 1;
        if (!Z.low) Z.low = 1;
        set_pos(s, Z.px, 20); Z.swoop = 1;
    } else {                                                              /* elsewhere: leave (+0x64 = 0) */
        if (Z.phase == -1 || Z.phase == 0 || st == 7) return;
        Z.phase = 0;
        if (st == 2) s->dir = -s->dir;
        else if (st == 3 || st == 4 || st == 9 || st == 8) { buzz_set_state(2); s->dir = -s->dir; }
        else if (st == 5) { if (s->dir == 1) s->dir = -1; if (s->x > 320) s->dir = -s->dir; }   /* towards the nearer side */
    }
}
static void buzz_anim_end(void)     /* 0x486fa0 */
{
    Spr *s = &Z.s; int st = s->state;
    if (s->dur > s->t) return;
    switch (st) {
    case 2: case 5: s->t -= s->dur; break;
    case 3: buzz_set_state(9); break;  case 9: buzz_set_state(3); break;  /* shoot / reload */
    case 4: buzz_set_state(8); break;  case 8: buzz_set_state(4); break;  /* throw / wait */
    case 6: s->t -= s->dur; Z.swoop = 1; break;                           /* hidden above the screen, then the next swoop */
    case 7: buzz_set_state(6); Z.swoop = 0; s->base = 0; Z.fwd = !Z.fwd; Z.low = !Z.low; break;
    }
}
static void buzz_move(void)         /* 0x4870b0: the displacement per attack */
{
    Spr *s = &Z.s; int st = s->state; float m = Z.dt * Z.spd;
    switch (Z.phase) {
    case -1: s->vx = s->vy = 0; jump_reset(s); break;
    case 0:
        if (s->x < 45 || s->x > 595 || st == 6) Z.phase = -1;
        else if (st == 2) s->vx = s->dir * m * 80.0f;
        else if (st == 5) s->vx = s->dir * jump_step(s, Z.spd, Z.dt);
        break;
    case 1: if (st == 2) { if (s->x < 555) { buzz_set_state(3); set_pos(s, 555, 435); } else s->vx = s->dir * m * 80.0f; } break;
    case 2: if (st == 2) { if (s->x > 85) { buzz_set_state(4); set_pos(s, 85, 353); } else s->vx = s->dir * m * 80.0f; } break;
    case 3:                                                               /* chases him, gathering speed, turns 50 px past him */
        if (Z.px + 50 < s->x) { if (s->dir == 1) { s->dir = -1; jump_reset(s); } }
        else if (Z.px - 50 > s->x && s->dir == -1) { s->dir = 1; jump_reset(s); }
        s->vx = s->dir * jump_step(s, Z.spd, Z.dt);
        break;
    case 4:
        if (Z.swoop) {
            if (st != 7) {
                buzz_set_state(7);
                if (Z.fwd) { Z.sx0 = Z.px + 80; if (s->dir == 1) s->dir = -1; } else { Z.sx0 = Z.px - 80; if (s->dir == -1) s->dir = 1; }
                set_pos(s, Z.sx0, 20); s->base = 20;
            }
            float k = s->t * 100.0f;                                      /* 0x487ae0 / 0x487b20: 160 px across, down to 100 or 130 and back */
            s->vx = Z.fwd ? Z.sx0 - s->x - k : k - (s->x - Z.sx0);
            s->vy = (Z.low ? s->t * (200.0f - s->t * 125.0f) : s->t * (275.0f - s->t * 171.875f)) + s->base - s->y;
        } else { s->vx = Z.px - s->x; s->vy = 0; }
        break;
    }
}
static void buzz_frame(void)        /* 0x486dd0 */
{
    Spr *s = &Z.s; const short *f;
    s->vx = s->vy = 0;
    if (Z.phase != -1 && s->state != 6) spr_draw(s);
    s->t += Z.dt * Z.spd;
    s->frame = anim_frame(s); frame_wrap(s);
    f = cur(s); set_bb(s, 16 * s->scale, 28 * s->scale, 12 * s->scale, s->scale * (f[3] - f[5]));   /* 0x486f30 */
    buzz_anim_end();
    buzz_move();
    set_pos(s, s->x + s->vx, s->y + s->vy);
}
static int buzz_hits(int crouch)    /* 0x487ba0: only the swoop; crouching gets under the shallow one */
{
    int w[4], b[4], X = (int)Z.s.x, Y = (int)Z.s.y, px = (int)Z.px, py = (int)Z.py;
    if (Z.s.state != 7) return 0;
    w[0] = px - P.s.bb[0]; w[1] = py - P.s.bb[1]; w[2] = P.s.bb[2] + px; w[3] = P.s.bb[3] + py;
    b[0] = X - 10; b[1] = Y - 2; b[2] = X + 15; b[3] = Y + 15;
    if (!(b[0] <= w[2] && b[2] >= w[0] && b[1] <= w[3] && b[3] >= w[1])) return 0;
    return !(crouch && Z.low);
}
static void buzz_fire(void)         /* 0x487800: the shot, the rock and the dynamite leave his hands on a set frame */
{
    Spr *s = &Z.s, *o; int st = s->state;
    if (Z.phase <= 0) return;
    if (st == 3) {
        if (s->frame == 0) Z.armed = 1;
        if (s->frame != 6 || !Z.armed) return;
        audio_fx(18, NULL, NULL);
        if ((o = pool_alloc()) != NULL) {
            set_pos(o, s->x - 6, s->y - 20); o->vx = o->vy = 0; spr_dur(o, 15.0f); set_bb(o, 4, 3, 2, 3); spr_retype(o, T_SHOT);
            if (o->dir == 1) o->dir = -1;
        }
        Z.armed = 0;
    } else if (st == 4 || st == 5) {
        if (s->frame == 0 && Z.armed) {
            if ((o = pool_alloc()) != NULL) {
                if (st == 4) { set_pos(o, s->x + 11, s->y - 1); o->vx = o->vy = 0; spr_dur(o, 0.6f); set_bb(o, 10, 22, 12, 0); spr_retype(o, T_ROCK); }
                else { set_pos(o, s->x, s->y); o->vx = o->vy = 0; spr_dur(o, 0.4f); spr_retype(o, T_DYNAMITE); }   /* no box of its own: the slot keeps the last one */
                if (o->dir == -1) o->dir = 1;
                if (st == 5) jump_reset(o);
            }
            Z.armed = 0;
        }
        if (s->frame == 6 && !Z.armed) Z.armed = 1;
    }
}

/* ---------------------------------------------------------------- the round */
static float level_speed(void) { return B.level == 2 ? 1.4f : B.level == 3 ? 1.7f : 1.0f; }   /* 0x484ef0 */
static void set_speeds(void)        /* 0x484f20 */
{
    float k = level_speed(); B.pool_spd = k; Z.spd = k; B.kn.spd = k; B.sp.spd = k; P.spd = k;
}
static void set_dt(float dt) { B.dt = dt; B.pool_dt = dt; Z.dt = dt; B.kn.dt = dt; B.sp.dt = dt; P.dt = dt; }   /* 0x484f80 */
static void round_setup(void)       /* 0x484530 */
{
    set_pos(&P.s, 85, 430); set_pos(&Z.s, 600, 435);
    set_pos(&B.potion, 545, 107); set_bb(&B.potion, 1, 16, 22, 1);
    set_pos(&B.cage, 93, 233);
    set_pos(&B.sp.s, 84, 261); base_set_state(&B.sp.s, 0);
    set_pos(&B.kn.s, 104, 261); base_set_state(&B.kn.s, 0);
    set_pos(&B.icon, 125, 62);
    B.icon.frame = P.s.type == 0 ? 12 : P.s.type == 2 ? 6 : 0;            /* the head of the character that plays: Woody */
    set_speeds();
}
static void cage_blow(void)         /* 0x484c30: five explosions over the cage */
{
    static const float k[5][3] = { {94, 233, 0.8f}, {110, 240, 1.4f}, {80, 210, 1.6f}, {110, 225, 1.0f}, {80, 255, 1.8f} };
    for (int i = 0; i < 5; i++) {
        Spr *o = pool_alloc(); if (!o) continue;
        set_pos(o, k[i][0], k[i][1]); o->vx = o->vy = 0; spr_dur(o, k[i][2]); spr_retype(o, T_BOOM); audio_fx(6, NULL, NULL);
    }
}
static void potion_touch(void)      /* 0x484b20 */
{
    int w[4], p[4]; box_of(&P.s, w); box_of(&B.potion, p);
    if (p[0] > w[2] || p[2] < w[0] || p[1] > w[3] || p[3] < w[1]) return;
    set_pos(&P.s, B.potion.x, B.potion.y); hero_set_state(11); if (P.s.dir == -1) P.s.dir = 1;
}
static void next_round(void)        /* 0x484e40 */
{
    B.level++; round_setup(); B.cage_open = 0; B.potion.frame = 0;
    if (P.s.dir == -1) P.s.dir = 1;
    hero_lives(B.level);
}
static void hud_texts(void)         /* 0x484ff0: "Level" n, the head, the lives; size 15, black */
{
    float x = 64.0f, y = 40.0f;
    x = hud_pen_text(x, y, 15.0f, 0xff000000u, 127, 0);
    x = hud_pen_text(x + 3.0f, y, 15.0f, 0xff000000u, 0, B.level);
    set_pos(&B.icon, x + 12.0f, 62.0f);
    hud_pen_text(x + 34.0f, y, 15.0f, 0xff000000u, 0, P.lives);
}
static void end_texts(void)         /* 0x485130: "Level n  t Seconds", after round 3 also "Final Score  total Seconds"; size 30 */
{
    float x = 200.0f, y = 275.0f;
    x = hud_pen_text(x, y, 30.0f, 0xfe000000u, 127, 0);
    x = hud_pen_text(x + 10.0f, y, 30.0f, 0xfe000000u, 0, B.level);
    x = hud_pen_text(x + 30.0f, y, 30.0f, 0xfe000000u, 0, (int)B.times[B.level - 1]);
    hud_pen_text(x + 10.0f, y, 30.0f, 0xfe000000u, 128, 0);
    if (B.level != 3) return;
    y += 30.0f; x = 155.0f;
    x = hud_pen_text(x, y, 30.0f, 0xfe000000u, 129, 0);
    x = hud_pen_text(x + 30.0f, y, 30.0f, 0xfe000000u, 0, (int)(B.times[0] + B.times[1] + B.times[2]));
    hud_pen_text(x + 10.0f, y, 30.0f, 0xfe000000u, 128, 0);
}

int bb_frame(int menu, float dt, const int held[7])   /* 0x4846d0 */
{
    int hit;
    if (!B.ok) return 0;
    set_dt(menu ? 0 : dt);
    {   /* testing: WOODY_BBPOS="T x y [T x y ...]" puts Woody on x y T s into the BlackBox, once each */
        static float clk; static unsigned done; const char *e = getenv("WOODY_BBPOS"); float t, x, y; int n, k = 0;
        clk += dt;
        while (e && sscanf(e, "%f %f %f%n", &t, &x, &y, &n) == 3 && k < 32) { if (!(done >> k & 1) && clk >= t) { done |= 1u << k; set_pos(&P.s, x, y); } e += n; k++; }
    }
    if (getenv("WOODY_BBLOG")) {                                          /* testing: the round state 4 times a second */
        static float lt; int n = 0; for (int i = 0; i < 10; i++) n += B.pool[i].on;
        if ((lt += dt) >= 0.25f) { lt = 0; printf("BB lvl %d woody st %d pos %.1f %.1f z %.0f dir %d lives %d inv %.2f | buzz ph %d st %d pos %.0f %.0f | pool %d | cage %d t %.2f end %.2f\n",
            B.level, P.s.state, P.s.x, P.s.y, P.s.z, P.s.dir, P.lives, P.invul, Z.phase, Z.s.state, Z.s.x, Z.s.y, n, B.cage_open, B.timer, B.end_t); }
    }
    hud_rect(0xff000000u);                                               /* port: black under the picture (the sides of a wide view) */
    bg_draw();
    kid_frame(&B.sp); kid_frame(&B.kn);
    spr_draw(&B.icon);
    hero_frame(B.cage_open, held);
    {   int st = P.s.state;
        if (st != 7 && st != 8 && st != 5 && st != 11 && st != 12) {      /* gravity: 0x488df0 with the falling integrator */
            float oldy = P.s.y, d = jump_step(&P.s, level_speed(), B.dt);
            hit = menu;                                                   /* a 0 step leaves the flag alone: it still holds the argument (0x4846f0) */
            if (d != 0) mask_move(&P.s, 0, d, &hit, P.long_fall);
            hero_land(hit, oldy);
        }
    }
    mask_move(&P.s, P.s.vx, P.s.vy, &hit, 0);
    if (hit) hero_bump();
    if (P.s.state == 7) jump_reset(&P.s);
    P.long_fall = 0;                                                      /* 0x486850 */
    Z.px = P.s.x; Z.py = P.s.y;                                           /* 0x486f10 */
    buzz_ai(); buzz_frame();
    if (P.s.state != 9 && P.s.state != 10 && buzz_hits(hero_crouched()) && P.invul == 0) hero_hurt();
    buzz_fire();
    pool_update(P.s.y);
    if (P.s.state != 9 && P.s.state != 10 && pool_hits(hero_crouched()) && P.invul == 0) hero_hurt();
    if (P.s.state != 11) { spr_draw(&B.potion); B.timer += B.dt; }
    else if (B.potion.frame == 0) B.potion.frame = 6;                     /* taken: the bottle shows its second frame from now on */
    if (B.potion.frame != 0 && P.s.state != 11) {
        if (!B.cage_open) { spr_draw(&B.cage); cage_blow(); B.cage_open = 1; hud_texts(); return 0; }
        if (pool_empty()) {
            if (B.times[B.level - 1] == 0) B.times[B.level - 1] = B.timer;
            B.timer = 0;
            if (B.kn.s.state == 0 && B.sp.s.state == 0 && B.kn.s.x < 320 && B.sp.s.x < 320) { base_set_state(&B.kn.s, 1); base_set_state(&B.sp.s, 1); }   /* free: they walk off to the right */
            end_texts();
            if ((B.end_t += B.dt) > 5.0f) {
                B.end_t = 0;
                if (B.level == 3) return 1;                               /* 0x401d31: RequestLevel(0.5, 0x1a, 0, 0x20) */
                next_round();
            }
        }
    } else { spr_draw(&B.cage); if (P.s.state != 11) potion_touch(); }
    hud_texts();
    return 0;
}

/* ---------------------------------------------------------------- setup */
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int load_images(const char *path)   /* the level bank's type-1 items (docs/RCK.md) */
{
    FILE *f = fopen(path, "rb"); uint8_t h[0x38]; int n = 0;
    if (!f) return -1;
    if (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "RKET", 4)) { fclose(f); return -1; }
    for (int type = 0; type < 2; type++) {
        uint32_t count = rd32(h + 8 + 0x18 + type * 4);
        for (uint32_t i = 0; i < count; i++) {
            uint8_t ih[8]; if (fread(ih, 1, 8, f) != 8) { fclose(f); return -1; }
            uint32_t size = rd32(ih);
            if (type == 0 || i >= 66 || size < 8) { fseek(f, (long)size, SEEK_CUR); continue; }
            uint8_t *d = malloc(size); if (fread(d, 1, size, f) != size) { free(d); fclose(f); return -1; }
            int w = (int16_t)(d[0] | d[1] << 8), hh = (int16_t)(d[2] | d[3] << 8), bpp = d[4] | d[5] << 8;
            if (w > 0 && hh > 0 && size >= 8 + (uint32_t)w * hh * 4) { B.tex[i] = upload(d + 8, w, hh, bpp); B.tw[i] = w; B.th[i] = hh; n++; }
            free(d);
        }
    }
    fclose(f); return n;
}
int bb_init(const char *data_dir)   /* 0x484420 */
{
    char path[600]; FILE *f;
    bb_free();
    snprintf(path, sizeof path, "%s/../Game/mask.bin", data_dir);          /* 0x4887f6: \Game\mask.bin, 0xc0000 B */
    B.mask = malloc(0xc0000);
    if (!(f = fopen(path, "rb")) || fread(B.mask, 1, 0xc0000, f) != 0xc0000) { if (f) fclose(f); fprintf(stderr, "BlackBox: cannot read %s\n", path); bb_free(); return -1; }
    fclose(f);
    snprintf(path, sizeof path, "%s/BlackBox/Blackbox.rck", data_dir);
    if (load_images(path) < 66) { fprintf(stderr, "BlackBox: images missing in %s\n", path); bb_free(); return -1; }
    spr_ctor(&P.s, 0); hero_set_state(0); P.lives_max = P.lives = 3;      /* 0x485a50 */
    spr_ctor(&Z.s, 1); buzz_set_state(2); Z.armed = Z.fwd = Z.low = 1; Z.phase = -1;   /* 0x486d60 */
    spr_ctor(&B.potion, 12); spr_ctor(&B.cage, 5);
    spr_ctor(&B.sp.s, 3); base_set_state(&B.sp.s, 0);                     /* 0x487c90 */
    spr_ctor(&B.kn.s, 2); base_set_state(&B.kn.s, 0);
    for (int i = 0; i < 10; i++) { Spr *s = &B.pool[i].s; memset(s, 0, sizeof *s); s->scale = 1; s->dir = 1; s->pp = 1; s->jv = 25.0f; }   /* 0x488150 */
    spr_ctor(&B.icon, 4);
    B.level = 1;
    round_setup();
    B.ok = 1; printf("BlackBox: mask + 66 images\n");
    return 0;
}
void bb_free(void)
{
    for (int i = 0; i < 66; i++) if (B.tex[i]) glDeleteTextures(1, &B.tex[i]);
    if (B.steps_on) for (int i = 25; i <= 28; i++) audio_fx_stop(i, NULL, 0);
    free(B.mask); memset(&B, 0, sizeof B);
}
int bb_active(void) { return B.ok; }
