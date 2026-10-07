/* touch.c - the on-screen pad (touch.h, PORT EXTRA). Sizes follow the short side of the screen (u), so the controls are
 * the same physical size on a phone in landscape whatever its aspect. */
#ifndef _WIN32
#include "touch.h"
#include <SDL.h>
#include <GL/gl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { G_TRI_UP, G_TRI_DOWN, G_DIAMOND, G_RING, G_CAMERA, G_EYE, G_PAUSE };
typedef struct { int pad, glyph, face; float x, y, r, rgb[3]; } Btn;
#define NBTN 7
#define NFING 10
static struct {
    int init, enabled, shown;
    float W, H;
    struct { SDL_FingerID id; int ctl, seen, up; } f[NFING]; int nf;   /* ctl: -1 nothing, 0 the stick, 1 + i button i; seen: the
                                                                      * game has had a frame of it; up: lifted before that, goes after it */
    int stick; float sx, sy, kx, ky;                          /* the stick's finger: base and knob, pixels */
} T;

static float unit(void) { return T.W < T.H ? T.W : T.H; }
static void layout(Btn *b)
{
    float u = unit(), cx = T.W - 0.27f * u, cy = T.H - 0.27f * u, d = 0.15f * u, r = 0.075f * u, top = cy - d - 0.2f * u;
    const Btn k[NBTN] = {
        { PAD_A, G_TRI_UP,   1, cx,               cy + d,         r,          { 0.25f, 0.78f, 0.30f } },   /* jump / confirm */
        { PAD_B, G_TRI_DOWN, 1, cx + d,           cy,             r,          { 0.88f, 0.25f, 0.22f } },   /* duck / back */
        { PAD_X, G_DIAMOND,  1, cx - d,           cy,             r,          { 0.25f, 0.50f, 0.92f } },   /* attack */
        { PAD_Y, G_RING,     1, cx,               cy - d,         r,          { 0.95f, 0.80f, 0.20f } },   /* special */
        { PAD_LB, G_CAMERA,  0, cx - 0.12f * u,   top,            0.06f * u,  { 0.70f, 0.70f, 0.70f } },   /* camera behind */
        { PAD_RB, G_EYE,     0, cx + 0.12f * u,   top,            0.06f * u,  { 0.70f, 0.70f, 0.70f } },   /* look around */
        { PAD_START, G_PAUSE, 0, T.W - 0.08f * u, 0.08f * u,      0.05f * u,  { 0.70f, 0.70f, 0.70f } } }; /* pause */
    memcpy(b, k, sizeof k);
}
static int hit(float x, float y, int faces_only)
{
    Btn b[NBTN]; layout(b); int best = -1; float bd = 0;
    for (int i = 0; i < NBTN; i++) {
        if (faces_only && !b[i].face) continue;
        float dx = x - b[i].x, dy = y - b[i].y, d2 = dx * dx + dy * dy, R = b[i].r * (b[i].face ? 1.3f : 1.5f);
        if (d2 < R * R && (best < 0 || d2 < bd)) { best = i; bd = d2; }
    }
    return best;
}
static void check_init(void)
{
    if (T.init) return;
    T.init = 1;
#ifdef __ANDROID__
    T.enabled = 1; T.shown = 1;
#elif defined __SWITCH__
    T.enabled = 1;                                                          /* the handheld's screen: shown once it is touched */
#else
    const char *e = getenv("WOODY_TOUCH"); T.enabled = e && atoi(e) != 0;
#endif
    T.stick = -1;
}

static void drop(int k)
{
    if (T.stick == k) T.stick = -1;
    T.f[k] = T.f[--T.nf];
    if (T.stick == T.nf) T.stick = k;                                       /* the stick's finger moved into the freed slot */
}
void touch_event(const void *ev, int width, int height)
{
    const SDL_Event *e = (const SDL_Event *)ev; check_init();
    if (!T.enabled) return;
    T.W = (float)width; T.H = (float)height;
    if (e->type == SDL_KEYDOWN && e->key.keysym.scancode != SDL_SCANCODE_AC_BACK) { T.shown = 0; return; }   /* a keyboard */
    if (e->type != SDL_FINGERDOWN && e->type != SDL_FINGERUP && e->type != SDL_FINGERMOTION) return;
    float x = e->tfinger.x * T.W, y = e->tfinger.y * T.H; int k = -1;
    { static int log = -1; if (log < 0) log = getenv("WOODY_TOUCHLOG") != NULL;   /* testing: the finger events */
      if (log) printf("touch %s id %lld at %.0f %.0f (%.3f %.3f of %.0fx%.0f)\n", e->type == SDL_FINGERDOWN ? "down" : e->type == SDL_FINGERUP ? "up" : "move",
                      (long long)e->tfinger.fingerId, x, y, e->tfinger.x, e->tfinger.y, T.W, T.H); }
    for (int i = 0; i < T.nf; i++) if (T.f[i].id == e->tfinger.fingerId) k = i;
    if (e->type == SDL_FINGERDOWN) {
        T.shown = 1;
        if (k < 0) { if (T.nf == NFING) return; k = T.nf++; T.f[k].id = e->tfinger.fingerId; }
        T.f[k].seen = T.f[k].up = 0;
        int b = hit(x, y, 0);
        if (b >= 0) T.f[k].ctl = 1 + b;
        else if (x < T.W * 0.5f && T.stick < 0) { T.f[k].ctl = 0; T.stick = k; T.sx = T.kx = x; T.sy = T.ky = y; }
        else T.f[k].ctl = -1;
    } else if (k >= 0 && e->type == SDL_FINGERMOTION) {
        if (T.f[k].ctl == 0) {
            float R = 0.14f * unit(), dx = x - T.sx, dy = y - T.sy, d = sqrtf(dx * dx + dy * dy);
            if (d > R) { T.sx = x - dx / d * R; T.sy = y - dy / d * R; }   /* the base follows a finger that goes further */
            T.kx = x; T.ky = y;
        } else if (T.f[k].ctl > 0) {                                        /* slide between the face buttons */
            Btn bs[NBTN]; layout(bs);
            if (bs[T.f[k].ctl - 1].face) { int b = hit(x, y, 1); if (b >= 0) T.f[k].ctl = 1 + b; }
        }
    } else if (k >= 0 && e->type == SDL_FINGERUP) {
        if (!T.f[k].seen) { T.f[k].up = 1; return; }                        /* a tap within one frame: the game still gets that frame */
        drop(k);
    }
}

void touch_pad(PadState *st, int real_pad_used)
{
    check_init();
    if (!T.enabled) return;
    if (real_pad_used) { T.shown = 0; T.nf = 0; T.stick = -1; }
    if (!T.shown) return;
    Btn b[NBTN]; layout(b); uint32_t m = 0;
    for (int i = 0; i < T.nf; i++) if (T.f[i].ctl > 0) m |= 1u << b[T.f[i].ctl - 1].pad;
    float lx = 0, ly = 0;
    if (T.stick >= 0) {
        float R = 0.14f * unit(); lx = (T.kx - T.sx) / R; ly = (T.ky - T.sy) / R;
        float d = sqrtf(lx * lx + ly * ly); if (d > 1) { lx /= d; ly /= d; }
    }
    for (int i = T.nf; i-- > 0; ) { if (T.f[i].up) drop(i); else T.f[i].seen = 1; }
    if (!m && !lx && !ly) return;
    st->buttons |= m;
    if (lx * lx + ly * ly > st->lx * st->lx + st->ly * st->ly) { st->lx = lx; st->ly = ly; }
    if (st->kind == PADK_NONE) st->kind = PADK_XBOX;
}

/* ---- drawing ------------------------------------------------------------------------------------------------------- */
static void col(const float *rgb, float a) { glColor4f(rgb[0], rgb[1], rgb[2], a); }
static void disc(float x, float y, float r)
{
    glBegin(GL_TRIANGLE_FAN); glVertex2f(x, y);
    for (int i = 0; i <= 32; i++) { float a = (float)i * 6.2831853f / 32; glVertex2f(x + cosf(a) * r, y + sinf(a) * r); }
    glEnd();
}
static void ring(float x, float y, float r, float w)
{
    glBegin(GL_TRIANGLE_STRIP);
    for (int i = 0; i <= 32; i++) { float a = (float)i * 6.2831853f / 32, c = cosf(a), s = sinf(a); glVertex2f(x + c * r, y + s * r); glVertex2f(x + c * (r - w), y + s * (r - w)); }
    glEnd();
}
static void quad(float x0, float y0, float x1, float y1)
{
    glBegin(GL_TRIANGLE_STRIP); glVertex2f(x0, y0); glVertex2f(x1, y0); glVertex2f(x0, y1); glVertex2f(x1, y1); glEnd();
}
static void glyph(const Btn *b)
{
    float x = b->x, y = b->y, s = b->r * 0.45f; const float white[3] = { 1, 1, 1 }; col(white, 0.85f);
    switch (b->glyph) {
    case G_TRI_UP:   glBegin(GL_TRIANGLES); glVertex2f(x, y - s); glVertex2f(x + s, y + s * 0.7f); glVertex2f(x - s, y + s * 0.7f); glEnd(); break;
    case G_TRI_DOWN: glBegin(GL_TRIANGLES); glVertex2f(x, y + s); glVertex2f(x - s, y - s * 0.7f); glVertex2f(x + s, y - s * 0.7f); glEnd(); break;
    case G_DIAMOND:  glBegin(GL_TRIANGLE_FAN); glVertex2f(x, y - s); glVertex2f(x + s, y); glVertex2f(x, y + s); glVertex2f(x - s, y); glEnd(); break;
    case G_RING:     ring(x, y, s, s * 0.35f); break;
    case G_CAMERA:   quad(x - s, y - s * 0.6f, x + s * 0.5f, y + s * 0.6f);
                     glBegin(GL_TRIANGLES); glVertex2f(x + s * 0.5f, y); glVertex2f(x + s * 1.1f, y - s * 0.6f); glVertex2f(x + s * 1.1f, y + s * 0.6f); glEnd(); break;
    case G_EYE:      ring(x, y, s, s * 0.25f); disc(x, y, s * 0.35f); break;
    case G_PAUSE:    quad(x - s * 0.7f, y - s, x - s * 0.2f, y + s); quad(x + s * 0.2f, y - s, x + s * 0.7f, y + s); break;
    }
}
void touch_draw(int width, int height)
{
    check_init();
    if (!T.enabled || !T.shown || width <= 0 || height <= 0) return;
    T.W = (float)width; T.H = (float)height;
    static const GLenum caps[] = { GL_DEPTH_TEST, GL_TEXTURE_2D, GL_CULL_FACE, GL_ALPHA_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST, GL_BLEND };
    GLboolean was[sizeof caps / sizeof caps[0]]; GLint vp[4], bs = GL_ONE, bd = GL_ZERO;
    for (unsigned i = 0; i < sizeof caps / sizeof caps[0]; i++) { was[i] = glIsEnabled(caps[i]); glDisable(caps[i]); }
    glGetIntegerv(GL_VIEWPORT, vp); glGetIntegerv(GL_BLEND_SRC, &bs); glGetIntegerv(GL_BLEND_DST, &bd);
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0, width, height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Btn b[NBTN]; layout(b); uint32_t held = 0;
    for (int i = 0; i < T.nf; i++) if (T.f[i].ctl > 0) held |= 1u << (T.f[i].ctl - 1);
    for (int i = 0; i < NBTN; i++) { col(b[i].rgb, held >> i & 1 ? 0.75f : 0.35f); disc(b[i].x, b[i].y, b[i].r); glyph(&b[i]); }
    const float grey[3] = { 0.8f, 0.8f, 0.8f }; float u = unit(), R = 0.14f * u;
    float sx = T.stick >= 0 ? T.sx : 0.27f * u, sy = T.stick >= 0 ? T.sy : T.H - 0.27f * u, kx = T.stick >= 0 ? T.kx : sx, ky = T.stick >= 0 ? T.ky : sy;
    { float dx = kx - sx, dy = ky - sy, d = sqrtf(dx * dx + dy * dy); if (d > R) { kx = sx + dx / d * R; ky = sy + dy / d * R; } }
    col(grey, 0.18f); disc(sx, sy, R); col(grey, 0.45f); ring(sx, sy, R, 0.012f * u);
    col(grey, T.stick >= 0 ? 0.7f : 0.4f); disc(kx, ky, 0.06f * u);

    glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glBlendFunc((GLenum)bs, (GLenum)bd); glColor4f(1, 1, 1, 1);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    for (unsigned i = 0; i < sizeof caps / sizeof caps[0]; i++) if (was[i]) glEnable(caps[i]); else glDisable(caps[i]);
}
#endif
