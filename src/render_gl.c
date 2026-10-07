/* render_gl.c - Win32 window (the SDL2 one is plat_sdl.c) + OpenGL 1.1 fixed-function renderer.
 * The game data is left-handed (D3D). We keep world coordinates untouched and mirror z in the
 * projection, so a camera with yaw 0 looks along +z like the original. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#endif
#include "plat.h"
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "render_gl.h"
#include "player.h"                                   /* volume_contains (0x4300c0) */
#include "texpack.h"
#include "gtao.h"

#ifdef _WIN32
static Window *g_win;

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Window *w = g_win;
    switch (msg) {
    case WM_CLOSE: case WM_DESTROY: if (w) w->quit = 1; return 0;
    case WM_SIZE: if (w) { w->width = LOWORD(lp); w->height = HIWORD(lp); } return 0;
    case WM_DEVICECHANGE: if (w && wp == 0x0007 /* DBT_DEVNODES_CHANGED, sent to every top-level window */) w->dev_changes++; break;
    case WM_KILLFOCUS:                                            /* Alt+Tab, a click elsewhere: the key-up of a held key goes to the other window, */
        if (w) { memset(w->keys, 0, sizeof w->keys); if (w->mouse_right) { w->mouse_right = 0; ReleaseCapture(); } }   /* so let go of all of them (plat_sdl.c does the same) */
        break;
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {   /* Esc is a menu key now (docs/MENU_NEWGAME.md 1.3), not quit */
        int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN, sc = (int)(lp >> 16) & 0xff, ext = (int)(lp >> 24) & 1;
        if (w && wp < 256) w->keys[wp] = down;
        /* the sides of Ctrl / Shift / Alt and the numpad whatever Num Lock says: Woody.cfg binds DirectInput keys (docs/INPUT.md) */
        if (w && wp == VK_CONTROL) w->keys[ext ? VK_RCONTROL : VK_LCONTROL] = down;   /* extended-key bit: right Ctrl (the special attack) */
        if (w && wp == VK_SHIFT) w->keys[sc == 0x36 ? VK_RSHIFT : VK_LSHIFT] = down;
        if (w && wp == VK_MENU) w->keys[ext ? VK_RMENU : VK_LMENU] = down;
        if (w && !ext && sc >= 0x47 && sc <= 0x53) { static const unsigned char np[13] = { VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9, 0, VK_NUMPAD4, VK_NUMPAD5, VK_NUMPAD6, 0, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD0, VK_DECIMAL }; if (np[sc - 0x47]) w->keys[np[sc - 0x47]] = down; }
        if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) break;        /* Alt+F4 and friends */
        return 0;
    }
    case WM_RBUTTONDOWN: if (w) { w->mouse_right = 1; SetCapture(h); } return 0;
    case WM_RBUTTONUP: if (w) { w->mouse_right = 0; ReleaseCapture(); } return 0;
    case WM_MOUSEMOVE: {
        static int lx = -1, ly = -1; int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (w && w->mouse_right && lx >= 0) { w->mouse_dx += x - lx; w->mouse_dy += y - ly; }
        lx = x; ly = y; return 0; }
    case WM_INPUT: {                                              /* relative mouse counts (RegisterRawInputDevices in win_open) */
        RAWINPUT ri; UINT sz = sizeof ri;
        if (w && GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 && ri.header.dwType == RIM_TYPEMOUSE
            && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) { w->raw_dx += ri.data.mouse.lLastX; w->raw_dy += ri.data.mouse.lLastY; }
        break; }                                                  /* DefWindowProc cleans up after WM_INPUT */
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int win_open(Window *w, const char *title, int width, int height)
{
    memset(w, 0, sizeof *w); g_win = w;
    WNDCLASSA wc = {0}; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "WoodyRE"; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1)); /* out/woody.rc, if linked in */ wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    RECT rc = {0, 0, width, height}; AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("WoodyRE", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return -1;
    HDC hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {0}; pfd.nSize = sizeof pfd; pfd.nVersion = 1; pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER; pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cDepthBits = 24; pfd.cStencilBits = 8;
    int pf = ChoosePixelFormat(hdc, &pfd); SetPixelFormat(hdc, pf, &pfd);
    HGLRC rc2 = wglCreateContext(hdc); wglMakeCurrent(hdc, rc2);
    w->hwnd = hwnd; w->hdc = hdc; w->hglrc = rc2; w->width = width; w->height = height;
    /* the mouse as raw input (usage page 1, usage 2): relative counts, delivered only while the window is in the foreground, the
     * cursor left alone - like the original's DirectInput mouse, cooperative level 6 = non-exclusive + foreground (0x467be5) */
    { RAWINPUTDEVICE rid = { 0x01, 0x02, 0, hwnd }; RegisterRawInputDevices(&rid, 1, sizeof rid); }
    printf("OpenGL: %s / %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    return 0;
}
void win_poll(Window *w) { MSG m; w->mouse_dx = w->mouse_dy = 0; w->raw_dx = w->raw_dy = 0; while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); } w->focused = GetForegroundWindow() == (HWND)w->hwnd; }
void win_title(Window *w, const char *title) { SetWindowTextA((HWND)w->hwnd, title); }
void win_swap(Window *w) { SwapBuffers((HDC)w->hdc); }
void win_mode(Window *w, int width, int height, int full)
{
    HWND h = (HWND)w->hwnd; MONITORINFO mi = { sizeof mi };
    GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    if (full) {
        SetWindowLongA(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        return;
    }
    RECT rc = { 0, 0, width, height }; AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    int fw = rc.right - rc.left - width, fh = rc.bottom - rc.top - height, aw = mi.rcWork.right - mi.rcWork.left - fw, ah = mi.rcWork.bottom - mi.rcWork.top - fh;
    if (width > aw || height > ah) { float s = (float)aw / width < (float)ah / height ? (float)aw / width : (float)ah / height; printf("window %dx%d does not fit the screen: %dx%d\n", width, height, (int)(width * s), (int)(height * s)); width = (int)(width * s); height = (int)(height * s); }
    SetWindowLongA(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    SetWindowPos(h, HWND_NOTOPMOST, mi.rcWork.left + (aw - width) / 2, mi.rcWork.top + (ah - height) / 2, width + fw, height + fh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}
int win_vsync(int interval)
{
    typedef BOOL (WINAPI *SwapFn)(int);
    SwapFn f = (SwapFn)(void (*)(void))wglGetProcAddress("wglSwapIntervalEXT");
    return f && f(interval) ? 0 : -1;
}
void win_close(Window *w) { wglMakeCurrent(NULL, NULL); wglDeleteContext((HGLRC)w->hglrc); ReleaseDC((HWND)w->hwnd, (HDC)w->hdc); DestroyWindow((HWND)w->hwnd); }
double win_time(void) { static LARGE_INTEGER f; LARGE_INTEGER c; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart; }
#endif

/* screen brightness like the original's fader 0x4776d0: 1 = normal, 0 = black */
void rnd_fade(float brightness)
{
    if (brightness >= 1.0f) return;
    if (brightness < 0) brightness = 0;
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_TEXTURE_2D); glDisable(GL_CULL_FACE); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_FALSE);
    glColor4f(0, 0, 0, 1.0f - brightness);
    glBegin(GL_QUADS); glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1); glEnd();
    glColor4f(1, 1, 1, 1); glDepthMask(GL_TRUE); glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW); glPopMatrix();
}

static uint8_t col2x(uint32_t v) { v = (v & 0xff) * 2; return v > 255 ? 255 : (uint8_t)v; }
Vec3 cam_forward(const FreeCamera *c) { Vec3 v = { sinf(c->yaw) * cosf(c->pitch), sinf(c->pitch), cosf(c->yaw) * cosf(c->pitch) }; return v; }
Vec3 cam_right(const FreeCamera *c) { Vec3 v = { -cosf(c->yaw), 0, sinf(c->yaw) }; return v; }   /* right-handed world: looking along +z, +x is on the left */

/* ---------------------------------------------------------------- textures */
/* 0x47fa60 (called by the .tex loader 0x426eaa with 3 extra levels): every level texture is a DirectDraw surface with
 * DDSD_MIPMAPCOUNT = 4 (0x47fac2) and caps TEXTURE|MIPMAP|COMPLEX (0x47fb5c); the loop 0x47fc80..0x47fed1 fills the
 * three smaller levels itself with a 2x2 box filter (0x47fd83..0x47fe17: (a+b+c+d)/4 per channel, alpha included).
 * The device samples them with MIN/MAG LINEAR and MIPFILTER POINT (0x47ed3a..0x47ed62, D3D7 D3DTFP_POINT = 2), i.e.
 * GL_LINEAR_MIPMAP_NEAREST. Without the smaller levels a far wall of a dense texture aliases into noise: W1B's star
 * box (group 52, one-texel stars repeated every 100 units) turned into flickering speckles (issue #38).
 *
 * Every level is a 16-bit surface, and the port keeps the texels in exactly that form before it widens them for GL:
 * - 0x47f090(v, 0) widens the file's RGB565 to ARGB8888 with the low bits 0 (r5 << 3, g6 << 2, b5 << 3, alpha 0), the
 *   colour key test 0x47fc0e..0x47fc1e runs on that (magenta -> 0x00000000, anything else | 0xff000000), and 0x47f170
 *   packs it back into the surface format: [0x5e8690] (0x40283b: 0 = RGB565 when the device offers it, 1 = 555,
 *   2 = 4444) or, for a colour key texture, format 3 = ARGB1555 (0x47fae9, 0x47f1fd). The low-bits-0 value is only
 *   ever an intermediate: an RGB565 texture reaches the device bit for bit as the file has it, a colour key texture
 *   loses the lowest bit of green and keeps one alpha bit. What the sampler makes of the 16-bit texel is up to the
 *   hardware; the port widens it by bit replication (= x * 255 / 31 up to rounding), as D3D7-era cards and every
 *   current one do. Widening with zeros instead would darken every texture by up to 7/255.
 * - the smaller levels (0x47fd83..0x47fe17) decode the four source texels of the level above the same way (format 3
 *   decodes its alpha bit as 0x80, 0x47f127), average them per channel with truncation - (sum of (p & 0xfcfcfc)) >> 2,
 *   alpha (sum of (p >> 2) & 0x3fc00000) & 0xff000000 - and pack the result with truncation again. So each level
 *   loses a fraction of a 5/6-bit step (they grow a little darker), and in a colour key texture a smaller texel is
 *   opaque only when all four sources are: 4 x 0x80 / 4 = 0x80 keeps the 1555 alpha bit, 3 x 0x80 / 4 = 0x60 does not. */
static uint16_t tex16_texel(uint16_t v, int ck)                          /* level 0: 0x47f090(v, 0), colour key, 0x47f170(argb, fmt) */
{
    if (!ck) return v;                                                   /* 565 -> 8888 (low bits 0) -> 565 is the identity */
    uint32_t r5 = v >> 11 & 31, g6 = v >> 5 & 63, b5 = v & 31;
    if ((r5 << 3 & 0xf0) == 0xf0 && (g6 << 2 & 0xf0) == 0 && (b5 << 3 & 0xf0) == 0xf0) return 0;   /* 0x47fc1e: (argb & 0xf0f0f0) == 0xf000f0 */
    return (uint16_t)(0x8000 | r5 << 10 | (g6 >> 1) << 5 | b5);         /* 0x47f1fd: ARGB1555, green truncated to 5 bits */
}
static void tex16_halve(const uint16_t *src, int w, int h, uint16_t *dst, int ck)   /* 0x47fd83..0x47fe17 */
{
    int dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
    for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) {
        int x0 = 2 * x < w ? 2 * x : w - 1, x1 = 2 * x + 1 < w ? 2 * x + 1 : x0, y0 = 2 * y < h ? 2 * y : h - 1, y1 = 2 * y + 1 < h ? 2 * y + 1 : y0;
        uint16_t p[4] = { src[y0 * w + x0], src[y0 * w + x1], src[y1 * w + x0], src[y1 * w + x1] };
        uint32_t r = 0, g = 0, b = 0, a = 0;
        for (int k = 0; k < 4; k++) {                                    /* 0x47f090: fmt 0 = 0x47f0a4, fmt 3 = 0x47f127; every channel has its low bits 0, so & 0xfc drops nothing */
            if (ck) { r += (p[k] >> 10 & 31u) << 3; g += (p[k] >> 5 & 31u) << 3; b += (p[k] & 31u) << 3; a += p[k] & 0x8000 ? 0x80u : 0; }
            else { r += (p[k] >> 11 & 31u) << 3; g += (p[k] >> 5 & 63u) << 2; b += (p[k] & 31u) << 3; }
        }
        r >>= 2; g >>= 2; b >>= 2; a >>= 2;
        dst[y * dw + x] = ck ? (uint16_t)((a & 0x80 ? 0x8000 : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3)   /* 0x47f1fd */
                             : (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);                            /* 0x47f184 */
    }
}
static void tex16_widen(const uint16_t *s, uint8_t *d, uint32_t n, int ck)  /* the sampler's side: 5/6 bits -> 8 by replication */
{
    for (uint32_t i = 0; i < n; i++, d += 4) {
        uint32_t v = s[i], r5, g, b5;
        if (ck) { r5 = v >> 10 & 31; b5 = v & 31; g = v >> 5 & 31; g = g << 3 | g >> 2; d[3] = v & 0x8000 ? 255 : 0; }
        else { r5 = v >> 11 & 31; b5 = v & 31; g = v >> 5 & 63; g = g << 2 | g >> 4; d[3] = 255; }
        d[0] = (uint8_t)(r5 << 3 | r5 >> 2); d[1] = (uint8_t)g; d[2] = (uint8_t)(b5 << 3 | b5 >> 2);
    }
}
/* texture sharpness (PORT EXTRA, docs/DISPLAY.md 5): anisotropic filtering (GL_EXT_texture_filter_anisotropic) on the level
 * textures, over the original's filters; 1 = off, the original. rnd_frame applies a change to the loaded textures. */
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
static int g_aniso = 1, g_aniso_max = -1, g_aniso_dirty;
int rnd_aniso_max(void)
{
    if (g_aniso_max < 0) {
        const char *e = (const char *)glGetString(GL_EXTENSIONS); GLfloat m = 0;
        if (e && (strstr(e, "GL_EXT_texture_filter_anisotropic") || strstr(e, "GL_ARB_texture_filter_anisotropic"))) glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &m);
        g_aniso_max = m >= 16 ? 16 : m >= 8 ? 8 : m >= 4 ? 4 : m >= 2 ? 2 : 0;
    }
    return g_aniso_max;
}
void rnd_set_aniso(int n) { if (n < 1) n = 1; if (n != g_aniso) { g_aniso = n; g_aniso_dirty = 1; } }
static void tex_aniso(void) { int m = rnd_aniso_max(); if (m >= 2) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, (GLfloat)(g_aniso < m ? g_aniso : m)); }
static GLuint upload_texture(const TexGroup *g, int frame)
{
    GLuint id; glGenTextures(1, &id); glBindTexture(GL_TEXTURE_2D, id);
    int w = (int)g->width, h = (int)g->height, ck = g->flags & 1; uint32_t n = g->width * g->height;
    uint64_t hash = tp_hash('T', g->frames[frame], n * 2, w, h);
    if (tp_replace(hash, ck ? TP_KEY : TP_OPAQUE)) {                         /* port extra: a texture pack's PNG (texpack.c) */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        tex_aniso(); return id;
    }
    uint16_t *s = (uint16_t *)malloc((size_t)n * 2 + 2), *half = (uint16_t *)malloc((size_t)n * 2 + 2); uint8_t *rgba = (uint8_t *)malloc((size_t)n * 4 + 4);
    for (uint32_t i = 0; i < n; i++) s[i] = tex16_texel(g->frames[frame][i], ck);
    tex16_widen(s, rgba, n, ck);
    tp_dump(hash, rgba, w, h);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    int level = 0;
    while (w > 1 || h > 1) {                  /* the original stops after 3 levels (GL_TEXTURE_MAX_LEVEL below); the rest only makes the chain complete for GL 1.1 */
        tex16_halve(s, w, h, half, ck);
        w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; level++;
        tex16_widen(half, rgba, (uint32_t)(w * h), ck);
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        uint16_t *t = s; s = half; half = t;
    }
    glTexParameteri(GL_TEXTURE_2D, 0x813D /* GL_TEXTURE_MAX_LEVEL (1.2) */, level < 3 ? level : 3);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    tex_aniso();
    free(s); free(half); free(rgba); return id;
}

/* ---------------------------------------------------------------- world batches */
static void material_uv(const Material *m, float x, float y, float z, float *u, float *v)
{
    *u = m->m[0] * x + m->m[3] * y + m->m[6] * z + m->m[9];
    *v = m->m[1] * x + m->m[4] * y + m->m[7] * z + m->m[10];
}

/* ---------------------------------------------------------------- static lighting (docs/LIGHTING.md)
 * pixel = 2 * tex * vcol * min(1, AMB + sum C/255 * max(0, 1 - |P-L|/R)) over the lights that see the point. Which
 * light sees what is precomputed in the .lit: list A = faces seen completely, list C = the lit fragments of the
 * partially lit faces B. A cast shadow is simply where no light polygon is. */
#define LIT_AMB (76.0f / 255.0f)                     /* 0x4C4C4C */
static const float *lit_vpos(const Renderer *r, int32_t i, float tmp[3])
{
    if (i >= 0) { const GelVert *v = &r->gel->verts[(uint32_t)i < r->gel->nverts ? (uint32_t)i : 0]; tmp[0] = v->x; tmp[1] = v->y; tmp[2] = v->z; }
    else { uint32_t k = (uint32_t)(-i - 1); Vec3 e = k < r->lit->nextra ? r->lit->extra[k] : (Vec3){ 0, 0, 0 }; tmp[0] = e.x; tmp[1] = e.y; tmp[2] = e.z; }
    return tmp;
}
static void batch_reserve(struct WorldBatch *b, uint32_t extra_tris, uint32_t *cap)
{
    if (b->ntris + extra_tris <= *cap) return;
    *cap = (b->ntris + extra_tris) * 2 + 64;
    b->pos = (float *)realloc(b->pos, (size_t)*cap * 9 * sizeof(float)); b->uv = (float *)realloc(b->uv, (size_t)*cap * 6 * sizeof(float)); b->col = (uint8_t *)realloc(b->col, (size_t)*cap * 12);   /* RGBA: OpenGL ES takes no 3-byte colours */
    if (b->face) b->face = (uint32_t *)realloc(b->face, (size_t)*cap * 4);
}
/* one light polygon (flush 0x4293f0 / 0x42c320): constant colour C*k, radial texture 15 - round(k*15.49) */
/* The world face a C polygon lies on. Its own field +0x08 is NOT that face: it holds the material word of the parent
 * (docs/LIGHTING.md 2; 0x42c320 never reads it), so culling the fragment by it tied every partial light patch to an
 * unrelated face and the lit part of a floor came and went with the sectors in view. The parent is the B face of the
 * same light that is coplanar with the fragment (litparse check 3: every C polygon has one) and holds its centroid;
 * several coplanar tiles are told apart by that containment test. -1 = none found (the fragment is then always kept). */
static uint32_t lit_c_parent(const Renderer *r, const LitLight *L, const LitPoly *P)
{
    const GelFile *g = r->gel; float c[3] = { 0, 0, 0 }, t[3]; uint32_t best = UINT32_MAX; float best_d = 1e30f;
    if (P->n < 3) return UINT32_MAX;
    for (uint32_t k = 0; k < P->n; k++) { const float *v = lit_vpos(r, P->indices[k], t); c[0] += v[0]; c[1] += v[1]; c[2] += v[2]; }
    for (int q = 0; q < 3; q++) c[q] /= (float)P->n;
    for (uint32_t k = 0; k < L->nb; k++) {
        uint32_t f = L->b[k]; if (f >= g->npolys) continue;
        const GelPoly *gp = &g->polys[f]; const float *pl = gp->plane; if (gp->nverts < 3) continue;
        if (pl[0] * P->plane[0] + pl[1] * P->plane[1] + pl[2] * P->plane[2] < 0.999f || fabsf(pl[3] - P->plane[3]) > 1.0f) continue;
        float dmax = -1e30f, dmin = 1e30f;                     /* signed edge distances; the winding decides which sign is outside */
        for (uint32_t e = 0; e < gp->nverts; e++) {
            const GelVert *a = &g->verts[gp->indices[e]], *b = &g->verts[gp->indices[(e + 1) % gp->nverts]];
            float ex = b->x - a->x, ey = b->y - a->y, ez = b->z - a->z;
            float nx = ey * pl[2] - ez * pl[1], ny = ez * pl[0] - ex * pl[2], nz = ex * pl[1] - ey * pl[0], nl = sqrtf(nx * nx + ny * ny + nz * nz);
            if (nl < 1e-6f) continue;
            float d = ((c[0] - a->x) * nx + (c[1] - a->y) * ny + (c[2] - a->z) * nz) / nl; if (d > dmax) dmax = d; if (d < dmin) dmin = d;
        }
        float out = dmax < -dmin ? dmax : -dmin; if (out < 0) out = 0;   /* 0 = the centroid is inside the face */
        if (out < best_d) { best_d = out; best = f; }
    }
    return best;
}
static void light_poly(Renderer *r, const LitLight *L, const float *plane, const int32_t *idx, uint32_t n, uint32_t cap[16], uint32_t face)
{
    if (n < 3) return;
    float dist = plane[0] * L->pos.x + plane[1] * L->pos.y + plane[2] * L->pos.z + plane[3], R = L->range;
    if (fabsf(dist) >= R || R <= 0) return;
    float k = 1.0f - fabsf(dist) / R; int ti = 15 - (int)(k * 15.49f + 0.5f); if (ti < 0) ti = 0; if (ti > 15) ti = 15;
    float s = 0.5f / sqrtf(R * R - dist * dist);
    float F[3] = { L->pos.x - plane[0] * dist, L->pos.y - plane[1] * dist, L->pos.z - plane[2] * dist }, t0[3], t1[3], t2[3];
    const float *v2 = lit_vpos(r, idx[2], t2);
    float U[3] = { v2[0] - F[0], v2[1] - F[1], v2[2] - F[2] }, ul = sqrtf(U[0] * U[0] + U[1] * U[1] + U[2] * U[2]);
    if (ul < 1e-4f) { U[0] = plane[1]; U[1] = plane[2]; U[2] = plane[0]; float d = U[0] * plane[0] + U[1] * plane[1] + U[2] * plane[2]; for (int q = 0; q < 3; q++) U[q] -= plane[q] * d; ul = sqrtf(U[0] * U[0] + U[1] * U[1] + U[2] * U[2]); if (ul < 1e-4f) return; }
    for (int q = 0; q < 3; q++) U[q] /= ul;
    float W[3] = { plane[1] * U[2] - plane[2] * U[1], plane[2] * U[0] - plane[0] * U[2], plane[0] * U[1] - plane[1] * U[0] };
    struct WorldBatch *b = &r->lightb[ti]; if (!b->face) b->face = (uint32_t *)malloc(4); batch_reserve(b, n - 2, &cap[ti]);
    uint8_t col[3]; for (int q = 0; q < 3; q++) { float c = L->colour[q] * k; col[q] = (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c); }
    for (uint32_t t = 1; t + 1 < n; t++) {
        const float *pv[3] = { lit_vpos(r, idx[0], t0), lit_vpos(r, idx[t], t1), lit_vpos(r, idx[t + 1], t2) };
        for (int c = 0; c < 3; c++) {
            size_t o = (size_t)b->ntris * 3 + c; float d[3] = { pv[c][0] - F[0], pv[c][1] - F[1], pv[c][2] - F[2] };
            b->pos[o * 3] = pv[c][0]; b->pos[o * 3 + 1] = pv[c][1]; b->pos[o * 3 + 2] = pv[c][2];
            b->uv[o * 2] = 0.5f + s * (d[0] * W[0] + d[1] * W[1] + d[2] * W[2]); b->uv[o * 2 + 1] = 0.5f + s * (d[0] * U[0] + d[1] * U[1] + d[2] * U[2]);
            b->col[o * 4] = col[0]; b->col[o * 4 + 1] = col[1]; b->col[o * 4 + 2] = col[2]; b->col[o * 4 + 3] = 255;
        }
        b->face[b->ntris] = face;
        b->ntris++;
    }
}
static void light_textures(Renderer *r)                 /* generator 0x480090: 16 radial 32x32 textures */
{
    enum { N = 32 }; uint8_t px[N * N * 3];
    for (int i = 0; i < 16; i++) {
        float a = i / 16.0f;
        for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
            float fx = (x + 0.5f) / N * 2 - 1, fy = (y + 0.5f) / N * 2 - 1, d = sqrtf(fx * fx + fy * fy + a * a); if (d > 1) d = 1;
            float g = (1.0f - d) / (1.0f - a) * 255.0f; if (x == 0 || y == 0 || x == N - 1 || y == N - 1) g = 0;
            uint8_t v = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g); px[(y * N + x) * 3] = px[(y * N + x) * 3 + 1] = px[(y * N + x) * 3 + 2] = v;
        }
        GLuint id; glGenTextures(1, &id); glBindTexture(GL_TEXTURE_2D, id); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, N, N, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        r->light_tex[i] = id;
    }
}

/* 0x42acd3: a world face whose material has bit 15 set has no material and is never handed to the face drawer
 * 0x42b6c0 - it is collision only (the ground type lookup 0x462948 tests the same bit), which is how an invisible
 * barrier like the glass plate in W1A is built. Drawing such a face anyway put flat grey slabs in the middle of the
 * room (issue #2), because without a material it can only be vertex colour. A material or group index past the end of
 * the table cannot be drawn either and is treated the same way. */
static int gel_face_invisible(const GelFile *g, const TexFile *t, uint32_t i)
{
    uint32_t mat = g->polys[i].material;
    return (mat & 0x8000) || (mat & 0x7fff) >= t->nmaterials || t->materials[mat & 0x7fff].group >= t->ngroups;
}

static void sh_reset(void);
int rnd_init(Renderer *r, TexFile *tex, GelFile *gel, InsFile *ins, const LitFile *lit, const VisFile *vis)
{
    sh_reset();
    memset(r, 0, sizeof *r); r->tex = tex; r->gel = gel; r->ins = ins; r->show_world = r->show_instances = r->show_light = 1;
    r->lit = (lit && lit->nlights) ? lit : NULL;
    r->vis = (vis && vis->nsectors == gel->nsectors) ? vis : NULL;
    r->cull = gel->nsectors ? (r->vis ? 2 : 1) : 0;
    r->face_batch = (struct FaceBatch *)calloc(gel->npolys ? gel->npolys : 1, sizeof(struct FaceBatch));
    r->face_stamp = (uint32_t *)calloc(gel->npolys ? gel->npolys : 1, 4);
    r->sec_vis = (uint8_t *)calloc(gel->nsectors ? gel->nsectors : 1, 1);
    r->sec_prev = (uint8_t *)calloc(gel->nsectors ? gel->nsectors : 1, 1);
    r->sec_dirty = 1; r->sky_on = 1;
    for (int k = 0; k < 6; k++) r->race[k] = -1;                                  /* no race region list: the plain .vis path */
    for (uint32_t z = 0, i = 0; z < gel->ngroups; z++) for (; i < gel->groups[z].end && i < gel->npolys; i++) r->face_batch[i].zone = z;
    for (uint32_t g = 0; g < tex->ngroups; g++) {
        TexGroup *tg = &tex->groups[g]; tg->gl_frames = (uint32_t *)calloc(tg->frame_count ? tg->frame_count : 1, 4);
        for (uint32_t f = 0; f < tg->frame_count; f++) tg->gl_frames[f] = upload_texture(tg, (int)f);
        tg->gl_tex = tg->gl_frames[0];
    }
    /* face classes: 0 = not lit by any light, 1 = lit (in a list A or B) */
    uint8_t *lit_face = (uint8_t *)calloc(gel->npolys + 1, 1);
    if (r->lit) for (uint32_t l = 0; l < r->lit->nlights; l++) {
        const LitLight *L = &r->lit->lights[l];
        for (uint32_t k = 0; k < L->na; k++) if (L->a[k] < gel->npolys) lit_face[L->a[k]] = 1;
        for (uint32_t k = 0; k < L->nb; k++) if (L->b[k] < gel->npolys) lit_face[L->b[k]] = 1;
    }
    r->nbatches = tex->ngroups;
    r->batches = (struct WorldBatch *)calloc(r->nbatches, sizeof *r->batches); r->litb = (struct WorldBatch *)calloc(r->nbatches, sizeof *r->litb);
    uint32_t *cap = (uint32_t *)calloc(r->nbatches * 2, 4);
    for (uint32_t g = 0; g < r->nbatches; g++) r->batches[g].group = r->litb[g].group = g;
    for (uint32_t i = 0; i < gel->npolys; i++) {
        GelPoly *p = &gel->polys[i]; if (p->nverts < 3) continue;
        if (gel_face_invisible(gel, tex, i)) continue;                             /* collision only, never drawn (0x42acd3) */
        const Material *m = &tex->materials[p->material & 0x7fff];
        uint32_t grp = m->group; uint32_t gflags = tex->groups[grp].flags;
        if (((gflags >> 8) & 0xff) == 2) {                                         /* sky group (0x42acea): the face is never drawn, it only switches the sky cube on */
            TexGroup *sg = &tex->groups[grp];
            r->face_batch[i].sky = 1;
            if (!r->have_sky) { r->have_sky = 1; r->sky_hu = 0.5f / (float)sg->width; r->sky_hv = 0.5f / (float)sg->height; for (int f = 0; f < 5; f++) r->sky_tex[f] = sg->gl_frames[(uint32_t)f < sg->frame_count ? f : 0]; }
            continue;
        }
        /* multi-pass only for plain opaque textures: colour-keyed faces would get their holes filled by the ambient pass,
         * they take the same light per vertex instead */
        int multipass = r->lit && lit_face[i] && !(gflags & 3);
        struct WorldBatch *b = multipass ? &r->litb[grp] : &r->batches[grp];
        batch_reserve(b, p->nverts - 2, &cap[grp * 2 + multipass]);
        r->face_batch[i].tri0 = b->ntris; r->face_batch[i].group = grp;
        r->face_batch[i].ntris = p->nverts - 2; r->face_batch[i].lit = (uint8_t)multipass;
        for (uint32_t k = 1; k + 1 < p->nverts; k++) {
            uint32_t idx[3] = { p->indices[0], p->indices[k], p->indices[k + 1] };
            for (int c = 0; c < 3; c++) {
                GelVert *v = &gel->verts[idx[c]]; size_t o = (size_t)b->ntris * 3 + c;
                b->pos[o * 3] = v->x; b->pos[o * 3 + 1] = v->y; b->pos[o * 3 + 2] = v->z;
                float u, vv; material_uv(m, v->x, v->y, v->z, &u, &vv);
                b->uv[o * 2] = u; b->uv[o * 2 + 1] = vv;
                float scale[3] = { 2, 2, 2 };                                  /* bytes R,G,B; 128 = neutral (modulate 2x) */
                if (multipass) scale[0] = scale[1] = scale[2] = 1.0f;           /* texture pass: 2 * src * dst */
                else if (r->lit && !(gflags & 2)) {
                    if (!lit_face[i]) scale[0] = scale[1] = scale[2] = 0.6f;    /* unlit faces: tex x vcol x 0.6, modulate 1x (0x42bef9) */
                    else for (uint32_t l = 0; l < r->lit->nlights; l++) {       /* colour-keyed lit face: light per vertex */
                        const LitLight *L = &r->lit->lights[l]; int in = 0;
                        for (uint32_t q = 0; q < L->na && !in; q++) in = L->a[q] == i;
                        for (uint32_t q = 0; q < L->nb && !in; q++) in = L->b[q] == i;
                        float dx = v->x - L->pos.x, dy = v->y - L->pos.y, dz = v->z - L->pos.z, f = 1.0f - sqrtf(dx * dx + dy * dy + dz * dz) / L->range;
                        if (l == 0) scale[0] = scale[1] = scale[2] = LIT_AMB;
                        if (in && f > 0) for (int q = 0; q < 3; q++) scale[q] += L->colour[q] / 255.0f * f;
                        if (l + 1 == r->lit->nlights) for (int q = 0; q < 3; q++) scale[q] = (scale[q] > 1 ? 1 : scale[q]) * 2;
                    }
                }
                for (int q = 0; q < 3; q++) { float cv = ((v->colour >> (8 * q)) & 0xff) * scale[q]; b->col[o * 4 + q] = (uint8_t)(cv > 255 ? 255 : cv); }
                b->col[o * 4 + 3] = 255;
            }
            b->ntris++;
        }
    }
    free(cap); free(lit_face);
    for (uint32_t g = 0; g < tex->ngroups; g++) if (tex->groups[g].flags & 2) {          /* additive groups: intensity = flags byte 2 */
        uint32_t a = (tex->groups[g].flags >> 16) & 0xff; struct WorldBatch *b = &r->batches[g];
        for (size_t i = 0; i < (size_t)b->ntris * 12; i++) if (i % 4 != 3) b->col[i] = (uint8_t)(b->col[i] * a / 255);
    }
    if (r->lit) {
        r->face_bound = (float *)calloc(gel->npolys + 1, 4 * sizeof(float));
        for (uint32_t i = 0; i < gel->npolys; i++) {
            const GelPoly *p = &gel->polys[i]; float *fb = &r->face_bound[4 * i]; if (!p->nverts) continue;
            for (uint32_t k = 0; k < p->nverts; k++) { const GelVert *v = &gel->verts[p->indices[k]]; fb[0] += v->x; fb[1] += v->y; fb[2] += v->z; }
            fb[0] /= p->nverts; fb[1] /= p->nverts; fb[2] /= p->nverts;
            for (uint32_t k = 0; k < p->nverts; k++) { const GelVert *v = &gel->verts[p->indices[k]]; float dx = v->x - fb[0], dy = v->y - fb[1], dz = v->z - fb[2], d = sqrtf(dx * dx + dy * dy + dz * dz); if (d > fb[3]) fb[3] = d; }
        }
        {   /* shadow receivers (port, speed only): every face's fan as GL_TRIANGLES indices, for the stencil marks of cast_shadow */
            uint32_t np = gel->npolys, nfan = 0;
            r->face_fan = (uint32_t *)calloc(np + 1, sizeof *r->face_fan);
            for (uint32_t i = 0; i < np; i++) { r->face_fan[i] = nfan; if (gel->polys[i].nverts >= 3) nfan += (gel->polys[i].nverts - 2) * 3; }
            r->face_fan[np] = nfan; r->fan_idx = (uint32_t *)malloc((nfan + 1) * sizeof *r->fan_idx);
            for (uint32_t i = 0; i < np; i++) { const GelPoly *p = &gel->polys[i]; uint32_t *o = &r->fan_idx[r->face_fan[i]];
                for (uint32_t k = 2; k < p->nverts; k++) { *o++ = p->indices[0]; *o++ = p->indices[k - 1]; *o++ = p->indices[k]; } }
        }
        uint32_t lcap[16] = { 0 }; light_textures(r);
        for (uint32_t l = 0; l < r->lit->nlights; l++) {
            const LitLight *L = &r->lit->lights[l];
            /* no light spot on a face that is not drawn; a fragment whose face index is unusable is kept, the
             * visibility pass keeps those too */
            for (uint32_t k = 0; k < L->na; k++) if (L->a[k] < gel->npolys && !gel_face_invisible(gel, tex, L->a[k])) { const GelPoly *p = &gel->polys[L->a[k]]; light_poly(r, L, p->plane, (const int32_t *)p->indices, p->nverts, lcap, L->a[k]); }
            for (uint32_t k = 0; k < L->nc; k++) {
                uint32_t f = lit_c_parent(r, L, &L->c[k]);
                if (f >= gel->npolys || !gel_face_invisible(gel, tex, f)) light_poly(r, L, L->c[k].plane, L->c[k].indices, L->c[k].n, lcap, f);
            }
        }
        uint32_t nl = 0; for (int t = 0; t < 16; t++) nl += r->lightb[t].ntris;
        printf("lighting: %u lights, %u light triangles\n", r->lit->nlights, nl);
    }
    for (uint32_t i = 0; i < r->nbatches; i++) r->total_tris += r->batches[i].ntris + r->litb[i].ntris;
    printf("world: %u triangles, %u sectors, %u kd cells, %u loose faces; culling %s\n",
           r->total_tris, gel->nsectors, gel->ncells, gel->nloose,
           !r->cull ? "off (no sectors)" : r->vis ? "frustum + .vis" : "frustum (no .vis)");
    return 0;
}

/* The letterbox image strip inside the view box (x, y from its bottom left). The original only ever renders 640x480, and its
 * letterbox (0x41f8d0 / 0x41f910: sy = 0.5625, sx = 1.0) is a 640x360 = 16:9 picture with hfov 100.4 / vfov 68.0 - 1 centred
 * (y 60..420, cinematics), 2 shifted up (y 30..390, mode 4). In a wider box (the Hor+ window mode) widening that strip would
 * show more than the original's frame: Woody, whose cinematic animations run him out of the shot and then hold the last
 * key (W2D anim 73: the root stops at frame 2100 of 3500), stayed visible standing still at the side. So the strip keeps
 * 16:9 on every window: as wide as the box, pillarboxed when the box is wider than 16:9; the bar split stays 1:1 / 1:3. */
static void lb_strip(const Window *w, int lb, int *x, int *y, int *vw, int *vh)
{
    int W = w->width, H = w->height, sw = W, sh = W * 9 / 16;
    if (sh > H) { sh = H; sw = H * 16 / 9; }
    *x = (W - sw) / 2; *y = lb == 1 ? (H - sh) / 2 : (H - sh) * 3 / 4; *vw = sw; *vh = sh;
}
static float view_aspect(const Window *w, const FreeCamera *cam)
{
    if (!w->height) return 1.333f;
    if (!cam->letterbox) return (float)w->width / (float)w->height;
    int x, y, vw, vh; lb_strip(w, cam->letterbox, &x, &y, &vw, &vh); return vh ? (float)vw / (float)vh : 16.0f / 9.0f;
}

/* ---- visibility (0x42a980 / 0x42ac10) ---------------------------------------------------------
 * The original never hands the whole level to the device. It looks up the sector the camera stands in, takes the
 * list of sectors the .vis says are reachable from there, and stamps the faces of those - each face once, however
 * many cells list it. The port does the same and then drops every sector whose box misses the view frustum, which
 * the original gets for free from its portal walk. The result is one index list per batch; the vertex arrays
 * themselves are built once and never touched again.
 *
 * Everything degrades to "draw it all": a camera in no sector (the free camera outside the level), a level without
 * sectors, a .vis that does not match the .gel, and cull mode 0 from F4. */
static void frustum_planes(const FreeCamera *cam, Vec3 fw, Vec3 rt, Vec3 up, float aspect, float fy, float zn, float zf, float pl[6][4])
{
    float ty = 1.0f / fy, tx = ty * aspect;
    Vec3 n[6] = { fw, { -fw.x, -fw.y, -fw.z },
                  { fw.x * tx - rt.x, fw.y * tx - rt.y, fw.z * tx - rt.z },     /* the four side planes hold */
                  { fw.x * tx + rt.x, fw.y * tx + rt.y, fw.z * tx + rt.z },     /* |v.rt| <= tx * v.fw and */
                  { fw.x * ty - up.x, fw.y * ty - up.y, fw.z * ty - up.z },     /* |v.up| <= ty * v.fw */
                  { fw.x * ty + up.x, fw.y * ty + up.y, fw.z * ty + up.z } };
    float off[6] = { -zn, zf, 0, 0, 0, 0 };
    for (int i = 0; i < 6; i++) {
        pl[i][0] = n[i].x; pl[i][1] = n[i].y; pl[i][2] = n[i].z;
        pl[i][3] = off[i] - (n[i].x * cam->pos.x + n[i].y * cam->pos.y + n[i].z * cam->pos.z);
    }
}
#define CULL_SLACK 4.0f                        /* the boxes are exact; a hair of slack for the float rounding in them */
static int aabb_in_frustum(const float pl[6][4], const float b[6])
{
    for (int i = 0; i < 6; i++) {              /* the corner furthest along the normal: outside it, the box is outside */
        float x = pl[i][0] > 0 ? b[1] : b[0], y = pl[i][1] > 0 ? b[3] : b[2], z = pl[i][2] > 0 ? b[5] : b[4];
        if (pl[i][0] * x + pl[i][1] * y + pl[i][2] * z + pl[i][3] < -CULL_SLACK) return 0;
    }
    return 1;
}
static void idx_reserve(struct WorldBatch *b, uint32_t extra)
{
    if (b->nidx + extra <= b->idx_cap) return;
    b->idx_cap = (b->nidx + extra) * 2 + 256; b->idx = (uint32_t *)realloc(b->idx, (size_t)b->idx_cap * 4);
}
static void add_face(Renderer *r, uint32_t f)
{
    if (f >= r->gel->npolys || r->face_stamp[f] == r->stamp_gen) return;
    const float *pl = r->gel->polys[f].plane; Vec3 e = r->face_eye;
    if (pl[0] * e.x + pl[1] * e.y + pl[2] * e.z + pl[3] <= 0.0f) return;   /* 0x42b6eb..0x42b716: n.eye + D <= 0 ([0x4a9004] = 0) -> not drawn. Without it a
                                                                         * camera behind a wall (the door tracks put it outside the WWS jackpot room) saw its back */
    const struct FaceBatch *fb = &r->face_batch[f];
    if (r->race_vis && (int32_t)fb->zone != r->race_zone[0] && (int32_t)fb->zone != r->race_zone[1]) return;   /* 0x42ac32: only the polygons of a marked group carry the frame stamp */
    r->face_stamp[f] = r->stamp_gen;
    if (!fb->ntris) return;
    struct WorldBatch *b = (fb->lit ? r->litb : r->batches) + fb->group;
    idx_reserve(b, fb->ntris * 3);
    for (uint32_t k = 0; k < fb->ntris * 3; k++) b->idx[b->nidx++] = fb->tri0 * 3 + k;
}
static void world_visibility(Renderer *r, const FreeCamera *cam, const float pl[6][4])
{
    const GelFile *g = r->gel; uint32_t ns = g->nsectors;
    r->pvs_on = 0; r->race_vis = 0; r->sky_on = 1; r->race_zone[0] = r->race_zone[1] = -2;
    if (!r->cull || !ns || !g->sectors) { r->drawn_tris = r->total_tris; r->nsec_vis = ns; return; }
    memset(r->sec_vis, 0, ns);
    int32_t cs = r->cull >= 2 && r->vis ? gel_sector(g, cam->pos) : -1;
    uint32_t npairs = 0;
    const VisList *E = r->race[0] != -1 && cs >= 0 ? vis_entry(r->vis, g, cam->pos) : NULL;   /* 0x401c36: race list given and its first entry != -1 */
    if (E) {
        /* Race path of 0x42a980 (docs/RACE.md 2.1): the sectors are still every pair's first word of the camera's .vis entry
         * (0x408210: the entry whose id is the floor group under the camera), but the groups are not the pairs' second words:
         * only z = the floor group under the camera (0x42aae2) and the region list entry after z (0x42ab25). z = -1 marks no
         * group at all (0x42aaed), so no world face is drawn. The original searches z in the list without a bound
         * (0x42aaef..0x42aafb, past the -1 at +0xd4 into whatever follows); the port stops at the 5 entries and then takes z alone. */
        int32_t z = gel_floor_group(g, cam->pos), z2 = -1;
        if (z != -1) { int k = 0; while (k < 5 && r->race[k] != z) k++; if (k < 5) z2 = r->race[k + 1]; }
        r->race_vis = 1; r->pvs_on = 1; r->race_zone[0] = z; r->race_zone[1] = z2;
        for (uint32_t k = 0; k < E->npairs; k++) r->sec_vis[E->pairs[2 * k]] = 1;   /* no "own sector" here: the original takes the pairs only */
        npairs = E->npairs;
        /* The sky cube needs a sky face among the stamped faces of those sectors (0x42acea), before any frustum test:
         * with only two groups stamped it can go off. Recomputed when the entry or the two groups change. */
        if (E != (const VisList *)r->race_entry || z != r->race_prev[0] || z2 != r->race_prev[1]) {
            int sky = 0;
            for (uint32_t k = 0; k < E->npairs && !sky; k++) { const GelCell *S = &g->sectors[E->pairs[2 * k]];
                for (uint32_t q = 0; q < S->npolys && !sky; q++) { uint32_t f = S->polys[q]; if (f >= g->npolys) continue;
                    const struct FaceBatch *fb = &r->face_batch[f]; sky = fb->sky && ((int32_t)fb->zone == z || (int32_t)fb->zone == z2); } }
            r->race_entry = E; r->race_sky = sky; r->sec_dirty = 1;
        }
        r->sky_on = r->race_sky;
        if ((z != r->race_prev[0] || z2 != r->race_prev[1]) && wenv("WOODY_RACEVISLOG"))
            printf("  RACEVIS camera (%.0f %.0f %.0f) sector %d entry id %u (%u pairs): groups %d + %d, sky %d\n", cam->pos.x, cam->pos.y, cam->pos.z, cs, E->id, E->npairs, z, z2, r->race_sky);
    } else if (cs >= 0 && (uint32_t)cs < r->vis->nsectors) {                   /* 0x42a980: the .vis list of that sector */
        const VisSector *S = &r->vis->sectors[cs];
        for (uint32_t e = 0; e < S->nlists; e++) {
            const VisList *L = &r->vis->pool[S->first + e];
            npairs += L->npairs;
            for (uint32_t k = 0; k < L->npairs; k++) r->sec_vis[L->pairs[2 * k]] = 1;   /* the union of both lists: the
                                                     * meaning of the list id is not confirmed, and a union can only show too much */
        }
    }
    if (r->race_vis) {}
    else if (npairs) { r->sec_vis[cs] = 1; r->pvs_on = 1; }                    /* the camera's own sector is always in */
    else memset(r->sec_vis, 1, ns);                                            /* no sector, or an empty list: show everything */
    r->nsec_vis = 0;
    for (uint32_t i = 0; i < ns; i++) if (r->sec_vis[i]) { if (aabb_in_frustum(pl, g->sectors[i].bbox)) r->nsec_vis++; else r->sec_vis[i] = 0; }
    /* the index lists only have to be rebuilt when the set of sectors or the camera changed (the back-face test of add_face) */
    int moved = !r->face_eye_ok || r->face_eye.x != cam->pos.x || r->face_eye.y != cam->pos.y || r->face_eye.z != cam->pos.z;
    if (!r->sec_dirty && !moved && !memcmp(r->sec_vis, r->sec_prev, ns) && r->race_zone[0] == r->race_prev[0] && r->race_zone[1] == r->race_prev[1]) return;
    r->face_eye = cam->pos; r->face_eye_ok = 1;
    memcpy(r->sec_prev, r->sec_vis, ns); r->sec_dirty = 0; r->race_prev[0] = r->race_zone[0]; r->race_prev[1] = r->race_zone[1];
    for (uint32_t i = 0; i < r->nbatches; i++) { r->batches[i].nidx = 0; r->litb[i].nidx = 0; }
    if (++r->stamp_gen == 0) { memset(r->face_stamp, 0, (size_t)g->npolys * 4); r->stamp_gen = 1; }
    for (uint32_t i = 0; i < ns; i++) if (r->sec_vis[i]) { const GelCell *S = &g->sectors[i]; for (uint32_t k = 0; k < S->npolys; k++) add_face(r, S->polys[k]); }
    for (uint32_t k = 0; k < g->nloose; k++) add_face(r, g->loose[k]);         /* faces no sector lists: always drawn (race path: if in a marked group) */
    r->drawn_tris = 0;
    for (uint32_t i = 0; i < r->nbatches; i++) r->drawn_tris += (r->batches[i].nidx + r->litb[i].nidx) / 3;
    for (int t = 0; t < 16; t++) {              /* the .lit light polygons follow the face they lie on */
        struct WorldBatch *b = &r->lightb[t]; b->nidx = 0; if (!b->ntris || !b->face) continue;
        idx_reserve(b, b->ntris * 3);
        for (uint32_t q = 0; q < b->ntris; q++) if (b->face[q] >= g->npolys || r->face_stamp[b->face[q]] == r->stamp_gen)
            for (int c = 0; c < 3; c++) b->idx[b->nidx++] = q * 3 + c;      /* a fragment without a usable face is kept */
    }
}
/* the triangles of one batch that survived the visibility pass */
static void batch_draw(const Renderer *r, const struct WorldBatch *b)
{
    if (!r->cull) glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b->ntris * 3);
    else if (b->nidx) glDrawElements(GL_TRIANGLES, (GLsizei)b->nidx, GL_UNSIGNED_INT, b->idx);
}
static int batch_empty(const Renderer *r, const struct WorldBatch *b) { return r->cull ? !b->nidx : !b->ntris; }

int rnd_screenshot(const Window *w, const char *path)
{
    int W = w->width, H = w->height; uint8_t *px = (uint8_t *)malloc((size_t)W * H * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1); glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb"); if (!f) { free(px); return -1; }
    fprintf(f, "P6 %d %d 255\n", W, H);
    for (int y = H - 1; y >= 0; y--) fwrite(px + (size_t)y * W * 3, 1, (size_t)W * 3, f);
    fclose(f); free(px); return 0;
}

/* The original's player converts the 565 frame for a 32-bit back buffer through the table 0x4c93d0 (built in 0x425fa0: each channel
 * shifted up with its low bits set, r5 << 3 | 7 ...) and Blts it stretched to the screen rect (0x4263d0, message 0x8000 / 2). */
void rnd_film_frame(const Window *w, const uint16_t *px, int width, int height)
{
    static GLuint tex; static int tw, th; static uint8_t *rgb; static size_t rgb_n;
    if (!px) { if (tex) glDeleteTextures(1, &tex); tex = 0; free(rgb); rgb = NULL; rgb_n = 0; return; }
    if (!tex || width > tw || height > th) {
        if (tex) glDeleteTextures(1, &tex);
        for (tw = 64; tw < width; tw <<= 1) {} for (th = 64; th < height; th <<= 1) {}   /* OpenGL 1.1: power-of-two sizes */
        glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tw, th, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    }
    size_t n = (size_t)width * height;
    if (n > rgb_n) { free(rgb); rgb = (uint8_t *)malloc(n * 3); rgb_n = rgb ? n : 0; if (!rgb) return; }
    for (size_t i = 0; i < n; i++) { unsigned p = px[i]; rgb[i * 3] = (uint8_t)((p >> 11) << 3 | 7); rgb[i * 3 + 1] = (uint8_t)((p >> 5 & 63) << 2 | 3); rgb[i * 3 + 2] = (uint8_t)((p & 31) << 3 | 7); }
    glBindTexture(GL_TEXTURE_2D, tex); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    int W = w->width > 0 ? w->width : 1, H = w->height > 0 ? w->height : 1;
    float s = (float)W / width < (float)H / height ? (float)W / width : (float)H / height, dw = width * s, dh = height * s, x0 = (W - dw) / 2, y0 = (H - dh) / 2;
    glViewport(0, 0, W, H); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, H, 0, -1, 1); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_LIGHTING); glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST); glDisable(GL_FOG);
    glEnable(GL_TEXTURE_2D); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE); glColor4f(1, 1, 1, 1);
    float u = (float)width / tw, v = (float)height / th;
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(x0, y0); glTexCoord2f(u, 0); glVertex2f(x0 + dw, y0);
    glTexCoord2f(u, v); glVertex2f(x0 + dw, y0 + dh); glTexCoord2f(0, v); glVertex2f(x0, y0 + dh);
    glEnd();
    glDisable(GL_TEXTURE_2D); glEnable(GL_DEPTH_TEST);
}

void rnd_free(Renderer *r)
{
    sh_reset();
    for (uint32_t i = 0; i < r->nbatches; i++) { free(r->batches[i].pos); free(r->batches[i].uv); free(r->batches[i].col); free(r->batches[i].idx); free(r->litb[i].pos); free(r->litb[i].uv); free(r->litb[i].col); free(r->litb[i].idx); }
    free(r->batches); free(r->litb); free(r->face_bound); free(r->face_fan); free(r->fan_idx);
    free(r->face_batch); free(r->face_stamp); free(r->sec_vis); free(r->sec_prev); free(r->model_blend); free(r->links); r->links = NULL; r->nlinks = r->links_cap = 0;
    free(r->list); free(r->list_sec); free(r->list_grp); free(r->chain);
    for (int t = 0; t < 16; t++) { free(r->lightb[t].pos); free(r->lightb[t].uv); free(r->lightb[t].col); free(r->lightb[t].idx); free(r->lightb[t].face); if (r->light_tex[t]) { GLuint id = r->light_tex[t]; glDeleteTextures(1, &id); } }
    for (uint32_t g = 0; r->tex && g < r->tex->ngroups; g++) {                 /* the level's textures live in the GL context, not in the TexFile */
        TexGroup *tg = &r->tex->groups[g]; if (!tg->gl_frames) continue;
        for (uint32_t f = 0; f < tg->frame_count; f++) { GLuint id = tg->gl_frames[f]; if (id) glDeleteTextures(1, &id); }
        free(tg->gl_frames); tg->gl_frames = NULL; tg->gl_tex = 0;
    }
    memset(r, 0, sizeof *r);
}

/* ---------------------------------------------------------------- blend modes
 * .tex group flags (tex+0x44 in the exe): bit 0 colour key, bit 1 = blended (rendered additively here: the glare
 * plates are white-on-black), byte 2 = intensity/alpha, byte 3 = ground type (0x46295f). */
static int group_blended(const Renderer *r, uint32_t group) { return group < r->tex->ngroups && (r->tex->groups[group].flags & 2); }
static int mat_blended(const Renderer *r, uint32_t material) { return !(material & 0x8000) && material < r->tex->nmaterials && group_blended(r, r->tex->materials[material].group); }
/* Fading instances (0x43b504: alpha = (1 - inst+0x6c) * 255 < 252) do not go into the opaque batch list +0x1c0 but
 * into list +0x1c4, which 0x428d00 draws after the transparent world buckets: back to front in 254 depth buckets, and
 * every bucket twice with ZWRITE on - first SRC ZERO / DEST ONE (depth only, 0x428f10), then SRCALPHA / INVSRCALPHA
 * (0x428fdd), under the device's ZFUNC LESSEQUAL (0x47ec44), so only the front-most surface of the object blends.
 * The vertex alpha is that alpha (0x43bdc4 -> v+0x30 -> diffuse byte 3, 0x43d926), and ALPHAOP MODULATE (0x47ed82)
 * multiplies it with the texture's. ALPHATESTENABLE follows the texture's colour key bit (tex+0x44 & 1, 0x428f6b):
 * the port leaves the alpha test on everywhere else, which only differs once the vertex alpha drops.
 * g_fading: 0 = normal, 1 = depth-only pass, 2 = blend pass. */
static int g_fading; static float g_fade_alpha = 1.0f; static GLenum g_zfunc = GL_LESS;
static void set_blend(int blended)
{
    if (blended) { glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE); glDepthMask(GL_FALSE); }
    else if (g_fading) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_TRUE); }
    else { glDisable(GL_BLEND); glDepthMask(GL_TRUE); }
}
static int inst_fading(const Instance *inst) { return (1.0f - inst->fade) * 255.0f < 252.0f; }
static uint32_t g_last_material = 0xffffffffu, g_last_frame;
static int g_mat_blended;                            /* [0x5ac8d8]: the material now bound has group flag bit 1 */

/* ---------------------------------------------------------------- instances */
/* ---- model vertex batching: the model code below is written like immediate mode (begin / colour / texcoord / vertex),
 * but the vertices are collected in one array per material state and sent with glDrawArrays. Immediate mode cost
 * 25 ms per frame in the hubs. Fans are turned into triangles; bt_flush() must run before any GL state change. */
#define BT_STRIDE 9                                  /* x y z u v r g b a */
static struct { float *v; uint32_t n, cap; float col[3], uv[2], first[BT_STRIDE], prev[BT_STRIDE]; int fan, count; } g_bt;
/* The models are drawn MODULATE2X (0x429740) with a lit vertex colour clamped at 255 (0x43be20), so a vertex reaches up to
 * twice its texture. A colour here is in units of the texture (1.0 = 1 x texture) and may go up to 2.0; GL clamps a vertex
 * colour at 1, so a textured MODULATE batch that goes above it is drawn at half colour through ARB_texture_env_combine
 * with RGB_SCALE 2. Every other batch is drawn exactly as given. */
static void bt_flush(void)
{
    if (!g_bt.n) return;
    int x2 = 0;
    for (uint32_t i = 0; i < g_bt.n && !x2; i++) { const float *c = g_bt.v + (size_t)i * BT_STRIDE + 5; x2 = c[0] > 1.0f || c[1] > 1.0f || c[2] > 1.0f; }
    if (x2) { GLint mode = 0; if (glIsEnabled(GL_TEXTURE_2D)) glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &mode); x2 = mode == GL_MODULATE; }
    if (x2) {
        for (uint32_t i = 0; i < g_bt.n; i++) { float *c = g_bt.v + (size_t)i * BT_STRIDE + 5; c[0] *= 0.5f; c[1] *= 0.5f; c[2] *= 0.5f; }
        enum { COMBINE = 0x8570, COMBINE_RGB = 0x8571, COMBINE_ALPHA = 0x8572, RGB_SCALE = 0x8573, PRIMARY = 0x8577, SRC0_RGB = 0x8580, SRC1_RGB = 0x8581, SRC0_A = 0x8588, SRC1_A = 0x8589 };
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, COMBINE);
        glTexEnvi(GL_TEXTURE_ENV, COMBINE_RGB, GL_MODULATE); glTexEnvi(GL_TEXTURE_ENV, SRC0_RGB, GL_TEXTURE); glTexEnvi(GL_TEXTURE_ENV, SRC1_RGB, PRIMARY);
        glTexEnvi(GL_TEXTURE_ENV, COMBINE_ALPHA, GL_MODULATE); glTexEnvi(GL_TEXTURE_ENV, SRC0_A, GL_TEXTURE); glTexEnvi(GL_TEXTURE_ENV, SRC1_A, PRIMARY);
        glTexEnvf(GL_TEXTURE_ENV, RGB_SCALE, 2.0f);
    }
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_TEXTURE_COORD_ARRAY); glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, BT_STRIDE * 4, g_bt.v); glTexCoordPointer(2, GL_FLOAT, BT_STRIDE * 4, g_bt.v + 3); glColorPointer(4, GL_FLOAT, BT_STRIDE * 4, g_bt.v + 5);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_bt.n);
    glDisableClientState(GL_VERTEX_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY); glDisableClientState(GL_COLOR_ARRAY);
    if (x2) { glTexEnvf(GL_TEXTURE_ENV, 0x8573, 1.0f); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE); }
    g_bt.n = 0;
}
static void bt_push(const float *v9)
{
    if (g_bt.n + 1 > g_bt.cap) { g_bt.cap = g_bt.cap * 2 + 4096; g_bt.v = (float *)realloc(g_bt.v, (size_t)g_bt.cap * BT_STRIDE * sizeof(float)); }
    memcpy(g_bt.v + (size_t)g_bt.n * BT_STRIDE, v9, BT_STRIDE * sizeof(float)); g_bt.n++;
}
static void bt_begin(int fan) { g_bt.fan = fan; g_bt.count = 0; }
static void bt_end(void) { }
static void bt_color(float r, float g, float b) { g_bt.col[0] = r; g_bt.col[1] = g; g_bt.col[2] = b; }
static void bt_texcoord(float u, float v) { g_bt.uv[0] = u; g_bt.uv[1] = v; }
static void bt_vertex(float x, float y, float z)
{
    float v[BT_STRIDE] = { x, y, z, g_bt.uv[0], g_bt.uv[1], g_bt.col[0], g_bt.col[1], g_bt.col[2], g_fade_alpha };
    if (!g_bt.fan) { bt_push(v); return; }
    if (g_bt.count == 0) memcpy(g_bt.first, v, sizeof v);
    else if (g_bt.count >= 2) { bt_push(g_bt.first); bt_push(g_bt.prev); bt_push(v); }
    memcpy(g_bt.prev, v, sizeof v); g_bt.count++;
}

static float g_tex_now;                                          /* game time of this frame, for the per instance texture override */
static uint32_t tex_frame(const Instance *I, const TexGroup *g)   /* frame mode B of 0x47f290 (docs/INSTANCE.md 2), n = frame_count, P = duration x factor */
{
    int n = (int)g->frame_count; float P = g->anim_duration * I->tex_fac;
    if (n < 2 || P <= 0) return 0;
    float t = g_tex_now - I->tex_t0, u = t / P; int k, f = 0;
    u = u - floorf(u);                                           /* the looping modes run on the fraction, the one shot modes test t against P first */
    switch (I->tex_mode) {
    case 1: f = t >= P ? n - 1 : (int)(t / P * n); break;
    case 2: f = t >= P ? 0 : (int)((1.0f - t / P) * n); break;
    case 3: if (t >= P) { f = 0; break; } k = (int)(t / P * (2 * n - 1)); f = k >= n ? 2 * n - 1 - k : k; break;
    case 4: f = (int)(u * n); break;
    case 5: f = (int)((1.0f - u) * n); break;
    case 6: k = (int)(u * (2 * n - 1)); f = k >= n ? 2 * n - 1 - k : k; break;
    }
    return (uint32_t)(f < 0 ? 0 : f > n - 1 ? n - 1 : f);
}

/* UV scroll mode A of 0x47f290 (docs/INSTANCE.md 2), messages 15 / 17: only for a texture with a scroll speed (tex+0x48 / +0x4c,
 * the .tex group's scroll_u / scroll_v; only K3A g76, S3A g75 and W3A g77 have one) and only while an instance override is
 * on - without one, nothing in the engine scrolls a model texture. The offset goes onto the material's constant terms
 * (material+0xc = m[9] for u, +0x1c = m[10] for v) for this one polygon and is taken off again after it (0x43c369). */
static int tex_scroll(const Instance *I, const TexGroup *g, float *du, float *dv)
{
    int md = I->uv_mode; if (!md || (g->scroll_u == 0 && g->scroll_v == 0)) return 0;
    float t = g_tex_now - I->uv_t0, su = g->scroll_u * I->uv_fac, sv = g->scroll_v * I->uv_fac;
    if (md == 1 || md == 2) { if (!(t < I->uv_t2)) t = I->uv_t2; }              /* 15: the scroll stops after T2 */
    else if (md != 4 && md != 5) return 0;                                      /* jump table 0x47f61c: mode 3 moves nothing */
    su *= t; sv *= t; su -= floorf(su); sv -= floorf(sv);                        /* 0x499ede floor: only the fraction is added */
    if (md == 2 || md == 5) { su = -su; sv = -sv; }                             /* a2 = 0: backwards (0x47f5d8 fsubr) */
    *du = su; *dv = sv; return 1;
}

static void set_material(const Renderer *r, uint32_t material, const Material **mat_out, uint32_t frame, const Instance *inst)   /* frame: models never auto-cycle (0x47f290), only an override does */
{
    /* GL state = (texture or none, blend mode). Every polygon has its own material record (a planar projection), so the
     * batch is keyed on the state, not on the material index: g_last_material holds texture id + 1 (0 = untextured) | blend << 31 */
    *mat_out = NULL; g_mat_blended = 0;
    uint32_t tex = 0; int bl = 0, key_bit = 0; float col[3] = { 1, 0, 1 };
    if (material & 0x8000) argb1555_to_rgb(material, col);
    else if (material < r->tex->nmaterials) {
        const Material *m = &r->tex->materials[material]; *mat_out = m; const TexGroup *g = &r->tex->groups[m->group];
        bl = g_mat_blended = (g->flags & 2) != 0;        /* byte 2 of the flags looks like an intensity, but nothing in the engine reads tex+0x46 */
        key_bit = g->flags & 1;
        if (!frame && inst && inst->tex_mode) frame = tex_frame(inst, g);
        tex = g->gl_frames[frame < g->frame_count ? frame : 0];
    }
    uint32_t key = (tex + 1) | (uint32_t)bl << 31;
    if (key != g_last_material) {
        bt_flush(); g_last_material = key; set_blend(bl);
        if (g_fading) { if (key_bit) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST); }   /* 0x428f6b: the fade list tests the colour key bit */
        if (tex) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, tex); } else glDisable(GL_TEXTURE_2D);
    }
    bt_color(col[0], col[1], col[2]);
}

/* ---- model lighting (0x42e3e4 light choice, 0x43b912 light vector, 0x43bce4 vertex colour; docs/LIGHTING.md 3).
 * The light choice is per instance, the light vector per model part (instance_light). */
static const int32_t *model_owner(Model *m) { ins_point_owner(m, 0); return m->owner; }   /* the table level.c builds */
/* 0x42e3e4..0x42e573: the candidate lights of an instance are the lights of its sector (the .lit trailer, lightsys+0x10,
 * indexed by inst+0x1c). n == 0 means no light and no shadow; n == 1 is taken without any test; otherwise the first
 * light that sees the point wins and, when none does, the one whose shadow plane the point is least far behind. Taking
 * the first light of the whole level instead put everyone inside the House under the sun that stands over the village,
 * 15000 units away, and its lit faces are all outdoors - so nothing there ever received a shadow. */
static int sector_light(const Renderer *r, Vec3 p, int *have_list)
{
    const LitFile *lf = r->lit; const GelFile *g = r->gel;
    int32_t sec = gel_sector(g, p);
    *have_list = sec >= 0 && (uint32_t)sec < lf->nsectors;
    if (!*have_list) return -1;
    const LitSector *S = &lf->sectors[sec];
    if (S->n == 1) return S->idx[0] < lf->nlights ? (int)S->idx[0] : -1;        /* 0x42e422: no visibility or range test */
    int fallback = -1; float fb_d = -1e30f;
    for (uint32_t k = 0; k < S->n; k++) {
        uint32_t li = S->idx[k]; if (li >= lf->nlights) continue;
        const LitLight *L = &lf->lights[li];
        int32_t f = lit_bsp_face(L, g, p);
        if (f < 0) {                                                            /* 0x42e4c2: in the open part of the light volume */
            float dx = L->pos.x - p.x, dy = L->pos.y - p.y, dz = L->pos.z - p.z;
            if (dx * dx + dy * dy + dz * dz < L->range * L->range) return (int)li;
            continue;
        }
        const float *fp = g->polys[f].plane; float d = fp[0] * p.x + fp[1] * p.y + fp[2] * p.z + fp[3];
        if (d > 0.0f) return (int)li;                                           /* 0x42e541: in front of the leaf face, no range test */
        if (d > fb_d) { fb_d = d; fallback = (int)li; }                         /* 0x42e4a4: least far behind it */
    }
    return fallback;
}
static int affine_inv_apply(const Mat4 *M, Vec3 w, Vec3 *out);
/* The light vector is kept per mesh part, in the part's own space (inst+0xf4, 0x18 B each; 0x43b912..0x43bc4c), and the
 * part is lit by the instance's chosen light when the light's shadow BSP sees the part's origin - the translation of its
 * node matrix (0x43ba2d: leaf -1, or in front of the leaf face; no range test in world units). The light then goes into
 * the part's space through the inverse of the SCALED node matrix (0x440fc0 with the instance scale inst+0x4c..0x54,
 * 0x43ba86), less the node pivot N+0x20, and the falloff 1 - d/R and the range test d < R (0x43bb74) use that local
 * distance: an instance drawn at scale 4 is lit as if the light stood four times closer. The W3B knight statues by the
 * door (model 8, scale 4) stand 1791 units from light 11 (R 1600) - out of range in world units, grey 0.6 x vcol, but
 * 448 local units and k = 0.72 of its orange in the original. The vertex then takes N.Ldir with its own rest normal
 * (0x43bce4); a skinned vertex the colour its owner part computed. */
static void instance_light(const Renderer *r, Instance *inst, float dt)
{
    const LitFile *lf = r->lit; Model *m = inst->model; Vec3 p = ins_anim_centre(inst); p.y += 20.0f;   /* inst+0x60, the animated root (issue #35) */
    int have_list = 0, chosen = sector_light(r, p, &have_list), fallback = -1; float cd = 0;
    if (!have_list)                                                             /* no trailer in this .lit: the whole light list, as before */
        for (uint32_t l = 0; l < lf->nlights; l++) {
            const LitLight *L = &lf->lights[l]; float dx = L->pos.x - p.x, dy = L->pos.y - p.y, dz = L->pos.z - p.z, d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d < L->range && lit_point_lit(L, r->gel, p)) { chosen = (int)l; break; }
            if (fallback < 0 || d / L->range < 1.0f) fallback = (int)l;
        }
    if (chosen < 0) chosen = fallback;
    if (chosen >= 0) { const LitLight *L = &lf->lights[chosen]; float dx = L->pos.x - p.x, dy = L->pos.y - p.y, dz = L->pos.z - p.z; cd = sqrtf(dx * dx + dy * dy + dz * dz); }
    inst->l_seen = chosen >= 0 && cd < lf->lights[chosen].range && lit_point_lit(&lf->lights[chosen], r->gel, p);   /* the root, for the logs */
    float keep = powf(0.85f, dt * 60.0f); if (!inst->l_init) keep = 0;          /* Ldir *= 0.85 per frame [0x4aa3d8] */
    inst->light = chosen; inst->l_init = 1;
    if (!inst->plight || !inst->node_world) return;
    const LitLight *L = chosen >= 0 ? &lf->lights[chosen] : NULL;
    for (uint32_t k = 0; k < m->nmesh_nodes; k++) {
        uint32_t ni = m->mesh_nodes[k] - 1; if (ni >= m->nnodes || m->nodes[ni].type_code == 2) continue;   /* 0x43b6c2 */
        float *pl = &inst->plight[ni * 6]; const Mat4 *M = &inst->node_world[ni];
        pl[0] *= keep; pl[1] *= keep; pl[2] *= keep;
        Vec3 t = { M->m[12], M->m[13], M->m[14] }, q;
        if (!L || !lit_point_lit(L, r->gel, t) || !affine_inv_apply(M, L->pos, &q)) continue;
        q.x -= m->nodes[ni].pivot.x; q.y -= m->nodes[ni].pivot.y; q.z -= m->nodes[ni].pivot.z;
        float d = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z); if (d >= L->range || d < 1e-6f) continue;
        float f = (1.0f - keep) * (1.0f - d / L->range) / d;                      /* 0.15 [0x4aa1c8] x normalize(q) x (1 - d/R) */
        pl[0] += q.x * f; pl[1] += q.y * f; pl[2] += q.z * f;
        pl[3] = L->colour[0]; pl[4] = L->colour[1]; pl[5] = L->colour[2];
    }
    if (wenv("WOODY_LITLOG") && inst->drawn && m->nmesh_nodes) { const float *pl = &inst->plight[(m->mesh_nodes[0] - 1) * 6];   /* testing: the first part */
        printf("LITLOG idx %u at %.0f %.0f %.0f light %d root seen %d cd %.0f part0 ldir %.3f %.3f %.3f col %.0f %.0f %.0f\n", inst->index, inst->position.x, inst->position.y, inst->position.z, inst->light, inst->l_seen, cd, pl[0], pl[1], pl[2], pl[3], pl[4], pl[5]); }
}
/* Who casts a shadow: the player, SetFlags bit 1 (0x42b3cc); the other types are our addition */
static int shadow_caster(const Instance *inst)
{
    return inst->type == 1 || inst->type == 2 || inst->type == 3 || inst->type == 18 || inst->type == 19 || (inst->setflags & 1) || (inst->type >= 4 && inst->type <= 13);
}
static Vec3 g_cam_pos;                 /* the camera of this frame, for the per polygon back-face test and the culling */
/* Drawn only as a member of the frame's instance list (0x42a840 vtbl[2](0x81), 0x42b380 vtbl[2](5/7)): the base-class instances
 * and the enemies. Not the Perso (0x42b380 draws it first, outside the loop), not the bomb pool (0x44d820 ticks it with vtbl[2](1))
 * and not the links an actor draws itself (boss saucer, pads, race board: not scripted). The sector/group part of the list
 * (in_zone) is what gates here; the cone test stays instance_visible's, on this frame's camera. */
static int list_drawn(const Renderer *r, const Instance *in) { return in != r->perso && ((in->scripted && in->type != 40) || (in->type >= 4 && in->type <= 16)); }

/* Is this instance drawn at all this frame? The cone test on the model's bounding sphere is what the port already
 * did. On top of it, 0x42aa0b only walks the instances that belong to the sectors the visibility pass kept, so an
 * instance whose sphere lies entirely in sectors the camera cannot see goes as well. Whenever the sector lookup
 * cannot be sure - a sphere reaching outside the level, or more sectors than fit in the buffer - the instance stays. */
static int instance_visible(Renderer *r, Instance *inst, float aspect, float fy, Vec3 fw, Vec3 rt, Vec3 up)
{
    Model *m = inst->model;
    if (m->cull_r <= 0) { float mx = 1; for (uint32_t pi = 0; pi < m->npoints; pi++) { const Vec3 *q = &m->points[pi].pos; float d2 = q->x * q->x + q->y * q->y + q->z * q->z; if (d2 > mx) mx = d2; } m->cull_r = sqrtf(mx) * 1.5f + 50.0f; }
    const float *wm = inst->node_world ? inst->node_world[0].m : inst->world.m;
    float sx = sqrtf(wm[0] * wm[0] + wm[1] * wm[1] + wm[2] * wm[2]); if (sx < 1) sx = 1;
    float R = m->cull_r * sx, dx = wm[12] - g_cam_pos.x, dy = wm[13] - g_cam_pos.y, dz = wm[14] - g_cam_pos.z;
    float vz = dx * fw.x + dy * fw.y + dz * fw.z, vx = dx * rt.x + dy * rt.y + dz * rt.z, vy = dx * up.x + dy * up.y + dz * up.z;
    float ty = 1.0f / fy, tx = ty * aspect, zz = vz + R;
    if (zz < 0 || fabsf(vx) > zz * tx + R * 1.5f || fabsf(vy) > zz * ty + R * 1.5f) return 0;
    if (r->pvs_on) {
        float box[6] = { wm[12] - R, wm[12] + R, wm[13] - R, wm[13] + R, wm[14] - R, wm[14] + R };
        int32_t sec[64]; uint32_t n = gel_sectors_in_box(r->gel, box, sec, 64), k;
        if (n && n < 64) { for (k = 0; k < n; k++) if (r->sec_vis[sec[k]]) break; if (k == n) return 0; }
    }
    return 1;
}

/* ---- message 34 [inst, other] (0x42dc21, docs/INSTANCE.md 10.1): a pair {other, next} in the level table +0x50 (count
 * +0x4c), pushed on inst+0xd4. The one reader is the visibility pass 0x42a980: for every type-1 instance registered in
 * the camera's kd leaf cell (.col list cell+0x40/+0x44) whose volume node holds the camera position (0x4300c0,
 * 0x42aa38), each linked instance gets this frame's stamp +0x20 = [0x4c4c08] (0x42aa4b), and the sector walk 0x42a840
 * skips an instance that already carries it (0x42a92f): not drawn, not updated. A volume instance is listed in every
 * leaf its volume touches, so "in the camera's leaf and the camera inside the volume" is the volume test alone here. */
void rnd_link(Renderer *r, Instance *inst, Instance *other)
{
    if (!inst || !other) return;
    if (r->nlinks >= r->links_cap) {
        uint32_t cap = r->links_cap ? r->links_cap * 2 : 64; Instance **n = (Instance **)realloc(r->links, cap * 2 * sizeof *n);
        if (!n) return;
        r->links = n; r->links_cap = cap;
    }
    r->links[2 * r->nlinks] = inst; r->links[2 * r->nlinks + 1] = other; r->nlinks++;
}
static int link_inside(const Instance *v, Vec3 eye)                      /* 0x4300c0 on each volume node of v */
{
    if (!v->visible || !v->node_world) return 0;                            /* hidden by message 6 = in no cell list (0x407850) */
    for (uint32_t k = 0; k < v->model->nvolume_nodes; k++) {
        uint32_t node = v->model->volume_nodes[k] - 1;                       /* the node lists in the file are 1-based */
        if (node < v->model->nnodes && volume_contains(v, node, eye)) return 1;
    }
    return 0;
}
static void links_hide(Renderer *r, Vec3 eye)
{
    static int log = -1; if (log < 0) log = wenv("WOODY_LINKLOG") != NULL;
    const Instance *last = NULL; int inside = 0, hidden = 0;
    for (uint32_t i = 0; i < r->nlinks; i++) {
        Instance *v = r->links[2 * i], *o = r->links[2 * i + 1];
        if (v != last) {                                                     /* the pairs of one volume instance come in a row */
            last = v; inside = 0;
            if (log == 1 && v->node_world && v->model->nvolume_nodes) {         /* once: where the volume is */
                const InsNode *n = &v->model->nodes[v->model->volume_nodes[0] - 1]; float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
                for (uint32_t q = 0; q < n->npoints; q++) { Vec3 w = ins_point_world(v, n->point_base + q); float a[3] = { w.x, w.y, w.z }; for (int c = 0; c < 3; c++) { if (a[c] < lo[c]) lo[c] = a[c]; if (a[c] > hi[c]) hi[c] = a[c]; } }
                uint32_t cnt = 0; for (uint32_t j = i; j < r->nlinks && r->links[2 * j] == v; j++) cnt++;
                printf("link: inst %u hides %u instances while the camera is in its volume, box x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f", v->index, cnt, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]), puts("");
                if (i + cnt >= r->nlinks) log = 2;
            }
            inside = link_inside(v, eye);
            if (log) { static const Instance *was[8]; static int in_was[8]; int s = 0;
                while (s < 7 && was[s] && was[s] != v) s++;
                if (was[s] != v) { was[s] = v; in_was[s] = 0; }
                if (in_was[s] != inside) { in_was[s] = inside; printf("link: camera %s volume of inst %u at %.0f %.0f %.0f", inside ? "entered" : "left", v->index, eye.x, eye.y, eye.z), puts(""); } }
        }
        if (inside && o->drawn) { o->drawn = 0; hidden++; }
    }
    if (log && hidden) { static int prev = -1; if (hidden != prev) printf("link: %d linked instances hidden", hidden), puts(""); prev = hidden; }
}

/* ---- the per-frame instance list world+0x60/+0x64 (0x42a980 -> 0x42a840; docs/INSTANCE.md 4.1) ---------------------------
 * Built at frame step 9 (0x401c63) from the camera position of the camera manager, before any Think. Its readers: the Thinks
 * 0x42b400 (vtbl[3] of every listed instance: enemies and bosses 0x41a320, the ambient volume 0x472560, bonus halos, the
 * carousel figures ...), the draw/shadow pass 0x42b380, the skeleton list 0x401c68 and the sound Update 0x401ee7 (3D voices
 * of unlisted owners fall silent). An instance is listed when
 *   - it is in the world (+0x1c != -1; message 6 / 0x407850 takes it out) and its sector +0x1c is the first word of a pair
 *     of the camera's .vis list (0x408210: sector of the camera 0x4081c0, then its entry whose id is the floor group of the
 *     camera 0x40a0c0, else the first entry) - 0x42a840 walks that sector's chain sector+0x44 -> inst+0x24;
 *   - its floor group +0x18 is stamped this frame (0x42a867..0x42a87d): the second words of the same pairs, or in a race
 *     (Perso subtypes 4/5 with a region list world+0xc0 whose first entry is not -1) only the camera's group and the list
 *     entry after it (0x42aadf..0x42ab52);
 *   - no message-34 volume holding the camera links it (stamp +0x20, 0x42aa4b);
 *   - kind 1 with a cached bounding sphere (+0x88 == 1: a stationary instance, see sphere_cached): the sphere passes the
 *     four side planes (0x437b00, margin r * 1.4142) and, in a race only, |centre - camera|^2 + r^2 <= 1.21e8 (11000,
 *     0x42a8c8..0x42a91a). Moving instances and actors (enemies, the Perso: flag 0x20) are never frustum tested.
 * The list is malloc(0x400) = 256 pointers (0x42a4f9) and 0x42a931 writes without a bound; the port grows it and
 * WOODY_VISLOG reports a frame above 256. */
/* +0x88 == 1 (0x42eec9..0x42f008): the draw 0x42e2b0 caches the sphere of an instance whose clock stands still (speed
 * +0xa0 == 0), without flag 0x20 (the actors: enemy PostLoad 0x419e4d, the Perso, the race board), without an .ins path
 * (the loader sets +0x88 = 2, 0x42868b), without SetFlags bit 1 (0x42efd7), not a laser 50-52 (0x450db2) or a launcher 42
 * (0x45223e: both 2). Port: scripted = the base-class clock owns the instance (the actors are not scripted). The original
 * caches on the first draw after the instance stopped; the port counts it as cached from that frame on. */
static int sphere_cached(const Instance *in)
{
    return in->scripted && in->a_speed == 0 && !in->traj.npoints && !(in->setflags & 1) && !(in->type >= 50 && in->type <= 52) && in->type != 42;
}
/* the cached sphere: centre = the sum of the bounding-box node's points (model+0x24, 1-based) times 1/8 (0x4a9db8),
 * radius = the distance from it to the node's last point (0x42ef7e) */
static int inst_sphere(const Instance *in, Vec3 *c, float *rad)
{
    const Model *m = in->model; uint32_t bn = m->bbox_node;
    if (!in->node_world || !bn || bn > m->nnodes || !m->nodes[bn - 1].npoints) return 0;
    const InsNode *n = &m->nodes[bn - 1]; Vec3 s = { 0, 0, 0 }, q = s;
    for (uint32_t k = 0; k < n->npoints; k++) { q = ins_point_world(in, n->point_base + k); s.x += q.x; s.y += q.y; s.z += q.z; }
    c->x = s.x * 0.125f; c->y = s.y * 0.125f; c->z = s.z * 0.125f;
    *rad = sqrtf((c->x - q.x) * (c->x - q.x) + (c->y - q.y) * (c->y - q.y) + (c->z - q.z) * (c->z - q.z));
    return 1;
}
/* ---- the .col file (0x4271e0) and the sector chains -----------------------------------------------------------------
 * .col: for every kd leaf cell of the .gel (in .gel order) a u32 count and that many refs (mask << 16 | object index); the
 * object index addresses the level's object table world+0x40 = the script slots. 0x42aa0b reads the list of the camera's
 * leaf (cell+0x40 / +0x44), and every collision query walks the lists of the cells it visits (level.c gel_col_instances),
 * so the file is kept in the level's GelFile. */
int rnd_load_col(Renderer *r, const char *path)
{
    return gel_col_load((GelFile *)r->gel, path);
}
/* 0x407850: unlink from the chain of its sector (+0x1c = +0x18 = -1) */
static void chain_unlink(Renderer *r, Instance *in)
{
    if (!in->chain_sec1) return;
    Instance **pp = &r->chain[in->chain_sec1 - 1];
    while (*pp && *pp != in) pp = &(*pp)->cell_next;
    if (*pp) *pp = in->cell_next;
    in->cell_next = NULL; in->chain_sec1 = 0;
}
/* the cell point of a clock run or an actor's own re-cell: an enemy's collision centre (0x4077f0 in its mover), a flag-0x20
 * link its .ins position (the last explicit 0x4077f0(NULL)), everything else the animated root inst+0x60 (the clock 0x43f2f1) */
static Vec3 cell_point(const Instance *in)
{
    return in->cell_dy > 0 ? (Vec3){ in->position.x, in->position.y + in->cell_dy, in->position.z } : in->cell_fixed ? in->position : ins_anim_centre(in);
}
/* +8 & 0x20 (level.c ins_flag20): the actors move and re-cell themselves (the Perso, enemies and bosses, the race board, the
 * rocket, the bomb cannon, the bombs), the links of messages 61/62 are never re-celled by their clock */
static int inst_flag20(const Instance *in) { return ins_flag20(in); }
static int inst_actor(const Instance *in) { return in->cell_dy > 0 || (inst_flag20(in) && !in->cell_fixed); }
/* the point of 0x407790(NULL) / 0x4077f0(NULL): the .ins position +0xc. The loader (0x4288cf), a show (message 6, 0x42d99b),
 * the SetTypeInstance relink (0x403e7a, ebx = 0 from 0x403524) and the bonus respawns (0x44f595, 0x44f8f5) all pass NULL; only
 * a clock run (0x43f351) or an actor's mover passes a point of its own. For the actors the port takes their mover's point. */
static Vec3 load_point(const Instance *in) { return inst_actor(in) ? cell_point(in) : in->position; }
static void cell_compute(const GelFile *g, Instance *in, Vec3 ref)    /* 0x4081c0 sector, 0x40a0c0 floor group (cached per point) */
{
    if (!in->cell_ok || ref.x != in->cell_ref.x || ref.y != in->cell_ref.y || ref.z != in->cell_ref.z) {
        in->cell_ref = ref; in->cell_ok = 1; in->cell_sec = gel_sector(g, ref); in->cell_grp = gel_floor_group(g, ref);
    }
}
/* 0x4077f0 / 0x407790 at point p: out of its chain and IN FRONT of the chain of the sector of p (none: out of the world,
 * +0x1c = -1, and only a show or an actor's mover brings it back), +0x18 = the floor group under p (0x40a0c0) */
static void chain_recell(Renderer *r, Instance *in, Vec3 p)
{
    chain_unlink(r, in);
    cell_compute(r->gel, in, p);
    if (in->cell_sec >= 0 && (uint32_t)in->cell_sec < r->nchain) { in->cell_next = r->chain[in->cell_sec]; r->chain[in->cell_sec] = in; in->chain_sec1 = in->cell_sec + 1; }
}
/* the clock 0x43eee0 runs at most once per frame (inst+0x58 = [[0x509adc]]) and ends with the re-cell 0x43f351 - unless
 * the pose cache hits: 0x42e2b0 asks 0x42f3d0 for it when fade +0x6c < 0.01 (valid while the clock speed is 0 and the
 * position +0xc, the animation position +0xac and slot0 +0xb0 are those of the stored pose), and the clock then copies the
 * cached matrices and returns before the re-cell (0x43efff -> 0x43f06e; with a TRAJ +0x78 it never takes that branch).
 * The draw stores the cache (0x42ecf8, fade <= 0.98) when the speed is 0 and clears it otherwise (0x42f460 -> 0x42f483).
 * So a stationary opaque instance keeps its place in its chain, and only animated, moving or fading ones go to the front
 * (verified live with tools/wverify.py --probe list: W1A start, only the looping pairs 11/12, 14/15 and 77/79 swap each frame).
 * The clock is the ONLY re-cell of a non-actor after it entered the world: an instance keeps the cell of its .ins position
 * until its clock first runs - listed (0x42a94e), the camera leaf's .col objects (0x42aa0b) or a collision query that tests
 * it (0x4324d6, level.c gel_col_clock) - and afterwards the cell of its animated root as of its last clock run.
 * draw = the list's draw vtbl[2](5/7) follows (0x42b380), which is what stores or clears the pose cache. */
static void chain_clock(Renderer *r, Instance *in, int draw)
{
    if (in->clock_frame != r->frame) {
        in->clock_frame = r->frame;
        int hit = in->pc_ok && in->fade < 0.01f && in->a_speed == 0 && !in->traj.npoints && in->a_pos == in->pc_ac && in->slot[0] == in->pc_slot
                  && in->position.x == in->pc_pos.x && in->position.y == in->pc_pos.y && in->position.z == in->pc_pos.z;
        if (!hit && !inst_flag20(in)) chain_recell(r, in, ins_anim_centre(in));   /* 0x43f2f1..0x43f351 */
    }
    if (!draw) return;
    if (in->a_speed != 0) in->pc_ok = 0;
    else if (in->fade <= 0.98f) { in->pc_ok = 1; in->pc_pos = in->position; in->pc_ac = in->a_pos; in->pc_slot = in->slot[0]; }
}
static Renderer *g_clock_r;                                                  /* the renderer of the current level, for gel_col_clock */
static void query_clock(Instance *in) { if (g_clock_r && g_clock_r->chain_ok && in->visible && in->chain_sec1) chain_clock(g_clock_r, in, 0); }   /* a hidden or out-of-world instance is never tested (0x43248e) */
static uint32_t g_link_seq;
void rnd_note_link(Instance *in) { if (in) in->link_seq = ++g_link_seq; }
static int link_cmp(const void *a, const void *b) { uint32_t x = (*(Instance *const *)a)->link_seq, y = (*(Instance *const *)b)->link_seq; return x < y ? -1 : x > y; }
/* the SetTypeInstance relinks (0x403e7a) since the last list, in message order: each new object goes in front of its chain */
static void chains_relink(Renderer *r)
{
    InsFile *ins = r->ins; Instance *buf[256]; uint32_t n = 0, top = r->link_done;
    for (uint32_t mi = 0; mi < ins->nmodels; mi++) for (uint32_t k = 0; k < ins->models[mi].ninstances; k++) {
        Instance *in = &ins->models[mi].instances[k];
        if (in->link_seq > r->link_done) { if (in->link_seq > top) top = in->link_seq; if (n < 256) buf[n++] = in; }
    }
    if (!n) return;
    qsort(buf, n, sizeof *buf, link_cmp);
    for (uint32_t i = 0; i < n; i++) if (buf[i]->visible) chain_recell(r, buf[i], load_point(buf[i]));   /* 0x403e7a: 0x407790(NULL) */
    r->link_done = top;
}
/* keep the chains in step with what happened since the last frame: 0x4288cf put every instance of the .ins in front of
 * its sector's chain in file order at load (cell point: the .ins position), then the script's 1200s (chains_relink);
 * message 6 unlinks (0x407850, +0x1c = +0x18 = -1) and a show links in front again at the .ins position (0x42d99b); an
 * actor's own mover re-cells it in front whenever it moved. Nothing else moves an instance between chains: the clock
 * (chain_clock) is its only re-cell, so a moving instance that is neither listed nor tested by a query keeps its old cell,
 * and one whose re-cell found no sector stays out of the world (the original's +0x1c = -1: no clock, no list, no query). */
static void chains_sync(Renderer *r)
{
    const GelFile *g = r->gel; InsFile *ins = r->ins;
    if (!r->chain_ok) {
        free(r->chain); r->nchain = g->nsectors; r->chain = (Instance **)calloc(r->nchain ? r->nchain : 1, sizeof *r->chain); r->chain_ok = 1;
        for (uint32_t mi = 0; mi < ins->nmodels; mi++) for (uint32_t k = 0; k < ins->models[mi].ninstances; k++) { Instance *in = &ins->models[mi].instances[k]; in->chain_sec1 = 0; in->cell_next = NULL; in->cell_ok = 0; }
        for (uint32_t mi = 0; mi < ins->nmodels; mi++) for (uint32_t k = 0; k < ins->models[mi].ninstances; k++) {
            Instance *in = &ins->models[mi].instances[k]; if (in->visible) chain_recell(r, in, load_point(in));   /* 0x4288cf: 0x407790(NULL), in file order */
        }
        chains_relink(r);                                                              /* then the level script's init 1200s */
        return;
    }
    chains_relink(r);
    for (uint32_t mi = 0; mi < ins->nmodels; mi++) for (uint32_t k = 0; k < ins->models[mi].ninstances; k++) {
        Instance *in = &ins->models[mi].instances[k];
        if (!in->visible) { if (in->chain_sec1) chain_unlink(r, in); in->cell_ok = 0; continue; }   /* message 6: 0x407850 */
        if (!in->cell_ok) { chain_recell(r, in, load_point(in)); continue; }          /* shown again: 0x407790(NULL) */
        if (!inst_actor(in)) continue;                                                 /* only its clock re-cells it */
        Vec3 ref = cell_point(in);
        if (ref.x != in->cell_ref.x || ref.y != in->cell_ref.y || ref.z != in->cell_ref.z) chain_recell(r, in, ref);   /* an actor that moved: its own 0x4077f0 */
    }
}
void rnd_instance_list(Renderer *r, const Window *w, const FreeCamera *cam, const int32_t *race)
{
    const GelFile *g = r->gel; InsFile *ins = r->ins;
    static int log = -1; if (log < 0) { const char *e = wenv("WOODY_VISLOG"); log = e ? atoi(e) : 0; if (e && !log) log = 1; }
    if (!r->list_sec && g->nsectors) r->list_sec = (uint8_t *)calloc(g->nsectors, 1);
    if (!r->list_grp && g->ngroups) r->list_grp = (uint8_t *)calloc(g->ngroups, 1);
    r->nlist = 0; r->list_on = 1;                                               /* 0x42a98f: +0x60 = 0 */
    /* 0x408210: the camera's .vis list */
    int32_t cs = gel_sector(g, cam->pos), cg = gel_floor_group(g, cam->pos);
    const VisList *L = r->vis ? vis_entry(r->vis, g, cam->pos) : NULL;
    int32_t ent = L ? (int32_t)(L - &r->vis->pool[r->vis->sectors[cs].first]) : -1;
    int all = !L || !r->list_sec || !r->list_grp;                                /* port: no .vis, or a camera outside every sector: everything is listed */
    int racing = race && race[0] != -1;                                         /* 0x42a9d3..0x42a9f8: the flag of 0x42a840 */
    if (!all) {
        memset(r->list_sec, 0, g->nsectors); memset(r->list_grp, 0, g->ngroups);
        for (uint32_t k = 0; k < L->npairs; k++) {
            uint32_t s = L->pairs[2 * k], q = L->pairs[2 * k + 1];
            if (s < g->nsectors) r->list_sec[s] = 1;                            /* 0x42ab98: sector stamp +4, then 0x42a840 on its chain */
            if (!racing && q < g->ngroups) r->list_grp[q] = 1;                  /* 0x42aab8..0x42aacd: group stamp +0 */
        }
        if (racing && cg >= 0) {                                                /* 0x42aadf: the camera's group and the entry after it */
            int k = 0; while (k < 5 && race[k] != -1 && race[k] != cg) k++;
            if (k < 5 && race[k] == cg) { if ((uint32_t)cg < g->ngroups) r->list_grp[cg] = 1; if (k + 1 < 6 && race[k + 1] >= 0 && (uint32_t)race[k + 1] < g->ngroups) r->list_grp[race[k + 1]] = 1; }
            else if ((uint32_t)cg < g->ngroups) r->list_grp[cg] = 1;           /* port: the original searches on past the -1 (no bound, 0x42aaf3) */
        }
    }
    r->frame++;                                                                 /* [[0x509adc]]: the clocks of this frame */
    g_clock_r = r; gel_col_clock = query_clock;                                 /* the queries of this frame clock what they test */
    if (log >= 5 && !r->chain_ok) for (uint32_t mi = 0; mi < ins->nmodels; mi++) for (uint32_t k = 0; k < ins->models[mi].ninstances; k++) {
        /* WOODY_VISLOG=5, once per level: every instance without a floor group under its .ins position (the load cell) or under its
         * animated root (the cell of every later clock run), with what it is: mesh polygons 0 = never drawn anyway (volume boxes) */
        Instance *in = &ins->models[mi].instances[k]; Vec3 c = ins_anim_centre(in);
        int gp = gel_floor_group(g, in->position), gc = gel_floor_group(g, c), sp = gel_sector(g, in->position), sc = gel_sector(g, c);
        if (gp >= 0 && gc >= 0) continue;
        uint32_t np = 0, ncol = 0, npol = 0; ins_collision_nodes(in->model, &np);
        for (uint32_t cl = 0; cl < g->ncells; cl++) { uint32_t n = 0; const uint32_t *rf = gel_col_cell(g, cl, &n); for (uint32_t j = 0; j < n; j++) if ((rf[j] & 0xffff) == in->index) ncol++; }
        for (uint32_t q = 0; q < in->model->nnodes; q++) if (in->model->nodes[q].kind == 0) npol += in->model->nodes[q].npolys;
        printf("VIS load: inst %u model %u type %d shown %d | position %.0f %.0f %.0f sector %d group %d | root %.0f %.0f %.0f sector %d group %d | mesh polys %u, press nodes %u, .col refs %u, flag20 %d",
               in->index, mi, in->type, in->visible, in->position.x, in->position.y, in->position.z, sp, gp, c.x, c.y, c.z, sc, gc, npol, np, ncol, inst_flag20(in)), puts("");
    }
    chains_sync(r);
    /* message 34 (0x42aa0b): the linked instances of a volume that holds the camera get this frame's stamp first */
    for (uint32_t mi = 0; mi < ins->nmodels; mi++) for (uint32_t k = 0; k < ins->models[mi].ninstances; k++) ins->models[mi].instances[k].listed = ins->models[mi].instances[k].in_zone = 0;
    for (uint32_t i = 0; i < r->nlinks; i++) {
        const Instance *v = r->links[2 * i]; int in = link_inside(v, cam->pos);
        for (; i < r->nlinks && r->links[2 * i] == v; i++) if (in) r->links[2 * i + 1]->listed = -1;
        i--;
    }
    /* ... and that loop first runs the clock (vtbl[2](1)) of every type-1 object of the camera's kd leaf, in .col order
     * (0x42aa0b..0x42aa2e): each one that is in the world and not flag 0x20 goes in front of its sector's chain */
    {
        int32_t leaf = gel_cell(g, cam->pos); uint32_t nref = 0;
        const uint32_t *refs = leaf >= 0 ? gel_col_cell(g, (uint32_t)leaf, &nref) : NULL;
        for (uint32_t j = 0; j < nref; j++) {
            uint32_t idx = refs[j] & 0xffff; Instance *in = idx < ins->nslots ? ins->slots[idx] : NULL;
            if (in && in->visible && in->chain_sec1) chain_clock(r, in, 0);    /* 0x42e2c3: out of the world = no clock */
        }
    }
    /* the side planes of 0x437b00, from the same camera the renderer uses */
    float aspect = view_aspect(w, cam);
    float tv =tanf(cam->fov_deg * 3.14159265f / 360.0f), th = tv * aspect;
    Vec3 fw = cam_forward(cam), rt = cam_right(cam), up = { rt.y * fw.z - rt.z * fw.y, rt.z * fw.x - rt.x * fw.z, rt.x * fw.y - rt.y * fw.x };
    uint32_t n_sec = 0, n_grp = 0, n_link = 0, n_frus = 0, n_far = 0, n_act = 0, n_nofloor = 0, n_vis = 0, n_seen = 0;
    for (uint32_t mi = 0; mi < ins->nmodels; mi++) for (uint32_t k = 0; k < ins->models[mi].ninstances; k++) {
        const Instance *in = &ins->models[mi].instances[k];
        if (in->visible) { n_vis++; if (in->cell_grp < 0) n_nofloor++; if (in->listed < 0) n_link++; }
    }
    /* 0x42ab60: the sectors of the pairs, each once, in the order of the camera's .vis entry; 0x42a840 walks the chain of
     * each (the next pointer is read before the instance is handled, 0x42a85b) and every listed instance runs its clock
     * (vtbl[2](0x81)), whose re-cell puts it in front of its chain again unless the pose cache hits (chain_clock) - so the
     * animated instances of a chain come out reversed every frame and alternate between two orders, while stationary ones
     * keep their place (verified live, MODEL_RENDER.md 9.1).
     * No .vis (port only): every chain, in sector order. */
    uint32_t npass = all ? r->nchain : L->npairs;
    for (uint32_t pk = 0; pk < npass; pk++) {
        uint32_t s = all ? pk : L->pairs[2 * pk];
        if (s >= r->nchain) continue;
        if (!all) { if (r->list_sec[s] == 2) continue; r->list_sec[s] = 2; }        /* the sector stamp +4 (0x42ab8b) */
        for (Instance *in = r->chain[s], *next; in; in = next) {
            next = in->cell_next; n_seen++;
            if (in->listed < 0) { in->listed = 0; continue; }                        /* message 34: stamped (0x42a92f) */
            if (in->listed > 0) continue;                                            /* re-celled into a sector walked later: already in */
            if (!all && (in->cell_grp < 0 || (uint32_t)in->cell_grp >= g->ngroups || !r->list_grp[in->cell_grp])) { n_grp++; continue; }   /* 0x42a85e: +0x18 == -1 is never listed */
            in->in_zone = 1;
            Vec3 c; float rad;
            if (sphere_cached(in) && inst_sphere(in, &c, &rad)) {                   /* 0x42a891 */
                Vec3 d = { c.x - cam->pos.x, c.y - cam->pos.y, c.z - cam->pos.z };
                float x = d.x * rt.x + d.y * rt.y + d.z * rt.z, y = d.x * up.x + d.y * up.y + d.z * up.z, z = d.x * fw.x + d.y * fw.y + d.z * fw.z, mg = rad * 1.4142f;
                if (fabsf(x) > z * th + mg || fabsf(y) > z * tv + mg) { n_frus++; continue; }
                if (racing && d.x * d.x + d.y * d.y + d.z * d.z + rad * rad > 1.21e8f) { n_far++; continue; }   /* 0x4aa2f4 */
            }
            if (r->nlist >= r->list_cap) { uint32_t cap = r->list_cap ? r->list_cap * 2 : 256; Instance **nl = (Instance **)realloc(r->list, cap * sizeof *nl); if (!nl) continue; r->list = nl; r->list_cap = cap; }
            r->list[r->nlist++] = in; in->listed = 1;                               /* 0x42a931..0x42a948 */
            if (in->type >= 4 && in->type <= 16) n_act++;
            chain_clock(r, in, 1);                                                  /* vtbl[2](0x81) (not for flag 0x20, 0x42a94e) -> 0x43eee0 -> 0x4077f0 */
        }
    }
    if (all) {                                                                      /* port: instances in no sector are listed too without a .vis */
        for (uint32_t mi = 0; mi < ins->nmodels; mi++) for (uint32_t k = 0; k < ins->models[mi].ninstances; k++) {
            Instance *in = &ins->models[mi].instances[k];
            if (!in->visible || in->chain_sec1 || in->listed) continue;
            if (in->listed < 0) { in->listed = 0; continue; }
            if (r->nlist >= r->list_cap) { uint32_t cap = r->list_cap ? r->list_cap * 2 : 256; Instance **nl = (Instance **)realloc(r->list, cap * sizeof *nl); if (!nl) continue; r->list = nl; r->list_cap = cap; }
            in->in_zone = 1; r->list[r->nlist++] = in; in->listed = 1;
        }
    }
    for (uint32_t mi = 0; mi < ins->nmodels; mi++) for (uint32_t k = 0; k < ins->models[mi].ninstances; k++) { Instance *in = &ins->models[mi].instances[k]; if (in->listed < 0) in->listed = 0; }
    gel_col_dynamic(g, r->list, r->nlist);                                      /* 0x401c68: the collision queries' dynamic list 0x4c3bb4 from this list */
    n_sec = n_vis > n_seen ? n_vis - n_seen : 0;
    if (log >= 4) { printf("VIS frame %u order:", r->frame); for (uint32_t i = 0; i < r->nlist; i++) printf(" %u", r->list[i]->index); puts(""); }   /* every frame: the list order (tools/wverify.py --probe list prints the original's) */
    if (log) {
        static double next; static uint32_t mx; double t = win_time(); if (r->nlist > mx) mx = r->nlist;
        if (r->nlist > 256) { static int warned; if (!warned) { warned = 1; printf("VIS: %u instances listed - the original's list holds 256 (0x42a4f9)", r->nlist), puts(""); } }
        if (t >= next) {
            next = t + 1.0;
            printf("VIS cam %.0f %.0f %.0f sector %d group %d entry %d%s pairs %u%s: listed %u (enemies %u, max %u) | out: sector %u group %u (no floor %u) link %u frustum %u far %u",
                   cam->pos.x, cam->pos.y, cam->pos.z, cs, cg, ent, ent >= 0 && L && (int32_t)L->id != cg ? " (no id match: first)" : "", L ? L->npairs : 0, all ? " (all)" : racing ? " (race)" : "",
                   r->nlist, n_act, mx, n_sec, n_grp, n_nofloor, n_link, n_frus, n_far), puts("");
            if (log >= 3) for (uint32_t mi = 0; mi < ins->nmodels; mi++) for (uint32_t k = 0; k < ins->models[mi].ninstances; k++) {
                const Instance *in = &ins->models[mi].instances[k];
                if (in->visible && in->cell_grp < 0) printf("  VIS no floor: inst %u type %d at %.0f %.0f %.0f sector %d (position %.0f %.0f %.0f: group %d)", in->index, in->type, in->cell_ref.x, in->cell_ref.y, in->cell_ref.z, in->cell_sec, in->position.x, in->position.y, in->position.z, gel_floor_group(g, in->position)), puts("");
            }
            if (log >= 2) {
                printf("  VIS ids:"); for (uint32_t i = 0; i < r->nlist; i++) printf(" %u", r->list[i]->index); puts("");
                for (uint32_t mi = 0; mi < ins->nmodels; mi++) for (uint32_t k = 0; k < ins->models[mi].ninstances; k++) {   /* the actors, listed or not */
                    const Instance *in = &ins->models[mi].instances[k];
                    if (in->type >= 4 && in->type <= 16 && in->visible) printf("  VIS actor %u type %d %s at %.0f %.0f %.0f sector %d group %d (drawn last frame %d)", in->index, in->type, in->listed ? "listed" : "out", in->position.x, in->position.y, in->position.z, in->cell_sec, in->cell_grp, in->drawn), puts("");
                }
            }
            mx = 0;
        }
    }
}

/* ---- dynamic point lights (0x498790, docs/LIGHTING.md 7). The original registers them into a 16-slot table
 * (lightsys+0x14/+0x18, 0x2c bytes a record) that nothing ever frees and nothing ever draws: the one reader (0x42f05c)
 * collects the lights overlapping a model into a list that the model renderer never looks at, and after the first 16
 * registrations of a level every further call fails. So by default the port only keeps the table, exactly as
 * invisible as in the original. WOODY_DYNLIGHT=1 draws them the way the engine's own static-light code would have
 * (an extra light term C/255 * max(0, 1 - |P - L|/R) on world faces and models) - a port extra, not the original.
 * The port's table is emptied after every drawn frame, so a caller registers its light every frame (as all seven
 * callers do). */
typedef struct { Vec3 pos; float rgb[3], radius; int kind; } DynLight;
static DynLight g_dyn[16]; static int g_ndyn, g_dyn_draw = -1;
static int dyn_draw(void) { if (g_dyn_draw < 0) { const char *e = wenv("WOODY_DYNLIGHT"); g_dyn_draw = e && atoi(e) > 0; } return g_dyn_draw; }
int rnd_light_add(int kind, Vec3 pos, const float rgb[3], float radius)
{
    if (g_ndyn >= 16 || !(radius > 0)) return 0;                    /* 0x4987bc: table full -> 0; a radius <= 0 lights nothing */
    DynLight *d = &g_dyn[g_ndyn++]; d->pos = pos; d->radius = radius; d->kind = kind;
    for (int q = 0; q < 3; q++) d->rgb[q] = rgb[q];
    if (wenv("WOODY_DYNLOG")) printf("  DYNLIGHT %d kind %d at %.0f %.0f %.0f rgb %.0f %.0f %.0f r %.1f", g_ndyn - 1, kind, pos.x, pos.y, pos.z, rgb[0], rgb[1], rgb[2], radius), puts("");
    return 1;
}
/* per drawn instance: the dynamic lights in range of its reference point, as light vectors scaled by the linear
 * falloff, like the static Ldir of 0x43b912 without the 0.85 smoothing */
static int g_idyn_n; static float g_idyn_dir[16][3], g_idyn_col[16][3];
static void instance_dyn(const Renderer *r, const Instance *inst)
{
    g_idyn_n = 0; if (!g_ndyn || !r->lit || !r->show_light || !dyn_draw()) return;
    Vec3 p = ins_anim_centre(inst); p.y += 20.0f;
    for (int i = 0; i < g_ndyn; i++) {
        const DynLight *L = &g_dyn[i]; float dx = L->pos.x - p.x, dy = L->pos.y - p.y, dz = L->pos.z - p.z, d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d >= L->radius) continue;
        float k = (1.0f - d / L->radius) / (d > 1e-3f ? d : 1.0f);
        g_idyn_dir[g_idyn_n][0] = dx * k; g_idyn_dir[g_idyn_n][1] = dy * k; g_idyn_dir[g_idyn_n][2] = dz * k;
        for (int q = 0; q < 3; q++) g_idyn_col[g_idyn_n][q] = L->rgb[q];
        g_idyn_n++;
    }
}

/* vertex colour: vcol * 0.3 + max(0, N.Ldir) * C, clamped at 255 (0x43be20) and drawn MODULATE2X: 0..2 x texture (bt_flush).
 * Except on a blended face: 0x43d91d tests the flag 0x43d7cf raises for polygon flags 0x20/0x40 (group flag bit 1,
 * copied into the polygon at load by 0x428020) and jumps straight past the lit RGB at v+0x24..0x2c. It writes
 * 0x00iiiiii with i = (int)(alpha * 0.5) (0x43d9a4) and alpha = (1 - inst->fade) * 255 (0x43b504), so i = 128 for an
 * instance that is not fading, and under MODULATE2X that is plain 1.0 x texture. A neon sign is therefore never dimmed
 * by the world light or by the angle its own plate makes with it - which is what "glowing" means here. */
/* render colour hook vtbl[26] with [0x5ac850] = 1 (multiply) or 2 (add), clamped at 255 (0x43bdd3..0x43be20). c is the lit colour
 * in units of 255 times the material base, so an added 128 is 128/255 of the base: the bomb's red flash (docs/BOMB.md 3.4) */
static void tint_apply(const Instance *inst, float *c, const float base[3])
{
    if (inst->tint_mode == 1) for (int q = 0; q < 3; q++) c[q] *= inst->tint_rgb[q];
    else if (inst->tint_mode == 2) for (int q = 0; q < 3; q++) { c[q] += inst->tint_rgb[q] * base[q]; if (c[q] > base[q]) c[q] = base[q]; }
}
static void lit_vertex_colour(const Renderer *r, const Instance *inst, const Mat4 *M, int ni, const InsPoint *pt, const float base[3])
{
    if (g_mat_blended) { float a = 1.0f - inst->fade; bt_color(base[0] * a, base[1] * a, base[2] * a); return; }
    float ts = inst->tint_scale > 0 ? inst->tint_scale : 1.0f;   /* 0x451a40: [0x5ac850] = 1, [0x5ac854..5c] = 0.1 times the lit colour (0x43bdfc) */
    if (!r->lit || !r->show_light) { float c[3] = { ts * base[0] * pt->colour.x / 128.0f, ts * base[1] * pt->colour.y / 128.0f, ts * base[2] * pt->colour.z / 128.0f }; tint_apply(inst, c, base); bt_color(c[0], c[1], c[2]); return; }
    const float *a = M->m; Vec3 n = pt->normal;
    Vec3 w = { a[0] * n.x + a[4] * n.y + a[8] * n.z, a[1] * n.x + a[5] * n.y + a[9] * n.z, a[2] * n.x + a[6] * n.y + a[10] * n.z };
    const Model *m = inst->model; if (ni < 0 && m->nmesh_nodes) ni = (int)m->mesh_nodes[0] - 1;   /* a point no part owns: the first part's light */
    const float *pl = ni >= 0 && (uint32_t)ni < m->nnodes && inst->plight ? &inst->plight[ni * 6] : NULL;
    float l = sqrtf(w.x * w.x + w.y * w.y + w.z * w.z), ndl = pl ? n.x * pl[0] + n.y * pl[1] + n.z * pl[2] : 0; if (ndl < 0) ndl = 0;   /* the rest normal P+0x10 against the part's own vector (0x43bce4) */
    float vc[3] = { pt->colour.x, pt->colour.y, pt->colour.z }, c[3], dyn[3] = { 0, 0, 0 };
    for (int i = 0; i < g_idyn_n && l > 1e-6f; i++) {                            /* WOODY_DYNLIGHT: the same N.L term per dynamic light */
        float nd = (w.x * g_idyn_dir[i][0] + w.y * g_idyn_dir[i][1] + w.z * g_idyn_dir[i][2]) / l;
        if (nd > 0) for (int q = 0; q < 3; q++) dyn[q] += 2.0f * nd * g_idyn_col[i][q];
    }
    for (int q = 0; q < 3; q++) { c[q] = (vc[q] * 0.6f + (pl ? 2.0f * ndl * pl[3 + q] : 0) + dyn[q]) / 255.0f; if (c[q] > 2) c[q] = 2; c[q] *= base[q] * ts; if (q && inst->tint_red) c[q] = 0; }
    tint_apply(inst, c, base); bt_color(c[0], c[1], c[2]);
}

/* ---- cast shadows (0x42e651-0x42ec3a, drawn by 0x4385f0, or 0x4388e0 while the caster fades): the caster's geometry
 * projected from its light onto the receiving faces, as opaque ambient-coloured polygons between the light pass and the
 * texture pass, so the shadow looks like an unlit face. The original clips against the faces on the CPU; here the stencil buffer does it. */
static float *g_sh; static uint32_t g_sh_n, g_sh_cap;                       /* caster triangles, world space */
static void sh_push(Vec3 a, Vec3 b, Vec3 c)
{
    if (g_sh_n + 1 > g_sh_cap) { g_sh_cap = g_sh_cap * 2 + 1024; g_sh = (float *)realloc(g_sh, (size_t)g_sh_cap * 9 * sizeof(float)); }
    float *o = &g_sh[(size_t)g_sh_n * 9]; o[0] = a.x; o[1] = a.y; o[2] = a.z; o[3] = b.x; o[4] = b.y; o[5] = b.z; o[6] = c.x; o[7] = c.y; o[8] = c.z; g_sh_n++;
}
/* Per caster, the cone from the light around each caster triangle (unit axis, cos and sin of its half angle; cos = -2:
 * never culled) and around each chunk of SH_CHUNK consecutive triangles, and per receiving face the cone around its
 * bounding sphere. A triangle whose cone misses the face's cannot project onto any point of the face, so it is neither
 * projected nor drawn: the stencil would have rejected every one of its pixels. A cone only bounds the spherical
 * triangle while it is narrower than a half sphere, so wider ones are never culled. A speed-up of the port only; the
 * pixels are the same. */
#define SH_CHUNK 16
static float *g_shc, *g_shk; static uint32_t g_shc_cap;
static void sh_cone_close(float *o, float cx, float cy, float cz, const float (*u)[3], uint32_t nu)
{
    float cl = sqrtf(cx * cx + cy * cy + cz * cz); if (cl < 1e-3f) { o[3] = -2.0f; return; }
    cx /= cl; cy /= cl; cz /= cl;
    float ct = 1.0f; for (uint32_t v = 0; v < nu; v++) { float d = cx * u[v][0] + cy * u[v][1] + cz * u[v][2]; if (d < ct) ct = d; }
    ct -= 1e-4f;                                                                  /* a hair wider against rounding */
    if (ct < 0.05f) { o[3] = -2.0f; return; }
    o[0] = cx; o[1] = cy; o[2] = cz; o[3] = ct; o[4] = sqrtf(1.0f - ct * ct);
}
static void sh_cones(const LitLight *L)
{
    uint32_t nk = (g_sh_n + SH_CHUNK - 1) / SH_CHUNK;
    if (g_shc_cap < g_sh_n) { g_shc_cap = g_sh_n + 1024; g_shc = (float *)realloc(g_shc, (size_t)g_shc_cap * 5 * sizeof(float)); g_shk = (float *)realloc(g_shk, (size_t)(g_shc_cap / SH_CHUNK + 1) * 5 * sizeof(float)); }
    static float u[SH_CHUNK * 3][3];
    for (uint32_t k = 0; k < nk; k++) {
        uint32_t t0 = k * SH_CHUNK, t1 = t0 + SH_CHUNK < g_sh_n ? t0 + SH_CHUNK : g_sh_n, nu = 0; float kx = 0, ky = 0, kz = 0; int kbad = 0;
        for (uint32_t t = t0; t < t1; t++) {
            const float *tv = &g_sh[(size_t)t * 9]; float *o = &g_shc[(size_t)t * 5], cx = 0, cy = 0, cz = 0; int bad = 0;
            for (int v = 0; v < 3; v++) {
                float ex = tv[v * 3] - L->pos.x, ey = tv[v * 3 + 1] - L->pos.y, ez = tv[v * 3 + 2] - L->pos.z, l = sqrtf(ex * ex + ey * ey + ez * ez);
                if (l < 1e-3f) { bad = 1; break; }
                float *w = u[nu + v]; w[0] = ex / l; w[1] = ey / l; w[2] = ez / l; cx += w[0]; cy += w[1]; cz += w[2];
            }
            if (bad) { o[3] = -2.0f; kbad = 1; continue; }
            sh_cone_close(o, cx, cy, cz, (const float (*)[3])&u[nu], 3);
            kx += cx; ky += cy; kz += cz; nu += 3;
        }
        float *ko = &g_shk[(size_t)k * 5];
        if (kbad) ko[3] = -2.0f; else sh_cone_close(ko, kx, ky, kz, (const float (*)[3])u, nu);
    }
}
static int sh_cone_miss(const float *cn, const float *fd, float cf, float sf)       /* the two caps are disjoint */
{
    return cn[3] > -2.0f && cf >= -cn[3]                                              /* the half angles add up to less than pi */
        && cn[0] * fd[0] + cn[1] * fd[1] + cn[2] * fd[2] < cn[3] * cf - cn[4] * sf;   /* angle between the axes > their sum */
}
/* the caster triangles g_sh projected from the light onto one plane (dl = the light's distance to it); triangles with a
 * vertex on the wrong side are dropped, and with fb (centre + radius of the receiving face) those whose cone misses it.
 * Returns the number written to out (9 floats each). */
static uint32_t sh_project(const LitLight *L, const float *pl, float dl, const float *fb, float *out)
{
    float fd[3] = { 0, 0, 0 }, cf = -2.0f, sf = 0;                                    /* cf = -2: no culling */
    if (fb) {
        float fx = fb[0] - L->pos.x, fy = fb[1] - L->pos.y, fz = fb[2] - L->pos.z, D = sqrtf(fx * fx + fy * fy + fz * fz), rr = fb[3] * 1.02f + 1.0f;
        if (D > rr) { fd[0] = fx / D; fd[1] = fy / D; fd[2] = fz / D; sf = rr / D; cf = sqrtf(1.0f - sf * sf); }
    }
    uint32_t np = 0;
    for (uint32_t t = 0; t < g_sh_n; t++) {
        if (cf > -2.0f) {
            if (t % SH_CHUNK == 0 && sh_cone_miss(&g_shk[(size_t)(t / SH_CHUNK) * 5], fd, cf, sf)) { t += SH_CHUNK - 1; continue; }
            if (sh_cone_miss(&g_shc[(size_t)t * 5], fd, cf, sf)) continue;
        }
        const float *tv = &g_sh[(size_t)t * 9]; float *o = &out[(size_t)np * 9]; int ok = 1;
        for (int v = 0; v < 3 && ok; v++) {
            float ex = tv[v * 3] - L->pos.x, ey = tv[v * 3 + 1] - L->pos.y, ez = tv[v * 3 + 2] - L->pos.z, nd = pl[0] * ex + pl[1] * ey + pl[2] * ez;
            if (nd > -1e-3f) { ok = 0; break; }
            float k = -dl / nd; if (k < 1.0f || k > 100.0f) { ok = 0; break; }   /* k < 1: the vertex is behind the plane */
            o[v * 3] = L->pos.x + ex * k; o[v * 3 + 1] = L->pos.y + ey * k; o[v * 3 + 2] = L->pos.z + ez * k;
        }
        if (ok) np++;
    }
    return np;
}
/* receiving faces as triangles (fan indices into gel->verts), for the stencil marks */
static void sh_faces(const Renderer *r, const uint32_t *idx, uint32_t n)
{
    glVertexPointer(3, GL_FLOAT, sizeof(GelVert), r->gel->verts);
    glDrawElements(GL_TRIANGLES, (GLsizei)n, GL_UNSIGNED_INT, idx);
}
/* an opaque caster's receiving faces, up to 255 at a time, each with its own stencil value: face i is marked with i + 1 and
 * its projection drawn where the stencil holds i + 1, face after face, and only then are all marks cleared with one draw.
 * Marking face by face keeps the per-face result exactly (a pixel two faces share gets the shadow of either), while the
 * per-face clear of the old loop - and a third of its GL calls - goes. */
#define SH_BATCH 255
typedef struct { uint32_t face, off, n; } ShQ;
static void sh_flush(const Renderer *r, const float *proj, const ShQ *q, uint32_t nq)
{
    if (!nq) return;
    static uint32_t *idx; static uint32_t icap; uint32_t ni = 0;
    for (uint32_t i = 0; i < nq; i++) {
        uint32_t f = q[i].face, k = r->face_fan[f + 1] - r->face_fan[f];
        glEnable(GL_DEPTH_TEST); glColorMask(0, 0, 0, 0); glStencilFunc(GL_ALWAYS, (GLint)(i + 1), 0xff); glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        glVertexPointer(3, GL_FLOAT, sizeof(GelVert), r->gel->verts);
        glDrawElements(GL_TRIANGLES, (GLsizei)k, GL_UNSIGNED_INT, &r->fan_idx[r->face_fan[f]]);   /* stencil = i + 1 on the visible part of face i */
        glColorMask(1, 1, 1, 1); glDisable(GL_DEPTH_TEST); glStencilFunc(GL_EQUAL, (GLint)(i + 1), 0xff); glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glVertexPointer(3, GL_FLOAT, 0, proj);
        glDrawArrays(GL_TRIANGLES, (GLint)(q[i].off * 3), (GLsizei)(q[i].n * 3));
        if (ni + k > icap) { icap = (ni + k) * 2 + 1024; idx = (uint32_t *)realloc(idx, icap * sizeof *idx); }
        memcpy(&idx[ni], &r->fan_idx[r->face_fan[f]], k * sizeof *idx); ni += k;
    }
    glEnable(GL_DEPTH_TEST); glColorMask(0, 0, 0, 0); glStencilFunc(GL_ALWAYS, 0, 0xff); glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glVertexPointer(3, GL_FLOAT, sizeof(GelVert), r->gel->verts);
    glDrawElements(GL_TRIANGLES, (GLsizei)ni, GL_UNSIGNED_INT, idx);
    glColorMask(1, 1, 1, 1);
}
/* the receiving faces of one light: lists A then B in their order, without the faces that can never show a shadow (no
 * polygon, never drawn, the light not in front of them), with what the caster tests read packed per face: plane,
 * bounding sphere, the light's distance. Built on first use per light; whether a face is drawn this frame is the
 * caller's test. */
typedef struct { int built; uint32_t n; uint32_t *f; float *d; } ShRecv;
static ShRecv *g_rcv; static uint32_t g_nrcv;
static const ShRecv *sh_receivers(const Renderer *r, int li)
{
    if ((uint32_t)li >= g_nrcv) { uint32_t n = r->lit->nlights > (uint32_t)li ? r->lit->nlights : (uint32_t)li + 1; g_rcv = (ShRecv *)realloc(g_rcv, n * sizeof *g_rcv); memset(&g_rcv[g_nrcv], 0, (n - g_nrcv) * sizeof *g_rcv); g_nrcv = n; }
    ShRecv *c = &g_rcv[li]; if (c->built) return c;
    const LitLight *L = &r->lit->lights[li]; c->built = 1; c->n = 0;
    c->f = (uint32_t *)malloc((L->na + L->nb + 1) * sizeof *c->f); c->d = (float *)malloc((size_t)(L->na + L->nb + 1) * 9 * sizeof *c->d);
    for (int list = 0; list < 2; list++) {
        const uint32_t *faces = list ? L->b : L->a; uint32_t nf = list ? L->nb : L->na;
        for (uint32_t fi = 0; fi < nf; fi++) {
            uint32_t f = faces[fi]; if (f >= r->gel->npolys) continue;
            if (r->gel->polys[f].nverts < 3 || gel_face_invisible(r->gel, r->tex, f)) continue;                   /* nor a shadow on one */
            const float *pl = r->gel->polys[f].plane, dl = pl[0] * L->pos.x + pl[1] * L->pos.y + pl[2] * L->pos.z + pl[3];
            if (dl <= 1.0f) continue;                                                                              /* the light behind the plane */
            float *d = &c->d[(size_t)c->n * 9]; memcpy(d, pl, 4 * sizeof *d); memcpy(d + 4, &r->face_bound[4 * f], 4 * sizeof *d); d[8] = dl;
            c->f[c->n++] = f;
        }
    }
    return c;
}
/* The caster's triangles in world space (g_sh) and their bounding sphere */
static void sh_caster(const Renderer *r, Instance *inst, float c[3], float *rad)
{
    Model *m = inst->model; const int32_t *own = model_owner(m);
    g_sh_n = 0;
    for (uint32_t ni = 0; ni < m->nnodes; ni++) {
        InsNode *n = &m->nodes[ni]; if (n->kind != 0 || !n->polys || n->type_code == 2) continue;
        for (uint32_t k = 0; k < n->npolys; k++) {
            InsPoly *p = &n->polys[k]; if (p->nverts < 3 || mat_blended(r, p->material)) continue;
            Vec3 w[3];
            for (uint32_t c = 0; c < p->nverts; c++) {
                InsPoint *pt = &m->points[p->indices[c]]; Vec3 lp = { pt->pos.x - n->pivot.x, pt->pos.y - n->pivot.y, pt->pos.z - n->pivot.z };
                Vec3 q = mat4_apply(&inst->node_world[ni], lp);
                if (c == 0) w[0] = q; else { w[1] = w[2]; w[2] = q; if (c >= 2) sh_push(w[0], w[1], w[2]); }
            }
        }
    }
    for (uint32_t t = 0; t < m->ntris; t++) {
        InsTri *tr = &m->tris[t]; if (mat_blended(r, tr->material)) continue;
        uint32_t idx[3] = { tr->i0, tr->i1, tr->i2 }; Vec3 w[3];
        for (int c = 0; c < 3; c++) { int o = own[idx[c]]; Vec3 lp = m->points[idx[c]].pos; if (o >= 0) { lp.x -= m->nodes[o].pivot.x; lp.y -= m->nodes[o].pivot.y; lp.z -= m->nodes[o].pivot.z; } w[c] = mat4_apply(o >= 0 ? &inst->node_world[o] : &inst->world, lp); }
        sh_push(w[0], w[1], w[2]);
    }
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < g_sh_n * 3; i++) for (int q = 0; q < 3; q++) { float v = g_sh[i * 3 + q]; if (v < lo[q]) lo[q] = v; if (v > hi[q]) hi[q] = v; }
    for (int q = 0; q < 3; q++) c[q] = (lo[q] + hi[q]) * 0.5f;
    *rad = 0.5f * sqrtf((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));
}
/* can the caster (centre c, radius rad) shade the receiving face d (sh_receivers record) at all? n counts the tests passed */
static int sh_face_test(const LitLight *L, const float *d, const float c[3], float rad, int n[3])
{
    const float *pl = d, *fb = d + 4, dl = d[8], dc = pl[0] * c[0] + pl[1] * c[1] + pl[2] * c[2] + pl[3];
    if (dc >= dl || dc < -rad) return 0;                                       /* caster must be between the light and the plane */
    n[0]++;
    float den = dl - dc; if (den < 1.0f) return 0;
    float s = dl / den; if (s > 40.0f) return 0;                              /* projection scale; huge = grazing */
    n[1]++;
    float pc[3] = { L->pos.x + (c[0] - L->pos.x) * s, L->pos.y + (c[1] - L->pos.y) * s, L->pos.z + (c[2] - L->pos.z) * s };
    float dx = pc[0] - fb[0], dy = pc[1] - fb[1], dz = pc[2] - fb[2], reach = rad * s * 1.5f + fb[3];
    if (dx * dx + dy * dy + dz * dz > reach * reach) return 0;
    n[2]++;
    return 1;
}
/* A caster that has not moved since the previous frame (same light, same node and instance matrices: the world triangles
 * and so every projection are the same) keeps its result: per receiving face, drawn or not this frame, the projected
 * triangles. Built on the second frame in the same pose, so a moving caster never pays for it; each frame only the faces
 * drawn this frame are replayed, in the same order. A speed-up of the port only. */
typedef struct {
    int light, built; uint32_t nn; Mat4 *key;                                  /* key: node_world[nn] + world */
    uint32_t nf, fcap, *f, *off, *cnt; float *proj; size_t pn, pcap;           /* faces, their triangle ranges in proj */
    uint32_t sh_n; int st[3];                                                  /* WOODY_SHLOG */
} ShCache;
static ShCache *g_shcache; static uint32_t g_nshcache, g_shcache_cap, g_sh_gen = 1;
static void sh_reset(void)                                                     /* a level is unloaded: every list belongs to it */
{
    for (uint32_t i = 0; i < g_nrcv; i++) { free(g_rcv[i].f); free(g_rcv[i].d); }
    free(g_rcv); g_rcv = NULL; g_nrcv = 0;
    for (uint32_t i = 0; i < g_nshcache; i++) { ShCache *c = &g_shcache[i]; free(c->key); free(c->f); free(c->off); free(c->cnt); free(c->proj); }
    free(g_shcache); g_shcache = NULL; g_nshcache = g_shcache_cap = 0;
    if (++g_sh_gen == 0) g_sh_gen = 1;                                         /* every Instance.sh_slot is stale */
}
static ShCache *sh_cache_of(Instance *inst)
{
    if (inst->sh_gen != g_sh_gen || !inst->sh_slot) {
        if (g_nshcache == g_shcache_cap) { g_shcache_cap = g_shcache_cap * 2 + 32; g_shcache = (ShCache *)realloc(g_shcache, g_shcache_cap * sizeof *g_shcache); }
        memset(&g_shcache[g_nshcache], 0, sizeof *g_shcache); g_shcache[g_nshcache].light = -1;
        inst->sh_gen = g_sh_gen; inst->sh_slot = (int)++g_nshcache;
    }
    return &g_shcache[inst->sh_slot - 1];
}
static void sh_cache_build(const Renderer *r, Instance *inst, ShCache *cc)
{
    const LitLight *L = &r->lit->lights[inst->light]; float c[3], rad;
    cc->built = 1; cc->nf = 0; cc->pn = 0; cc->st[0] = cc->st[1] = cc->st[2] = 0;
    sh_caster(r, inst, c, &rad); cc->sh_n = g_sh_n;
    if (!g_sh_n) return;
    sh_cones(L);
    const ShRecv *rv = sh_receivers(r, inst->light);
    for (uint32_t fi = 0; fi < rv->n; fi++) {
        const float *d = &rv->d[(size_t)fi * 9];
        if (!sh_face_test(L, d, c, rad, cc->st)) continue;
        if (cc->pn + g_sh_n > cc->pcap) { cc->pcap = (cc->pn + g_sh_n) * 2; cc->proj = (float *)realloc(cc->proj, cc->pcap * 9 * sizeof(float)); }
        uint32_t np = sh_project(L, d, d[8], d + 4, cc->proj + cc->pn * 9);
        if (!np) continue;
        if (cc->nf == cc->fcap) { cc->fcap = cc->fcap * 2 + 64; cc->f = (uint32_t *)realloc(cc->f, cc->fcap * 4); cc->off = (uint32_t *)realloc(cc->off, cc->fcap * 4); cc->cnt = (uint32_t *)realloc(cc->cnt, cc->fcap * 4); }
        cc->f[cc->nf] = rv->f[fi]; cc->off[cc->nf] = (uint32_t)cc->pn; cc->cnt[cc->nf] = np; cc->nf++; cc->pn += np;
    }
}
static int g_shlog;                                                         /* WOODY_SHLOG=1: one line per second per instance that reaches the caster test */
/* 0x42e798: before anything is projected, the original takes the outline of the caster's bounding-box node (model S+0x24) as
 * seen from the light, 0x43aaa0: a polygon is "front" when the light, in the node's own space (0x440fc0), lies on its outer
 * side (plane . L + d > 0), and the outline is every edge of a front polygon whose neighbour is not front. Two edges or fewer
 * and the whole shadow is skipped (0x42e7a0 -> 0x42ecdd). With the light inside the box no polygon is front, so a caster
 * whose light is inside its own box casts nothing: W2B's boss lift (model 54, a box 400 wide and 2800 high with the chain)
 * comes down around its lamp, light 8, and the port threw its shadow from inside over the walls and the ceiling. */
static const float *poly_plane(Model *m, const InsNode *n, InsPoly *p);
static int sh_outline(const Instance *inst, const LitLight *L)
{
    Model *m = inst->model; uint32_t bn = m->bbox_node; Vec3 q;
    if (!bn || bn > m->nnodes || !inst->node_world) return 1;
    const InsNode *n = &m->nodes[bn - 1]; if (!n->npolys || n->npolys > 64 || !affine_inv_apply(&inst->node_world[bn - 1], L->pos, &q)) return 1;
    q.x -= n->pivot.x; q.y -= n->pivot.y; q.z -= n->pivot.z;
    uint8_t front[64]; int edges = 0;
    for (uint32_t k = 0; k < n->npolys; k++) { const float *pl = poly_plane(m, n, &n->polys[k]); front[k] = pl[0] * q.x + pl[1] * q.y + pl[2] * q.z + pl[3] > 0; }
    for (uint32_t k = 0; k < n->npolys; k++) {
        const InsPoly *p = &n->polys[k]; if (!front[k]) continue;
        for (uint32_t e = 0; e < p->nverts; e++) {                            /* the neighbour across edge a-b: the other polygon with both points */
            uint32_t a = p->indices[e], b = p->indices[(e + 1) % p->nverts]; int nb_front = 0;
            for (uint32_t j = 0; j < n->npolys && !nb_front; j++) {
                const InsPoly *o = &n->polys[j]; int ha = 0, hb = 0; if (j == k) continue;
                for (uint32_t v = 0; v < o->nverts; v++) { ha |= o->indices[v] == a; hb |= o->indices[v] == b; }
                if (ha && hb) nb_front = front[j];
            }
            if (!nb_front) edges++;
        }
    }
    return edges > 2;
}
static void cast_shadow(const Renderer *r, Instance *inst)
{
    int fading = inst->fade > 0.01f;                                        /* 0x42e69a/0x42eb7a: [0x4a94f8] = 0.01 -> 0x4388e0, else 0x4385f0 */
    Model *m = inst->model; const LitLight *L = &r->lit->lights[inst->light];
    if (!sh_outline(inst, L)) { if (g_shlog) printf("    light %d at %.0f %.0f %.0f inside the bounding box: no outline, no shadow (0x42e7a0)\n", inst->light, L->pos.x, L->pos.y, L->pos.z); return; }
    int st[3] = { 0, 0, 0 }, n_drawn = 0; uint32_t n_tris = 0;
    static ShQ q[SH_BATCH]; uint32_t nq = 0;
    if (!fading) {                                                          /* opaque: the per-pose cache */
        ShCache *cc = sh_cache_of(inst);
        size_t kn = m->nnodes + 1;
        int same = cc->light == inst->light && cc->nn == m->nnodes && cc->key && !memcmp(cc->key, inst->node_world, m->nnodes * sizeof(Mat4)) && !memcmp(&cc->key[m->nnodes], &inst->world, sizeof(Mat4));
        if (same) {
            if (!cc->built) sh_cache_build(r, inst, cc);
            for (uint32_t i = 0; i < cc->nf; i++) {
                uint32_t f = cc->f[i]; if (r->cull && r->face_stamp[f] != r->stamp_gen) continue;    /* a face not drawn this frame cannot show one */
                q[nq].face = f; q[nq].off = cc->off[i]; q[nq].n = cc->cnt[i]; nq++; n_drawn++; n_tris += cc->cnt[i];
                if (nq == SH_BATCH) { sh_flush(r, cc->proj, q, nq); nq = 0; }
            }
            sh_flush(r, cc->proj, q, nq);
            if (g_shlog) printf("    tris %u light %d at %.0f %.0f %.0f range %.0f faces A %u B %u -> plane %d scale %d reach %d drawn %d (%u tris, cached)", cc->sh_n, inst->light, L->pos.x, L->pos.y, L->pos.z, L->range, L->na, L->nb, cc->st[0], cc->st[1], cc->st[2], n_drawn, n_tris), puts("");
            return;
        }
        if (cc->nn != m->nnodes || !cc->key) { free(cc->key); cc->key = (Mat4 *)malloc(kn * sizeof(Mat4)); cc->nn = m->nnodes; }
        memcpy(cc->key, inst->node_world, m->nnodes * sizeof(Mat4)); cc->key[m->nnodes] = inst->world;
        cc->light = inst->light; cc->built = 0;                             /* moved: drawn the plain way below, cached if it holds still */
    }
    float c[3], rad; sh_caster(r, inst, c, &rad);
    if (!g_sh_n) return;
    static float *proj; static uint32_t proj_cap; if (proj_cap < g_sh_n) { proj_cap = g_sh_n + 1024; proj = (float *)realloc(proj, (size_t)proj_cap * 9 * sizeof(float)); }
    sh_cones(L);
    static float *bproj; static size_t bcap; uint32_t boff = 0;            /* the opaque batch */
    const ShRecv *rv = sh_receivers(r, inst->light);
    for (uint32_t fi = 0; fi < rv->n; fi++) {
        uint32_t f = rv->f[fi]; if (r->cull && r->face_stamp[f] != r->stamp_gen) continue;            /* a face not drawn this frame cannot show one */
        const float *pl = &rv->d[(size_t)fi * 9], *fb = pl + 4, dl = pl[8];
        if (!sh_face_test(L, pl, c, rad, st)) continue;
        if (!fading) {
            if ((size_t)(boff + g_sh_n) * 9 > bcap) { bcap = (size_t)(boff + g_sh_n) * 9 * 2; bproj = (float *)realloc(bproj, bcap * sizeof(float)); }
            uint32_t np = sh_project(L, pl, dl, fb, bproj + (size_t)boff * 9);
            if (!np) continue;
            q[nq].face = f; q[nq].off = boff; q[nq].n = np; nq++; boff += np; n_drawn++; n_tris += np;
            if (nq == SH_BATCH) { sh_flush(r, bproj, q, nq); nq = 0; boff = 0; }
            continue;
        }
        uint32_t np = sh_project(L, pl, dl, fb, proj);
        if (!np) continue;
        const GelPoly *gp = &r->gel->polys[f];
        /* stencil = 1 on the visible part of the receiving face */
        uint32_t nfi = r->face_fan[f + 1] - r->face_fan[f]; const uint32_t *fan = &r->fan_idx[r->face_fan[f]];
        glColorMask(0, 0, 0, 0); glStencilFunc(GL_ALWAYS, 1, 1); glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        sh_faces(r, fan, nfi);
        glColorMask(1, 1, 1, 1); glDisable(GL_DEPTH_TEST);
        glVertexPointer(3, GL_FLOAT, 0, proj);
        {
            /* 0x4388e0 (bucket 2: blend off, z-write off, SPECULAR on): out = light texture x diffuse + specular, with
             * specular = AMB ([0x5e86ac]+0x1b0, 0x438c75) and diffuse = (int)(C' * k) per channel (0x438b65..0x438bc5),
             * C' = the light's colour (light+0x30) x inst+0x6c (0x42e6b7..0x42e6e2), k = 1 - |n.L + d|/R of the receiving
             * plane (0x498830 at 0x42ebaf, 0 outside the sphere), texture [0x5e8678] + 0x74 (15 - round(k * 15.49))
             * (0x438ba9..0x438bf3), u/v = the two rows of the sphere projection applied to the projected points
             * (0x438a32..0x438a6f). So the shaded area gets back fade x this light's own contribution: at fade 0.01 a
             * full shadow, at 0.98 almost none. Port: two passes through the stencil, AMB (1 -> 2) and then the textured
             * term added ONE/ONE (2 -> 3), so overlapping caster triangles still write every pixel once, as the
             * original's blend-off overwrite does. */
            glStencilFunc(GL_EQUAL, 1, 3); glStencilOp(GL_KEEP, GL_KEEP, GL_INCR); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)np * 3);
            float R = L->range, kk = dl < R ? 1.0f - dl / R : 0.0f, col[3];
            for (int k = 0; k < 3; k++) col[k] = (float)(int)(L->colour[k] * inst->fade * kk) / 255.0f;
            const GelVert *v2 = &r->gel->verts[gp->indices[2]];                          /* 0x498890: U towards the third vertex */
            float F[3] = { L->pos.x - pl[0] * dl, L->pos.y - pl[1] * dl, L->pos.z - pl[2] * dl }, U[3] = { v2->x - F[0], v2->y - F[1], v2->z - F[2] };
            float ul = sqrtf(U[0] * U[0] + U[1] * U[1] + U[2] * U[2]);
            if (kk > 0 && ul > 1e-4f && (col[0] > 0 || col[1] > 0 || col[2] > 0)) {
                int ti = 15 - (int)(kk * 15.49f + 0.5f); if (ti < 0) ti = 0; if (ti > 15) ti = 15;
                float sc = 0.5f / sqrtf(R * R - dl * dl);
                for (int k = 0; k < 3; k++) U[k] /= ul;
                float W[3] = { pl[1] * U[2] - pl[2] * U[1], pl[2] * U[0] - pl[0] * U[2], pl[0] * U[1] - pl[1] * U[0] };
                static float *uv; static uint32_t uv_cap; if (uv_cap < np) { uv_cap = np + 1024; uv = (float *)realloc(uv, (size_t)uv_cap * 6 * sizeof(float)); }
                for (uint32_t i = 0; i < np * 3; i++) {
                    float d[3] = { proj[i * 3] - F[0], proj[i * 3 + 1] - F[1], proj[i * 3 + 2] - F[2] };
                    uv[i * 2] = 0.5f + sc * (d[0] * W[0] + d[1] * W[1] + d[2] * W[2]); uv[i * 2 + 1] = 0.5f + sc * (d[0] * U[0] + d[1] * U[1] + d[2] * U[2]);
                }
                glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, r->light_tex[ti]); glEnableClientState(GL_TEXTURE_COORD_ARRAY); glTexCoordPointer(2, GL_FLOAT, 0, uv);
                glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE); glColor3f(col[0], col[1], col[2]);
                glStencilFunc(GL_EQUAL, 2, 3); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)np * 3);
                glDisable(GL_BLEND); glDisable(GL_TEXTURE_2D); glDisableClientState(GL_TEXTURE_COORD_ARRAY); glColor3f(LIT_AMB, LIT_AMB, LIT_AMB);
            }
        }
        n_drawn++; n_tris += np;
        glEnable(GL_DEPTH_TEST); glColorMask(0, 0, 0, 0); glStencilFunc(GL_ALWAYS, 0, 1); glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        sh_faces(r, fan, nfi);
        glColorMask(1, 1, 1, 1);
    }
    sh_flush(r, bproj, q, nq);
    if (g_shlog) printf("    tris %u light %d at %.0f %.0f %.0f range %.0f faces A %u B %u -> plane %d scale %d reach %d drawn %d (%u tris)", g_sh_n, inst->light, L->pos.x, L->pos.y, L->pos.z, L->range, L->na, L->nb, st[0], st[1], st[2], n_drawn, n_tris), puts("");
}
static void draw_cast_shadows(const Renderer *r)
{
    { static int last = -1; int s = (int)g_tex_now; g_shlog = wenv("WOODY_SHLOG") && (s != last || atoi(wenv("WOODY_SHLOG")) == 2); if (g_shlog) last = s; }
    glDisable(GL_TEXTURE_2D); glDisable(GL_BLEND); glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnable(GL_STENCIL_TEST); glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-1.0f, -1.0f); glColor3f(LIT_AMB, LIT_AMB, LIT_AMB);
    /* bucket 2 (fading casters, 0x4388e0) is flushed before bucket 5 (opaque, 0x4385f0) in 0x4293f0 (0x42960e, 0x42966c):
     * where both overlap, the flat AMB of an opaque caster wins */
    for (int bucket = 0; bucket < 2; bucket++)
    for (uint32_t mi = 0; mi < r->ins->nmodels; mi++) {
        Model *m = &r->ins->models[mi];
        for (uint32_t k = 0; k < m->ninstances; k++) {
            Instance *inst = &m->instances[k];
            if ((inst->fade > 0.01f) != (bucket == 0)) continue;
            int caster = shadow_caster(inst);
            if (g_shlog && (caster || inst->type)) printf("  SH t %.1f model %u inst %u type %d setflags %x caster %d vis %d fade %.2f l_seen %d light %d nodes %d at %.0f %.0f %.0f", g_tex_now, mi, k, inst->type, inst->setflags, caster, inst->visible, inst->fade, inst->l_seen, inst->light, inst->node_world != NULL, inst->world.m[12], inst->world.m[13], inst->world.m[14]), puts("");
            /* 0x42e2c3/0x42e377/0x42e417: no sector, fade >= 0.98 or an empty sector light list drop the shadow. Whether
             * the light SEES the caster is never tested - a character standing in shadow still casts one, from the
             * fallback light (0x42e524). Nor whether the caster itself is on screen: 0x42b380 runs 0x42e2b0 with bit 2
             * for every instance of world+0x64, so a platform just outside the picture still shades the floor in it.
             * Gating on the caster's own visibility made those shadows vanish the moment the camera turned away. */
            if (!caster || !inst->visible || inst->fade > 0.98f || inst->light < 0 || !inst->node_world) continue;
            cast_shadow(r, inst);
        }
    }
    glDisable(GL_STENCIL_TEST); glDisable(GL_POLYGON_OFFSET_FILL); glEnableClientState(GL_COLOR_ARRAY); glEnableClientState(GL_TEXTURE_COORD_ARRAY); glEnable(GL_BLEND);
}

/* inverse of an affine matrix applied to a point (rotation, scale, translation) */
static int affine_inv_apply(const Mat4 *M, Vec3 w, Vec3 *out)
{
    const float *a = M->m; float x = w.x - a[12], y = w.y - a[13], z = w.z - a[14];
    float c00 = a[5] * a[10] - a[9] * a[6], c01 = a[8] * a[6] - a[4] * a[10], c02 = a[4] * a[9] - a[8] * a[5];
    float det = a[0] * c00 + a[1] * c01 + a[2] * c02; if (fabsf(det) < 1e-12f) return 0;
    float id = 1.0f / det;
    out->x = (c00 * x + c01 * y + c02 * z) * id;
    out->y = ((a[9] * a[2] - a[1] * a[10]) * x + (a[0] * a[10] - a[8] * a[2]) * y + (a[8] * a[1] - a[0] * a[9]) * z) * id;
    out->z = ((a[1] * a[6] - a[5] * a[2]) * x + (a[4] * a[2] - a[0] * a[6]) * y + (a[0] * a[5] - a[4] * a[1]) * z) * id;
    return 1;
}
/* texture frames for mesh nodes with typecode 5..8: the last type-5 event of the root node's event track with
 * t <= the current frame (0x43b58b-0x43b62c, docs/MODEL_RENDER.md 5). This is how the eyes blink. */
static void event_frames(const Instance *inst, uint32_t out[4])
{
    const Model *m = inst->model; out[0] = out[1] = out[2] = out[3] = 0;
    if (!m->nnodes || !m->nanims || inst->anim < 0 || (uint32_t)inst->anim >= m->nanims) return;
    const InsNode *n = &m->nodes[0]; if (!n->event_refs || !n->pool) return;
    const InsAnim *a = &m->anims[inst->anim]; float dur = a->duration_s > 0 ? a->duration_s : 1.0f;
    float ph = fmodf(inst->anim_time / dur, 1.0f); if (ph < 0) ph += 1.0f; float tf = ph * (float)a->nframes;
    const uint32_t *e = (const uint32_t *)(n->pool + ((size_t)n->a + n->b + n->event_refs[inst->anim].off) * 4);
    for (uint32_t i = 0; i < n->event_refs[inst->anim].cnt; i++) {
        uint32_t type = e[0], size = type == 3 ? 15 : type == 4 ? 9 : type == 5 ? 6 : 0; if (!size) return;
        float t; memcpy(&t, &e[1], 4);
        if (type == 5 && t <= tf) { out[0] = e[2]; out[1] = e[3]; out[2] = e[4]; out[3] = e[5]; }
        e += size;
    }
}

/* one mesh node layer. helper >= 0: UVs come from the helper child node (0x43b74d-0x43b908), the moving pupil */
/* ---- back-face culling (0x43bf65 for node polygons, 0x43c1a4 for the skinned triangles). The original culls per
 * polygon on the CPU - the device is left on D3DCULL_NONE - because polygon flag 0x2 marks a double-sided polygon that
 * must survive. Drawing the back faces too is not just wasted fill: a back face has its normals pointing away, so
 * lit_vertex_colour() gives it ndl = 0 and only the 0.6 * vcol ambient term, and wherever front and back tie in depth
 * (exactly along a silhouette) the dark one can win - a dark rim around every character.
 * The plane is the loader's (0x4280c2-0x428375): over every run of three consecutive vertices P, Q, R it keeps the one
 * with the longest n = (R-Q) x (R-P) above 0.01, and d = -n.R; no triple that long -> (1, 0, 0, 0). The winding alone
 * decides the side - the stored vertex normals are never looked at, and on some models they are junk: W1A model 18
 * (the glass lift plate, issue #2) has every face twice, textured and a reversed 0xFFFF copy, and choosing the side by
 * the normal sum kept the wrong one of each pair. */
static const float *poly_plane(Model *m, const InsNode *n, InsPoly *p)
{
    if (!p->plane_ok) {
        float best = 0, nx = 1, ny = 0, nz = 0; const InsPoint *rb = NULL;
        for (uint32_t i = 0; i < p->nverts; i++) {
            const InsPoint *P = &m->points[p->indices[i]], *Q = &m->points[p->indices[(i + 1) % p->nverts]], *R = &m->points[p->indices[(i + 2) % p->nverts]];
            float ux = R->pos.x - Q->pos.x, uy = R->pos.y - Q->pos.y, uz = R->pos.z - Q->pos.z;
            float vx = R->pos.x - P->pos.x, vy = R->pos.y - P->pos.y, vz = R->pos.z - P->pos.z;
            float cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx, l = sqrtf(cx * cx + cy * cy + cz * cz);
            if ((!rb || best < l) && l > 0.01f) { best = l; nx = cx / l; ny = cy / l; nz = cz / l; rb = R; }
        }
        p->plane[0] = nx; p->plane[1] = ny; p->plane[2] = nz; p->plane[3] = 0;
        if (rb) p->plane[3] = -(nx * (rb->pos.x - n->pivot.x) + ny * (rb->pos.y - n->pivot.y) + nz * (rb->pos.z - n->pivot.z));
        p->plane_ok = 1;
    }
    return p->plane;
}
/* the UV of one vertex of a node polygon: the helper projection when this mesh has a helper child (0x43b74d-0x43b908),
 * otherwise the planar projection of the pivot-relative rest point (0x43da37). lp = point - node pivot, wp = its world
 * position. Shared with rnd_uv_report() so the report cannot drift from what is drawn. */
static void node_poly_uv(const Instance *inst, int helper, const Material *mat, Vec3 lp, Vec3 wp, float *u, float *v)
{
    const Model *m = inst->model; const InsNode *h = helper >= 0 ? &m->nodes[helper] : NULL; Vec3 q;
    if (h && h->helper_a != 0 && h->helper_b != 0 && affine_inv_apply(&inst->node_world[helper], wp, &q)) {
        float ha = h->helper_mode == 0 ? q.y : q.x, hb = h->helper_mode == 2 ? q.y : q.z;
        *u = 0.5f - ha / h->helper_b; *v = hb / h->helper_a - 0.5f;
    } else material_uv(mat, lp.x, lp.y, lp.z, u, v);
}
/* the helper child of a mesh node, or -1: the renderer looks for a helper (kind 0x10) whose parent is this mesh
 * (0x43b716-0x43b746) */
static int node_helper(const Model *m, uint32_t ni)          /* the first helper child (kind 0x10) of node ni, -1 = none; per model, built once */
{
    if (!m->helper) {
        Model *w = (Model *)m; w->helper = (int32_t *)malloc((m->nnodes + 1) * sizeof *w->helper);
        for (uint32_t i = 0; i < m->nnodes; i++) w->helper[i] = -1;
        for (uint32_t j = m->nnodes; j-- > 0; ) if (m->nodes[j].kind == 0x10 && m->nodes[j].parent >= 0 && (uint32_t)m->nodes[j].parent < m->nnodes) w->helper[m->nodes[j].parent] = (int32_t)j;
    }
    return ni < m->nnodes ? m->helper[ni] : -1;
}

static void draw_node_polys(const Renderer *r, Instance *inst, uint32_t ni, int pass, uint32_t frame, int helper)
{
    Model *m = inst->model; InsNode *n = &m->nodes[ni]; const Material *mat;
    Vec3 cl; int have_cl = affine_inv_apply(&inst->node_world[ni], g_cam_pos, &cl);    /* the camera in this node's space */
    for (uint32_t k = 0; k < n->npolys; k++) {
        InsPoly *p = &n->polys[k]; if (p->nverts < 3 || mat_blended(r, p->material) != pass) continue;
        if (have_cl && !(p->flags & 2)) { const float *pl = poly_plane(m, n, p); if (pl[0] * cl.x + pl[1] * cl.y + pl[2] * cl.z + pl[3] <= 0) continue; }
        set_material(r, p->material, &mat, frame, inst);
        float du = 0, dv = 0; int scroll = mat && helper < 0 && mat->group < r->tex->ngroups && tex_scroll(inst, &r->tex->groups[mat->group], &du, &dv);
        bt_begin(1);
        for (uint32_t c = 0; c < p->nverts; c++) {
            InsPoint *pt = &m->points[p->indices[c]];
            Vec3 lp = { pt->pos.x - n->pivot.x, pt->pos.y - n->pivot.y, pt->pos.z - n->pivot.z };
            Vec3 wp = mat4_apply(&inst->node_world[ni], lp);
            if (mat) { float u, v; node_poly_uv(inst, helper, mat, lp, wp, &u, &v); if (scroll) { u += du; v += dv; } bt_texcoord(u, v); }
            { float base[3] = { 1, 1, 1 }; if (!mat && (p->material & 0x8000)) argb1555_to_rgb(p->material, base); if (mat || (p->material & 0x8000)) lit_vertex_colour(r, inst, &inst->node_world[ni], (int)ni, pt, base); }
            bt_vertex(wp.x, wp.y, wp.z);
        }
        bt_end();
    }
}

static void draw_instance(const Renderer *r, Instance *inst, int pass)   /* pass 0 = opaque, 1 = blended */
{
    Model *m = inst->model;
    if (inst->type == 60) return;                                /* a water volume draws its own surface (Draw 0x4738c0, water.c), never its box */
    const Material *mat;
    uint32_t evf[4]; event_frames(inst, evf);
    instance_dyn(r, inst);
    /* rigid node polygons: only mesh nodes, never those with typecode 2 (0x43b6c2) */
    for (uint32_t ni = 0; ni < m->nnodes; ni++) {
        InsNode *n = &m->nodes[ni]; if (n->kind != 0 || !n->polys || n->type_code == 2) continue;
        int helper = node_helper(m, ni);
        uint32_t lid = (n->type_code >= 5 && n->type_code <= 8) ? evf[n->type_code - 5] : 0;
        draw_node_polys(r, inst, ni, pass, 0, helper);
        if (lid) { bt_flush(); glDepthFunc(GL_LEQUAL); draw_node_polys(r, inst, ni, pass, lid, -1); bt_flush(); glDepthFunc(g_zfunc); }   /* eyelid layer on top of the eyeball */
    }
    /* skinned triangles */
    if (m->ntris) {
        uint32_t last = 0xffffffff; const int32_t *own = model_owner(m); float base[3] = { 1, 1, 1 };
        bt_begin(0);
        for (uint32_t t = 0; t < m->ntris; t++) {
            InsTri *tr = &m->tris[t]; if (mat_blended(r, tr->material) != pass) continue;
            if (tr->material != last) { bt_end(); set_material(r, tr->material, &mat, 0, NULL); last = tr->material; base[0] = base[1] = base[2] = 1; if (tr->material & 0x8000) argb1555_to_rgb(tr->material, base); bt_begin(0); }
            uint32_t idx[3] = { tr->i0, tr->i1, tr->i2 };
            Vec3 wp[3]; const Mat4 *MM[3];
            for (int c = 0; c < 3; c++) {
                int o = own[idx[c]]; Vec3 lp = m->points[idx[c]].pos; if (o >= 0) { lp.x -= m->nodes[o].pivot.x; lp.y -= m->nodes[o].pivot.y; lp.z -= m->nodes[o].pivot.z; }
                MM[c] = o >= 0 ? &inst->node_world[o] : &inst->world; wp[c] = mat4_apply(MM[c], lp);
            }
            {   /* 0x43c1a4: n = (A - B) x (A - C) in world space, front-facing iff n . (camera - A) > 0 */
                float ux = wp[0].x - wp[1].x, uy = wp[0].y - wp[1].y, uz = wp[0].z - wp[1].z;
                float vx = wp[0].x - wp[2].x, vy = wp[0].y - wp[2].y, vz = wp[0].z - wp[2].z;
                float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                if (nx * (g_cam_pos.x - wp[0].x) + ny * (g_cam_pos.y - wp[0].y) + nz * (g_cam_pos.z - wp[0].z) <= 0) continue;
            }
            for (int c = 0; c < 3; c++) {
                InsPoint *pt = &m->points[idx[c]];
                if (mat || (tr->material & 0x8000)) lit_vertex_colour(r, inst, MM[c], own[idx[c]], pt, base);
                if (mat) bt_texcoord(mat->m[6 - 3 * c], mat->m[7 - 3 * c]);   /* explicit UVs: the material holds three UV pairs, file vertex j = (m[3j], m[3j+1]) and i0 is the third file vertex (0x43e39a) */
                bt_vertex(wp[c].x, wp[c].y, wp[c].z);
            }
        }
        bt_end();
    }
}

/* ---- WOODY_UVLOG (test helper): per mesh node and material of one instance, the texture group it binds and the UV
 * range this renderer generates for it. A surface whose polygons each span far less than one tile samples a single
 * texel and comes out as one flat colour; a span of tens of tiles is a projection that does not belong to the polygon.
 * A sane model polygon lands in 0..1 (docs/MODEL_RENDER.md 4). The same numbers as tools/modeluv.py formula B and
 * helperuv.py, but on the running pose and with the texture frame the instance really binds. */
void rnd_uv_report(const Renderer *r, const Instance *inst)
{
    const Model *m = inst->model;
    printf("uvlog inst %u model %d: %u nodes, %u anims, anim %d, tex_mode %d fac %.2f, pos %.0f %.0f %.0f", inst->index,
           (int)(m - r->ins->models), m->nnodes, m->nanims, inst->anim, inst->tex_mode, inst->tex_fac,
           inst->world.m[12], inst->world.m[13], inst->world.m[14]); puts("");
    for (uint32_t ni = 0; ni < m->nnodes; ni++) {
        const InsNode *n = &m->nodes[ni]; if (n->kind != 0 || !n->polys || !n->npolys) continue;
        int helper = node_helper(m, ni);
        printf("  node %u tc %u pivot (%.1f %.1f %.1f) polys %u", ni, n->type_code, n->pivot.x, n->pivot.y, n->pivot.z, n->npolys);
        if (helper >= 0) printf(" helper %d (mode %u, v_c %.1f v_8 %.1f)", helper, m->nodes[helper].helper_mode, m->nodes[helper].helper_a, m->nodes[helper].helper_b);
        puts("");
        for (uint32_t k = 0; k < n->npolys; k++) {
            uint32_t mi = n->polys[k].material; int seen = 0;
            for (uint32_t q = 0; q < k && !seen; q++) seen = n->polys[q].material == mi;
            if (seen) continue;
            const Material *mat = (mi & 0x8000) || mi >= r->tex->nmaterials ? NULL : &r->tex->materials[mi];
            float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f, span = 0; uint32_t np = 0;
            for (uint32_t q = k; q < n->npolys; q++) {
                const InsPoly *p = &n->polys[q]; if (p->material != mi || p->nverts < 3) continue;
                np++; if (!mat) continue;
                float pu0 = 1e30f, pu1 = -1e30f, pv0 = 1e30f, pv1 = -1e30f;
                for (uint32_t c = 0; c < p->nverts; c++) {
                    const InsPoint *pt = &m->points[p->indices[c]];
                    Vec3 lp = { pt->pos.x - n->pivot.x, pt->pos.y - n->pivot.y, pt->pos.z - n->pivot.z };
                    float u, v; node_poly_uv(inst, helper, mat, lp, mat4_apply(&inst->node_world[ni], lp), &u, &v);
                    if (u < pu0) pu0 = u; if (u > pu1) pu1 = u; if (v < pv0) pv0 = v; if (v > pv1) pv1 = v;
                }
                if (pu0 < u0) u0 = pu0; if (pu1 > u1) u1 = pu1; if (pv0 < v0) v0 = pv0; if (pv1 > v1) v1 = pv1;
                if (pu1 - pu0 > span) span = pu1 - pu0; if (pv1 - pv0 > span) span = pv1 - pv0;
            }
            if (!mat) { printf("    mat %04x flat colour, polys %u", mi, np); puts(""); continue; }
            if (mat->group >= r->tex->ngroups) { printf("    mat %04x group %u OUT OF RANGE, polys %u", mi, mat->group, np); puts(""); continue; }
            const TexGroup *g = &r->tex->groups[mat->group];
            uint32_t fr = inst->tex_mode ? tex_frame(inst, g) : 0; if (fr >= g->frame_count) fr = 0;
            printf("    mat %04x group %u (%ux%u flags %08x frames %u dur %.2f) frame %u polys %u u[%8.2f %8.2f] v[%8.2f %8.2f] widest poly %.2f",
                   mi, mat->group, g->width, g->height, g->flags, g->frame_count, g->anim_duration, fr, np, u0, u1, v0, v1, span); puts("");
        }
    }
    for (uint32_t t = 0; t < m->ntris; t++) {                                  /* skinned triangles: explicit UVs, three pairs per material (0x43e39a) */
        uint32_t mi = m->tris[t].material; int seen = 0;
        for (uint32_t q = 0; q < t && !seen; q++) seen = m->tris[q].material == mi;
        if (seen) continue;
        uint32_t np = 0; for (uint32_t q = t; q < m->ntris; q++) if (m->tris[q].material == mi) np++;
        if ((mi & 0x8000) || mi >= r->tex->nmaterials) { printf("  tris mat %04x flat colour, tris %u", mi, np); puts(""); continue; }
        const Material *mat = &r->tex->materials[mi];
        printf("  tris mat %04x group %u tris %u uv (%.2f %.2f) (%.2f %.2f) (%.2f %.2f)", mi, mat->group, np,
               mat->m[0], mat->m[1], mat->m[3], mat->m[4], mat->m[6], mat->m[7]); puts("");
    }
}

/* ---- outline (0x43ea30, fed by the two back-face lists 0x43b3f0 collects): the back faces once more, their stamped
 * vertices pushed out along their own normal, flat black, at the same depth as the model. Only for instances that carry
 * SetFlags bit 0x20 (message 45) - the characters and handful of props the level script flags - and only within 1500 units.
 * w = d/300 up to 2.5, then 5 - d/300 (0x43b4ce..0x43b4f3), so the rim keeps a constant width on screen. Drawn after
 * the model with the ordinary depth test: outside the silhouette the hull is all there is, and where it pokes through
 * a concave fold it beats the model - that is where the creases along a snout or a finger come from. */
static float *g_ol; static uint32_t g_ol_n, g_ol_cap;
static void ol_push(Vec3 a, Vec3 b, Vec3 c)
{
    if (g_ol_n + 1 > g_ol_cap) { g_ol_cap = g_ol_cap * 2 + 1024; g_ol = (float *)realloc(g_ol, (size_t)g_ol_cap * 9 * sizeof(float)); }
    float *o = &g_ol[(size_t)g_ol_n * 9]; o[0] = a.x; o[1] = a.y; o[2] = a.z; o[3] = b.x; o[4] = b.y; o[5] = b.z; o[6] = c.x; o[7] = c.y; o[8] = c.z; g_ol_n++;
}
static Vec3 ol_vertex(const Instance *inst, const Mat4 *M, const InsPoint *pt, Vec3 pivot, float w)
{
    Vec3 n = pt->normal; float l = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);   /* the original normalises at load (0x427c01), the port does not */
    if (l > 1e-6f) { n.x /= l; n.y /= l; n.z /= l; } else { n.x = n.y = n.z = 0; }
    Vec3 lp = { pt->pos.x - pivot.x + w * n.x, pt->pos.y - pivot.y + w * n.y, pt->pos.z - pivot.z + w * n.z };
    (void)inst; return mat4_apply(M, lp);
}
static void draw_outline(const Renderer *r, Instance *inst)
{
    Vec3 c = ins_anim_centre(inst);                                            /* inst+0x60, the ANIMATED root: W1A's Buzz is placed 1800 units from where his cinematic */
    float dx = c.x - g_cam_pos.x, dy = c.y - g_cam_pos.y, dz = c.z - g_cam_pos.z;   /* walks him, and measured from the .ins position he lost his rim (issue #35) */
    float d = sqrtf(dx * dx + dy * dy + dz * dz), w = d / 300.0f;               /* 0x43b447..0x43b4fe */
    if (w > 2.5f) w = 5.0f - w;                                                /* past 750 the rim narrows again, past 1500 there is none */
    if (g_shlog && (inst->type || inst->model->ntris) && (!(inst->setflags & 0x20) || w <= 0))   /* WOODY_SHLOG: why a character (typed, or skinned like every character model) has no rim */
        printf("  OL inst %u type %d setflags %x d %.0f: no outline (%s)", inst->index, inst->type, inst->setflags, d,
               !(inst->setflags & 0x20) ? "no SetFlags bit 0x20" : "further than 1500"), puts("");
    if (!(inst->setflags & 0x20) || !inst->node_world || w <= 0) return;        /* 0x43b423; the second gate is the cfg detail level, 2 in the shipped Woody.cfg */
    { const char *e = wenv("WOODY_OLW"); if (e) w *= (float)atof(e); }     /* test helper: scale the rim */
    Model *m = inst->model; const int32_t *own = model_owner(m); const Vec3 zero = { 0, 0, 0 };
    g_ol_n = 0;
    /* Which corners move out: 0x43b3f0 first stamps 0xffff0000 into v+0x40 of the vertex records of every back face -
     * all three corners of a skin triangle (0x43c3bc), but only the index words +0x18/+0x1a/+0x1c, corners 0/1/2, of a
     * node polygon (0x43c42d) - then 0x43c49a..0x43c56a rewrites the position of each stamped vertex as
     * M_node ((p - pivot) + w n) and 0x43ea30 draws every back face from those records. A fourth (or later) corner
     * that no other back face stamps keeps the plain position the model pass computed, so such a quad's rim tapers
     * to the surface at that corner. Reproduced: pass 0 stamps, pass 1 emits. */
    static uint8_t *mark; static uint32_t mark_cap;
    if (mark_cap < m->npoints) { mark_cap = m->npoints + 256; free(mark); mark = (uint8_t *)malloc(mark_cap); }
    if (!mark) return;
    memset(mark, 0, m->npoints);
    for (int pass = 0; pass < 2; pass++) {
    for (uint32_t ni = 0; ni < m->nnodes; ni++) {
        InsNode *n = &m->nodes[ni]; if (n->kind != 0 || !n->polys || n->type_code == 2) continue;
        if (n->type_code >= 5 && n->type_code <= 8) continue;                   /* 0x43bf65 throws the eyelid layer's back faces away */
        Vec3 cl; if (!affine_inv_apply(&inst->node_world[ni], g_cam_pos, &cl)) continue;
        for (uint32_t k = 0; k < n->npolys; k++) {
            InsPoly *p = &n->polys[k];
            if (p->nverts < 3 || (p->flags & 2) || (p->flags & 0x60)) continue; /* double sided and blended polygons never outline (0x43c0c2) */
            const float *pl = poly_plane(m, n, p);
            if (pl[0] * cl.x + pl[1] * cl.y + pl[2] * cl.z + pl[3] > 0) continue;   /* front facing: the model pass drew it */
            if (pass == 0) { for (uint32_t c = 0; c < 3; c++) if (p->indices[c] < m->npoints) mark[p->indices[c]] = 1; continue; }
            Vec3 v[3];
            for (uint32_t c = 0; c < p->nverts; c++) {
                Vec3 q = ol_vertex(inst, &inst->node_world[ni], &m->points[p->indices[c]], n->pivot, p->indices[c] < m->npoints && mark[p->indices[c]] ? w : 0.0f);
                if (c == 0) v[0] = q; else { v[1] = v[2]; v[2] = q; if (c >= 2) ol_push(v[0], v[2], v[1]); }
            }
        }
    }
    for (uint32_t t = 0; t < m->ntris; t++) {
        InsTri *tr = &m->tris[t];
        uint32_t idx[3] = { tr->i0, tr->i1, tr->i2 }; Vec3 wp[3]; const Mat4 *MM[3]; Vec3 pv[3];
        for (int c = 0; c < 3; c++) {
            int o = own[idx[c]]; pv[c] = o >= 0 ? m->nodes[o].pivot : zero;
            Vec3 lp = { m->points[idx[c]].pos.x - pv[c].x, m->points[idx[c]].pos.y - pv[c].y, m->points[idx[c]].pos.z - pv[c].z };
            MM[c] = o >= 0 ? &inst->node_world[o] : &inst->world; wp[c] = mat4_apply(MM[c], lp);
        }
        float ux = wp[0].x - wp[1].x, uy = wp[0].y - wp[1].y, uz = wp[0].z - wp[1].z;
        float vx = wp[0].x - wp[2].x, vy = wp[0].y - wp[2].y, vz = wp[0].z - wp[2].z;
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        if (nx * (g_cam_pos.x - wp[0].x) + ny * (g_cam_pos.y - wp[0].y) + nz * (g_cam_pos.z - wp[0].z) > 0) continue;   /* front facing */
        if (pass == 0) { for (int c = 0; c < 3; c++) if (idx[c] < m->npoints) mark[idx[c]] = 1; continue; }
        Vec3 e[3];
        for (int c = 0; c < 3; c++) e[c] = ol_vertex(inst, MM[c], &m->points[idx[c]], pv[c], idx[c] < m->npoints && mark[idx[c]] ? w : 0.0f);
        ol_push(e[0], e[2], e[1]);
    }
    }
    if (g_shlog) printf("  OL inst %u setflags %x w %.2f tris %u fade %.2f", inst->index, inst->setflags, w, g_ol_n, inst->fade), puts("");
    if (!g_ol_n) return;
    bt_flush();
    float a = 2.0f * (1.0f - inst->fade); if (a > 1) a = 1;                     /* 0x43ece9: twice the opacity, clamped */
    glDisable(GL_TEXTURE_2D); glDisable(GL_ALPHA_TEST); glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if (g_fading) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_TRUE); }   /* a fading instance's contour is one more batch of its fade list (0x43c59d passes the same mode) */
    else if (a < 0.999f) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_FALSE); } else { glDisable(GL_BLEND); glDepthMask(GL_TRUE); }
    glColor4f(0, 0, 0, a);
    glEnableClientState(GL_VERTEX_ARRAY);                                      /* bt_flush() leaves it disabled */
    glVertexPointer(3, GL_FLOAT, 0, g_ol); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_ol_n * 3);
    glDisableClientState(GL_VERTEX_ARRAY);
    glColor4f(1, 1, 1, 1); glDepthMask(GL_TRUE); glEnable(GL_TEXTURE_2D); glEnable(GL_ALPHA_TEST);
    glEnableClientState(GL_COLOR_ARRAY); glEnableClientState(GL_TEXTURE_COORD_ARRAY); glEnable(GL_BLEND);
    g_last_material = 0xffffffffu;
}

int rnd_project(const Window *w, const FreeCamera *cam, Vec3 p, float *sx, float *sy)
{
    Vec3 fw = cam_forward(cam), rt = cam_right(cam);
    Vec3 up = { rt.y * fw.z - rt.z * fw.y, rt.z * fw.x - rt.x * fw.z, rt.x * fw.y - rt.y * fw.x };
    Vec3 d = { p.x - cam->pos.x, p.y - cam->pos.y, p.z - cam->pos.z };
    float ex = d.x * rt.x + d.y * rt.y + d.z * rt.z, ey = d.x * up.x + d.y * up.y + d.z * up.z, ez = d.x * fw.x + d.y * fw.y + d.z * fw.z;
    if (ez <= 0.001f || !w->width || !w->height) return 0;
    float aspect = view_aspect(w, cam);
    int vpx = 0, vpy = 0, vpw = w->width, vph = w->height;
    if (cam->letterbox) lb_strip(w, cam->letterbox, &vpx, &vpy, &vpw, &vph);
    float f = 1.0f / tanf(cam->fov_deg * 3.14159265f / 360.0f);
    float ndx = f / aspect * ex / ez, ndy = f * ey / ez;
    if (ndx < -1 || ndx > 1 || ndy < -1 || ndy > 1) return 0;           /* the four side planes; there is no near/far test */
    *sx = (vpx + (ndx * 0.5f + 0.5f) * vpw) * 640.0f / w->width;
    *sy = (w->height - (vpy + (ndy * 0.5f + 0.5f) * vph)) * 480.0f / w->height;
    return 1;
}

/* WOODY_DYNLIGHT: the dynamic lights on the world. Every drawn opaque face in front of the light and within its radius
 * gets one more additive pass: its own texture x vertex colour (unit 0) x the radial light texture 15 - round(k * 15.49)
 * placed by the sphere projection of 0x498830 (unit 1), colour 2 * vcol * C * k. That adds exactly
 * 2 * tex * vcol * C/255 * max(0, 1 - |P - L|/R) to the pixel, the term a static light adds in the multipass (recipe 4),
 * but on unlit single-pass faces too and without the min(1, ...) of the framebuffer sum. */
typedef void (APIENTRY *PFN_ActiveTex)(GLenum);
typedef void (APIENTRY *PFN_MultiTC2f)(GLenum, GLfloat, GLfloat);
static void draw_dyn_world(Renderer *r)
{
    static int init; static PFN_ActiveTex act; static PFN_MultiTC2f mtc; static uint32_t *stamp, nstamp, gen;
    if (!init) { init = 1;
        act = (PFN_ActiveTex)plat_gl_proc("glActiveTexture"); if (!act) act = (PFN_ActiveTex)plat_gl_proc("glActiveTextureARB");
        mtc = (PFN_MultiTC2f)plat_gl_proc("glMultiTexCoord2f"); if (!mtc) mtc = (PFN_MultiTC2f)plat_gl_proc("glMultiTexCoord2fARB"); }
    if (!act || !mtc || !r->light_tex[0]) return;
    const GelFile *g = r->gel; const TexFile *tx = r->tex;
    if (nstamp < g->npolys) { free(stamp); nstamp = g->npolys; stamp = (uint32_t *)calloc(nstamp, 4); gen = 0; }
    enum { GL_TEX0 = 0x84C0, GL_TEX1 = 0x84C1 };
    glDepthMask(GL_FALSE); glDepthFunc(GL_LEQUAL); glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE);
    glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-1.0f, -1.0f); glEnable(GL_ALPHA_TEST);
    act(GL_TEX1); glEnable(GL_TEXTURE_2D); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    act(GL_TEX0); glEnable(GL_TEXTURE_2D);
    uint32_t bound0 = 0, bound1 = 0;
    for (int li = 0; li < g_ndyn; li++) {
        const DynLight *L = &g_dyn[li]; float R = L->radius;
        float box[6] = { L->pos.x - R, L->pos.x + R, L->pos.y - R, L->pos.y + R, L->pos.z - R, L->pos.z + R };
        GelPolySet set = gel_polys_in_box(g, box); int ndbg[4] = { 0 };   /* WOODY_DYNLOG counts */
        if (++gen == 0) { memset(stamp, 0, (size_t)nstamp * 4); gen = 1; }
        for (uint32_t k = 0; k < set.n; k++) {
            uint32_t f = set.polys[k]; if (f >= g->npolys || stamp[f] == gen) continue; stamp[f] = gen;
            const struct FaceBatch *fb = &r->face_batch[f]; if (!fb->ntris) continue;           /* invisible, sky or degenerate */
            ndbg[1]++; if (r->cull && r->face_stamp[f] != r->stamp_gen) continue;                           /* not drawn this frame */
            ndbg[2]++;
            const GelPoly *p = &g->polys[f]; const float *pl = p->plane;
            if (tx->groups[fb->group].flags & 2) continue;                                       /* additive groups stay as they are */
            float dist = pl[0] * L->pos.x + pl[1] * L->pos.y + pl[2] * L->pos.z + pl[3];
            if (dist <= 0 || dist >= R) continue;                                                /* the light must be in front of the face */
            if (pl[0] * g_cam_pos.x + pl[1] * g_cam_pos.y + pl[2] * g_cam_pos.z + pl[3] <= 0) continue;   /* 0x42c33c: back face */
            if (r->face_bound) { const float *b = &r->face_bound[4 * f]; float dx = b[0] - L->pos.x, dy = b[1] - L->pos.y, dz = b[2] - L->pos.z;
                                 if (dx * dx + dy * dy + dz * dz > (R + b[3]) * (R + b[3])) continue; }
            float kk = 1.0f - dist / R; int ti = 15 - (int)(kk * 15.49f + 0.5f); if (ti < 0) ti = 0; if (ti > 15) ti = 15;
            float s = 0.5f / sqrtf(R * R - dist * dist), F[3] = { L->pos.x - pl[0] * dist, L->pos.y - pl[1] * dist, L->pos.z - pl[2] * dist };
            const GelVert *v2 = &g->verts[p->indices[2]];                                        /* 0x498890: U towards the third vertex */
            float U[3] = { v2->x - F[0], v2->y - F[1], v2->z - F[2] }, ul = sqrtf(U[0] * U[0] + U[1] * U[1] + U[2] * U[2]);
            if (ul < 1e-4f) continue;
            for (int q = 0; q < 3; q++) U[q] /= ul;
            float W[3] = { pl[1] * U[2] - pl[2] * U[1], pl[2] * U[0] - pl[0] * U[2], pl[0] * U[1] - pl[1] * U[0] };
            const Material *m = &tx->materials[p->material & 0x7fff]; uint32_t t0 = tx->groups[fb->group].gl_tex, t1 = r->light_tex[ti];
            if (t0 != bound0) { bound0 = t0; glBindTexture(GL_TEXTURE_2D, t0); }
            if (t1 != bound1) { bound1 = t1; act(GL_TEX1); glBindTexture(GL_TEXTURE_2D, t1); act(GL_TEX0); }
            ndbg[3]++; glBegin(GL_POLYGON);
            for (uint32_t c = 0; c < p->nverts; c++) {
                const GelVert *v = &g->verts[p->indices[c]]; float u0, v0, d[3] = { v->x - F[0], v->y - F[1], v->z - F[2] }, col[3];
                for (int q = 0; q < 3; q++) { col[q] = 2.0f * (float)((v->colour >> (8 * q)) & 0xff) / 255.0f * L->rgb[q] / 255.0f * kk; if (col[q] > 1) col[q] = 1; }
                material_uv(m, v->x, v->y, v->z, &u0, &v0);
                glColor3f(col[0], col[1], col[2]); glTexCoord2f(u0, v0);
                mtc(GL_TEX1, 0.5f + s * (d[0] * W[0] + d[1] * W[1] + d[2] * W[2]), 0.5f + s * (d[0] * U[0] + d[1] * U[1] + d[2] * U[2]));
                glVertex3f(v->x, v->y, v->z);
            }
            glEnd();
        }
        if (wenv("WOODY_DYNLOG")) printf("  DYNWORLD light %d: %u faces in its box, %d drawable, %d drawn this frame, %d lit", li, set.n, ndbg[1], ndbg[2], ndbg[3]), puts("");
    }
    act(GL_TEX1); glDisable(GL_TEXTURE_2D); act(GL_TEX0);
    glDisable(GL_POLYGON_OFFSET_FILL); glDisable(GL_BLEND); glDepthMask(GL_TRUE); glDepthFunc(GL_LESS);
}

/* the lists of 0x428d00 for this frame: built by rnd_frame (fade list +0x1c4 and the glow faces of +0x1cc, each with the sort
 * depth its batch copied from [0x5ac8d4]), drawn by rnd_sorted together with the world sprites hud.c recorded meanwhile */
static struct {
    Instance **fl, **al; float *fd, *ad; uint32_t fn, an, fcap, acap;
    Vec3 pos, fw, rt, up; float sx, sy;                              /* camera and the side-plane scales of the frame */
    int on;
} g_srt;
static double T[6]; static int TN;                                   /* WOODY_PROF */

void rnd_frame(Renderer *r, const Window *w, const FreeCamera *cam, float time_s)
{
    glViewport(w->vx, w->vy, w->width, w->height);
    g_tex_now = time_s; g_cam_pos = cam->pos;
    if (g_aniso_dirty) {                                             /* the texture sharpness changed: every loaded texture */
        g_aniso_dirty = 0;
        for (uint32_t g = 0; g < r->tex->ngroups; g++) { const TexGroup *tg = &r->tex->groups[g]; if (!tg->gl_frames) continue;
            for (uint32_t k = 0; k < tg->frame_count; k++) if (tg->gl_frames[k]) { glBindTexture(GL_TEXTURE_2D, tg->gl_frames[k]); tex_aniso(); } }
        glBindTexture(GL_TEXTURE_2D, 0); g_last_material = 0xffffffffu;
    }
    for (uint32_t g = 0; g < r->tex->ngroups; g++) {                 /* texture animation: frame_count frames over anim_duration seconds */
        TexGroup *tg = &r->tex->groups[g];
        if (tg->frame_count > 1 && tg->anim_duration > 0 && ((tg->flags >> 8) & 0xff) != 2) tg->gl_tex = tg->gl_frames[(uint32_t)(time_s / tg->anim_duration * tg->frame_count) % tg->frame_count];
    }
    glDepthMask(GL_TRUE); glDisable(GL_BLEND); glClearColor(0, 0, 0, 1); glClearStencil(0);   /* 0x47ee70 (0x40174e, every frame): Clear(TARGET | ZBUFFER, colour 0 = black, z 1.0) */ glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    double q0 = win_time(); g_srt.on = 0; g_srt.fn = g_srt.an = 0;
    /* camera basis and frustum first: what is visible decides what still has to be lit and drawn */
    float aspect = view_aspect(w, cam), zn = 5.0f, zf = 200000.0f;   /* letterbox: the 16:9 image strip, see the viewport below */
    float f = 1.0f / tanf(cam->fov_deg * 3.14159265f / 360.0f);
    Vec3 fw = cam_forward(cam), rt = cam_right(cam);
    Vec3 up = { rt.y * fw.z - rt.z * fw.y, rt.z * fw.x - rt.x * fw.z, rt.x * fw.y - rt.y * fw.x };   /* right x forward */
    float frust[6][4]; frustum_planes(cam, fw, rt, up, aspect, f, zn, zf, frust);
    g_srt.pos = cam->pos; g_srt.fw = fw; g_srt.rt = rt; g_srt.up = up; g_srt.sx = f / aspect; g_srt.sy = f;
    { double a = win_time(); world_visibility(r, cam, frust); T[5] += win_time() - a; }
    {   /* Pose every visible instance, on screen or not: player.c collides against node_world, so a platform that
         * stops being posed stops carrying the player. Lighting goes to what is actually drawn, and to every shadow caster. */
        float dt = time_s - r->last_time; if (dt < 0 || dt > 0.25f) dt = 0.016f; r->last_time = time_s;
        for (uint32_t mi = 0; mi < r->ins->nmodels; mi++) { Model *m = &r->ins->models[mi]; for (uint32_t k = 0; k < m->ninstances; k++) {
            Instance *inst = &m->instances[k]; inst->drawn = 0;
            if (!inst->visible || inst->fade > 0.98f) continue;                /* 0x42e374 */
            ins_pose(inst, inst->anim, inst->anim_time); } }
        for (uint32_t mi = 0; mi < r->ins->nmodels; mi++) { Model *m = &r->ins->models[mi]; for (uint32_t k = 0; k < m->ninstances; k++) {
            Instance *inst = &m->instances[k];
            if (!inst->visible || inst->fade > 0.98f) continue;
            inst->drawn = (!r->list_on || inst->in_zone || !list_drawn(r, inst)) && instance_visible(r, inst, aspect, f, fw, rt, up);
            if ((inst->drawn || shadow_caster(inst)) && r->lit) instance_light(r, inst, dt); } }   /* a caster off screen still needs its light for the shadow */
        if (r->nlinks) links_hide(r, cam->pos);                                /* message 34: 0x42aa0b runs before the sector walk 0x42a840 */
        if (r->on_drawn) for (uint32_t mi = 0; mi < r->ins->nmodels; mi++) for (uint32_t k = 0; k < r->ins->models[mi].ninstances; k++)
            if (r->ins->models[mi].instances[k].drawn) r->on_drawn(&r->ins->models[mi].instances[k]);   /* vtbl[26], before the colours are used */
    }
    glEnable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GEQUAL, 127.0f / 255.0f);   /* 0x47ec50/0x47ec5c: ALPHAREF 0x7f, GREATEREQUAL. The device stays on CULL_NONE; the culling is per polygon on the CPU */
    glPolygonMode(GL_FRONT_AND_BACK, r->wireframe ? GL_LINE : GL_FILL);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    double q1 = win_time(); T[0] += q1 - q0;

    /* projection: the world data is right-handed (3ds Max export, y up after the -90 deg x instance rotation), so a plain GL frustum */
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    if (cam->letterbox) {                     /* image strip y = 30..390 of 480: black above (30) and below (90), docs/CAMERA_SCRIPT.md 2.4 */
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        int x, y, vw, vh; lb_strip(w, cam->letterbox, &x, &y, &vw, &vh);
        glViewport(w->vx + x, w->vy + y, vw, vh);   /* 1 = centred (cinematics, 0x41f8d0), 2 = shifted up (mode 4) */
    }
    float proj[16] = { f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (zf + zn) / (zn - zf), -1, 0, 0, 2 * zf * zn / (zn - zf), 0 };
    glMultMatrixf(proj);
    /* view: look-at from the free camera */
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    float view[16] = { rt.x, up.x, -fw.x, 0, rt.y, up.y, -fw.y, 0, rt.z, up.z, -fw.z, 0,          /* GL camera looks along -z */
                       -(rt.x * cam->pos.x + rt.y * cam->pos.y + rt.z * cam->pos.z), -(up.x * cam->pos.x + up.y * cam->pos.y + up.z * cam->pos.z), (fw.x * cam->pos.x + fw.y * cam->pos.y + fw.z * cam->pos.z), 1 };
    glLoadMatrixf(view);
    if (r->have_sky && r->show_world && r->sky_on) {                               /* 0x42ad40..0x42b373: five quads of a cube around the camera, white, unlit, drawn behind everything */
        static const signed char q[5][4][3] = {
            { {-1,-1, 1}, {-1, 1, 1}, { 1, 1, 1}, { 1,-1, 1} }, { { 1,-1, 1}, { 1, 1, 1}, { 1, 1,-1}, { 1,-1,-1} },
            { { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1}, {-1,-1,-1} }, { {-1,-1,-1}, {-1, 1,-1}, {-1, 1, 1}, {-1,-1, 1} },
            { {-1, 1, 1}, {-1, 1,-1}, { 1, 1,-1}, { 1, 1, 1} } };
        const float S = 50000.0f, hu = r->sky_hu, hv = r->sky_hv, uv[4][2] = { { hu, hv }, { hu, 1 - hv }, { 1 - hu, 1 - hv }, { 1 - hu, hv } };
        glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDisable(GL_CULL_FACE); glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST); glEnable(GL_TEXTURE_2D); glColor3f(1, 1, 1);
        for (int f = 0; f < 5; f++) {
            glBindTexture(GL_TEXTURE_2D, r->sky_tex[f]);
            glBegin(GL_QUADS);
            for (int c = 0; c < 4; c++) { glTexCoord2f(uv[c][0], uv[c][1]); glVertex3f(cam->pos.x + q[f][c][0] * S, cam->pos.y + q[f][c][1] * S, cam->pos.z + q[f][c][2] * S); }
            glEnd();
        }
        glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE);
    }

    for (int pass = 0; pass < 2; pass++) {                      /* pass 0 opaque, pass 1 additive world faces (no depth writes); the additive model faces go through the buckets below */
    if (r->show_world) {
        glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        set_blend(pass);
        for (uint32_t i = 0; i < r->nbatches; i++) {
            struct WorldBatch *b = &r->batches[i]; if (batch_empty(r, b) || group_blended(r, b->group) != pass) continue;
            glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, r->tex->groups[b->group].gl_tex);
            glVertexPointer(3, GL_FLOAT, 0, b->pos); glTexCoordPointer(2, GL_FLOAT, 0, b->uv); glColorPointer(4, GL_UNSIGNED_BYTE, 0, b->col);
            batch_draw(r, b);
        }
        if (pass == 0 && r->lit) {
            /* 1. ambient fill: flat 0x4C4C4C, opaque, writes z */
            glDisable(GL_TEXTURE_2D); glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY); glDisable(GL_ALPHA_TEST);
            if (r->show_light) glColor3f(LIT_AMB, LIT_AMB, LIT_AMB); else glColor3f(1, 1, 1);
            for (uint32_t i = 0; i < r->nbatches; i++) { struct WorldBatch *b = &r->litb[i]; if (batch_empty(r, b)) continue; glVertexPointer(3, GL_FLOAT, 0, b->pos); batch_draw(r, b); }
            glEnableClientState(GL_COLOR_ARRAY); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glDepthMask(GL_FALSE); glDepthFunc(GL_LEQUAL); glEnable(GL_BLEND);
            /* 2. light polygons, additive (flush 0x4293f0) */
            if (r->show_light) {
                glBlendFunc(GL_ONE, GL_ONE); glEnable(GL_TEXTURE_2D); glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-1.0f, -1.0f);
                for (int t = 0; t < 16; t++) {
                    struct WorldBatch *b = &r->lightb[t]; if (batch_empty(r, b)) continue;
                    glBindTexture(GL_TEXTURE_2D, r->light_tex[t]);
                    glVertexPointer(3, GL_FLOAT, 0, b->pos); glTexCoordPointer(2, GL_FLOAT, 0, b->uv); glColorPointer(4, GL_UNSIGNED_BYTE, 0, b->col);
                    batch_draw(r, b);
                }
                glDisable(GL_POLYGON_OFFSET_FILL);
            }
            { double a = win_time(); if (r->show_light && r->show_instances) draw_cast_shadows(r); T[2] += win_time() - a; }
            /* 3. texture pass: 2 * src * dst (0x4296a4) */
            glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);
            for (uint32_t i = 0; i < r->nbatches; i++) {
                struct WorldBatch *b = &r->litb[i]; if (batch_empty(r, b)) continue;
                glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, r->tex->groups[b->group].gl_tex);
                glVertexPointer(3, GL_FLOAT, 0, b->pos); glTexCoordPointer(2, GL_FLOAT, 0, b->uv); glColorPointer(4, GL_UNSIGNED_BYTE, 0, b->col);
                batch_draw(r, b);
            }
            glDisable(GL_BLEND); glDepthMask(GL_TRUE); glDepthFunc(GL_LESS); glEnable(GL_ALPHA_TEST);
        }
        glDisableClientState(GL_VERTEX_ARRAY); glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        if (pass == 0 && g_ndyn && r->show_light && dyn_draw()) draw_dyn_world(r);   /* WOODY_DYNLIGHT only (port extra) */
    }
    if (r->show_instances && pass == 0) {                          /* list +0x1c0; the additive faces (+0x1cc) wait for the buckets below */
        double a = win_time(); g_last_material = 0xffffffffu;
        for (uint32_t mi = 0; mi < r->ins->nmodels; mi++) {
            Model *m = &r->ins->models[mi];
            for (uint32_t k = 0; k < m->ninstances; k++) {
                Instance *inst = &m->instances[k]; if (!inst->drawn || inst_fading(inst)) continue;   /* a fading instance's opaque parts go to the fade list below */
                static int prof2 = -1; if (prof2 < 0) prof2 = wenv("WOODY_PROF2") != NULL;
                if (!prof2) { draw_instance(r, inst, 0); draw_outline(r, inst); }
                else { static double mt[512]; static int mn; double b0 = win_time(); draw_instance(r, inst, 0); draw_outline(r, inst); if (mi < 512) mt[mi] += win_time() - b0; if (mi == r->ins->nmodels - 1 && k == m->ninstances - 1 && ++mn == 120) { for (uint32_t z = 0; z < r->ins->nmodels && z < 512; z++) if (mt[z] / 120 * 1000 > 0.3) { printf("   model %u: %.2f ms (%u nodes, %u tris, %u inst)", z, mt[z] / 120 * 1000, r->ins->models[z].nnodes, r->ins->models[z].ntris, r->ins->models[z].ninstances); puts(""); } } }
            }
        }
        bt_flush(); g_last_material = 0xffffffffu;
        T[3] += win_time() - a;
    }
    if (pass == 0 && !r->wireframe) gtao_frame(zn, zf, f / aspect, f);   /* port extra (gtao.c): over the opaque image only, when the option is on */
    }
    if (r->show_instances && r->post_models) { r->post_models(r->tex, cam->pos); set_blend(0); g_last_material = 0xffffffffu; }
    if (r->show_instances) {
        /* 0x428d00, run after the transparent world lists: the fade list +0x1c4 and the additive list +0x1cc (every
         * face of a blended group, mode 3 at 0x43d7c8) share 256 depth buckets, drawn from 255 down to 0 (far to near).
         * bucket = round(depth * 254 / deepest) (fistp, [0x4aa2f0] = 254), deepest = max(1, every batch depth) (0x428d27..0x428db8).
         * Per bucket: the fade batches depth-only (ZERO/ONE, ZWRITE on, 0x428f10) and again blended (SRCALPHA/INVSRCALPHA,
         * 0x428fdd), then ZWRITE off (0x42908d), list +0x1c8 (SRCALPHA, the alpha blended world sprites) and list +0x1cc
         * with ONE/ONE (0x429182: the glow faces and the additive world sprites). Bucket 0 merges +0x1c8 and +0x1cc by batch
         * address (0x429240). The sprites are recorded after this function (hud.c), so the buckets are drawn by rnd_sorted.
         * The sort depth is the global [0x5ac8d4], which every model batch copies when a polygon lands in it (0x43e0d3,
         * 0x43eec7, 0x43ea1d); its only writer is 0x43b56a, for an instance that is fading (alpha < 252): camera-space
         * z of its .ins position (row +0x11c/+0x12c/+0x13c/+0x14c against inst+0xc), clamped at 0. A batch holds
         * consecutive polygons of one (texture, mode, instance) (0x43dbac..0x43dbbb), so the sort is per instance, not per
         * face, and an additive batch of an instance that is not fading gets whatever the last fading instance drawn
         * before it left there - 0 (.bss) until the first fade of the session. In the usual case that is bucket 0: the
         * glow faces come after every fade bucket. Among themselves ONE/ONE without z-write commutes, so only their
         * place against the fade list (which writes z) is visible. The additive RGB of a fading instance carries 1 - fade
         * (lit_vertex_colour); the vertex alpha stays 1 here, ONE/ONE ignores it. ALPHATESTENABLE follows the colour
         * key bit for this list too (0x4291c4), and no blended group of the 28 levels has one: off. */
        static float sort_depth;                                     /* [0x5ac8d4], never reset (not even on a level change) */
        double a = win_time();
        if (!r->model_blend) {
            r->model_blend = (uint8_t *)calloc(r->ins->nmodels + 1, 1);
            for (uint32_t mi = 0; mi < r->ins->nmodels; mi++) { const Model *m = &r->ins->models[mi];
                for (uint32_t ni = 0; ni < m->nnodes && !r->model_blend[mi]; ni++) { const InsNode *n = &m->nodes[ni]; if (n->kind != 0 || !n->polys || n->type_code == 2) continue;
                    for (uint32_t k = 0; k < n->npolys; k++) if (n->polys[k].nverts >= 3 && mat_blended(r, n->polys[k].material)) { r->model_blend[mi] = 1; break; } } }
        }
        static Instance **fl, **al; static float *fd, *ad; static uint32_t fcap, acap; uint32_t fn = 0, an = 0; float dmax = 1.0f;
        /* the draw order of 0x42b380: the Perso first (vtbl[2](4 or 6)), then the frame's instance list world+0x64 in its
         * order (vtbl[2](5 or 7), the Perso skipped), because that order decides which fading instance leaves its depth in
         * [0x5ac8d4] for the glow batches after it. The port draws a few instances outside the list (the bomb pool, links
         * an actor draws itself): those come last, in model order. */
        static Instance **ord; static uint32_t ocap; uint32_t on = 0, ntot = 0;
        for (uint32_t mi = 0; mi < r->ins->nmodels; mi++) ntot += r->ins->models[mi].ninstances;
        if (ntot > ocap) { ocap = ntot; ord = (Instance **)realloc(ord, ocap * sizeof *ord); }
        { Instance *perso = r->perso;
          for (uint32_t mi = 0; mi < r->ins->nmodels && !perso; mi++) for (uint32_t k = 0; k < r->ins->models[mi].ninstances; k++) { Instance *q = &r->ins->models[mi].instances[k];
              if (!q->scripted && (q->type == 1 || q->type == 2 || q->type == 3 || q->type == 18 || q->type == 19)) { perso = q; break; } }
          if (perso && perso->drawn) ord[on++] = perso;
          if (r->list_on) for (uint32_t i = 0; i < r->nlist; i++) { Instance *q = r->list[i]; if (q != perso && q->drawn) ord[on++] = q; }
          for (uint32_t mi = 0; mi < r->ins->nmodels; mi++) for (uint32_t k = 0; k < r->ins->models[mi].ninstances; k++) { Instance *q = &r->ins->models[mi].instances[k];
              if (q->drawn && q != perso && !(r->list_on && q->listed)) ord[on++] = q; } }
        for (uint32_t oi = 0; oi < on; oi++) {
            Instance *inst = ord[oi]; uint32_t mi = (uint32_t)(inst->model - r->ins->models); if (inst->type == 60) continue;
            int fading = inst_fading(inst), add = r->model_blend[mi];
            if (!fading && !add) continue;
            if (fn == fcap) { fcap = fcap * 2 + 16; fl = (Instance **)realloc(fl, fcap * sizeof *fl); fd = (float *)realloc(fd, fcap * sizeof *fd); }
            if (an == acap) { acap = acap * 2 + 64; al = (Instance **)realloc(al, acap * sizeof *al); ad = (float *)realloc(ad, acap * sizeof *ad); }
            if (fading) {                                            /* 0x43b528..0x43b56a */
                float d = (inst->position.x - cam->pos.x) * fw.x + (inst->position.y - cam->pos.y) * fw.y + (inst->position.z - cam->pos.z) * fw.z;
                sort_depth = d < 0 ? 0 : d;
            }
            if (fading) { fl[fn] = inst; fd[fn++] = sort_depth; }
            if (add) { al[an] = inst; ad[an++] = sort_depth; }
            if (sort_depth > dmax) dmax = sort_depth;
        }
        (void)dmax;
        g_srt.fl = fl; g_srt.fd = fd; g_srt.fn = fn; g_srt.al = al; g_srt.ad = ad; g_srt.an = an;
        T[3] += win_time() - a;
    }
    g_srt.on = 1;
    set_blend(0);
    g_ndyn = 0; g_idyn_n = 0;                                      /* the dynamic lights of this frame are used up */
    T[4] += win_time() - q0;
    if (wenv("WOODY_PROF") && ++TN == 60) { printf("  RND ms: pose %.2f cull %.2f shadows %.2f instances %.2f total %.2f | world %u/%u tris, %u/%u sectors%s", T[0] / 60 * 1000, T[5] / 60 * 1000, T[2] / 60 * 1000, T[3] / 60 * 1000, T[4] / 60 * 1000, r->drawn_tris, r->total_tris, r->nsec_vis, r->gel->nsectors, r->pvs_on ? " (.vis)" : ""); puts(""); T[0] = T[2] = T[3] = T[4] = T[5] = 0; TN = 0; }
    (void)time_s;
}

/* The sort depth of a world sprite (0x481560): its corners go to view space (the sprite submits them there, flag 0x20) with the
 * side planes at x = +-z and y = +-z (outcodes 1: x > z, 2: -x > z, 4: y > z, 8: -y > z, 0x48170e..0x48176b); all four outside
 * one plane = no batch at all (0x481930); otherwise the planes that any corner is outside of clip the polygon in the order
 * 1, 2, 4, 8 (0x482440 / 0x482690 / 0x4828c0 / 0x482ae0, Sutherland-Hodgman starting at the first vertex: an inside vertex is
 * kept, an edge that crosses adds its intersection), fewer than 3 vertices left = no batch, and batch+0x10 = the view z of the
 * FIRST vertex left (0x481d7f). There is no near plane. Returns 0 for no batch. */
static int sprite_depth(const float v[4][3], float *depth)
{
    float P[2][12][3]; int n = 4, cur = 0, all = 15, any = 0;
    for (int i = 0; i < 4; i++) {
        float d[3] = { v[i][0] - g_srt.pos.x, v[i][1] - g_srt.pos.y, v[i][2] - g_srt.pos.z };
        float x = (d[0] * g_srt.rt.x + d[1] * g_srt.rt.y + d[2] * g_srt.rt.z) * g_srt.sx, y = (d[0] * g_srt.up.x + d[1] * g_srt.up.y + d[2] * g_srt.up.z) * g_srt.sy;
        float z = d[0] * g_srt.fw.x + d[1] * g_srt.fw.y + d[2] * g_srt.fw.z;
        int oc = (x > z ? 1 : 0) | (-x > z ? 2 : 0) | (y > z ? 4 : 0) | (-y > z ? 8 : 0);
        all &= oc; any |= oc; P[0][i][0] = x; P[0][i][1] = y; P[0][i][2] = z;
    }
    if (all) return 0;
    for (int pl = 0; pl < 4; pl++) {
        if (!(any & (1 << pl))) continue;
        int m = 0; const float (*a)[3] = P[cur]; float (*o)[3] = P[cur ^ 1];
        #define SPR_D(q) ((pl == 0 ? (q)[0] : pl == 1 ? -(q)[0] : pl == 2 ? (q)[1] : -(q)[1]) - (q)[2])   /* > 0 = outside */
        for (int i = 0; i < n && m < 11; i++) {
            const float *c = a[i], *nx = a[(i + 1) % n]; float dc = SPR_D(c), dn = SPR_D(nx);
            if (!(dc > 0)) { o[m][0] = c[0]; o[m][1] = c[1]; o[m][2] = c[2]; m++; }
            if ((dc > 0) != (dn > 0) && m < 11) { float t = dc / (dc - dn); for (int k = 0; k < 3; k++) o[m][k] = c[k] + t * (nx[k] - c[k]); m++; }
        }
        #undef SPR_D
        n = m; cur ^= 1;
        if (n < 3) return 0;
    }
    *depth = P[cur][0][2];
    return 1;
}

void rnd_sorted(Renderer *r)
{
    if (!g_srt.on) return;
    g_srt.on = 0;
    double a = win_time();
    /* the world sprites recorded since rnd_frame (hud.c): one batch each on +0x1c8 (alpha blended) or +0x1cc (additive) */
    static float *qd; static int *qb; static uint8_t *qk; static int qcap;
    int nq = r->spr_count && r->spr_quad && r->spr_draw ? r->spr_count() : 0;
    if (nq > qcap) { qcap = nq + 256; qd = (float *)realloc(qd, (size_t)qcap * sizeof *qd); qb = (int *)realloc(qb, (size_t)qcap * sizeof *qb); qk = (uint8_t *)realloc(qk, (size_t)qcap); }
    Instance **fl = g_srt.fl, **al = g_srt.al; const float *fd = g_srt.fd, *ad = g_srt.ad; uint32_t fn = g_srt.fn, an = g_srt.an;
    /* deepest = max(1, every batch depth of +0x1cc, +0x1c8 and +0x1c4) (0x428d27..0x428db8) */
    float dmax = 1.0f;
    for (uint32_t i = 0; i < fn; i++) if (fd[i] > dmax) dmax = fd[i];
    for (uint32_t i = 0; i < an; i++) if (ad[i] > dmax) dmax = ad[i];
    for (int i = 0; i < nq; i++) {
        float v[4][3]; int bl, early; r->spr_quad(i, v, &bl, &early);
        qk[i] = (uint8_t)((bl ? 1 : 0) | (early ? 2 : 0) | (sprite_depth(v, &qd[i]) ? 4 : 0));
        if ((qk[i] & 4) && qd[i] > dmax) dmax = qd[i];
    }
    /* bucket = fistp(depth * 254 / deepest) (0x428dd7 / 0x428e28 / 0x428e89); a negative depth (a corner behind the eye that
     * survived the side planes) would index below the bucket table in the original - the port puts it in bucket 0 */
    static int *fb, *ab; static uint32_t bcap;
    if (fn > bcap || an > bcap) { bcap = (fn > an ? fn : an) + 64; fb = (int *)realloc(fb, bcap * sizeof *fb); ab = (int *)realloc(ab, bcap * sizeof *ab); }
    int bmax = -1;
    for (uint32_t i = 0; i < fn; i++) { fb[i] = (int)lrintf(fd[i] * 254.0f / dmax); if (fb[i] < 0) fb[i] = 0; if (fb[i] > bmax) bmax = fb[i]; }
    for (uint32_t i = 0; i < an; i++) { ab[i] = (int)lrintf(ad[i] * 254.0f / dmax); if (ab[i] < 0) ab[i] = 0; if (ab[i] > bmax) bmax = ab[i]; }
    for (int i = 0; i < nq; i++) { if (!(qk[i] & 4)) { qb[i] = -1; continue; } qb[i] = (int)lrintf(qd[i] * 254.0f / dmax); if (qb[i] < 0) qb[i] = 0; if (qb[i] > bmax) bmax = qb[i]; }
    if (wenv("WOODY_SORTLOG")) {
        static int fr; if (fr++ % 60 == 0) {
            printf("  SORT deepest %.0f | fade %u, glow %u, sprites %d:", dmax, fn, an, nq);
            for (int i = 0; i < nq && i < 40; i++) if (qb[i] >= 0) printf(" %s%d@%.0f", (qk[i] & 1) ? "b" : "a", qb[i], qd[i]);
            puts("");
        }
    }
    /* the sprite records of bucket b: blended (list +0x1c8) or additive (+0x1cc), each in creation order (a source list is LIFO
     * and the bucket split pushes in front again, 0x428dfb..0x428e01, so a bucket holds its batches oldest first) */
    #define SPR_STATE() do { if (!sstate) { sstate = 1; bt_flush(); glEnable(GL_BLEND); glDepthMask(GL_FALSE); glDisable(GL_ALPHA_TEST); glDisable(GL_CULL_FACE); } } while (0)
    for (int b = bmax; b >= 0; b--) {
        int sstate = 0;
        if (fn && r->show_instances) {
            g_zfunc = GL_LEQUAL; glDepthFunc(GL_LEQUAL);
            for (int sub = 1; sub <= 2; sub++) {
                int any = 0;
                for (uint32_t i = 0; i < fn; i++) {
                    if (fb[i] != b) continue;
                    if (!any) { any = 1; g_fading = sub; g_last_material = 0xffffffffu; if (sub == 1) glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE); }
                    g_fade_alpha = 1.0f - fl[i]->fade;
                    draw_instance(r, fl[i], 0); draw_outline(r, fl[i]);
                }
                if (!any) break;
                bt_flush(); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            }
            g_fading = 0; g_fade_alpha = 1.0f; g_zfunc = GL_LESS; glDepthFunc(GL_LESS);
        }
        /* then ZWRITE off (0x42908d) and, for b > 0, all of +0x1c8 (SRCALPHA/INVSRCALPHA, 0x4290bf) before all of +0x1cc
         * (ONE/ONE, 0x429182). Bucket 0 (0x429240) takes the two lists by batch address, i.e. in creation order: the halos of the
         * instance Updates (0x42b400), the model batches of the world draw (0x42b380), then the Perso and the effects (0x46d040). */
        for (int pass = 0; pass < 3; pass++) {
            if (pass == 1) {                                         /* the glow faces of this bucket (ONE/ONE commutes among them) */
                int any = 0;
                if (r->show_instances) for (uint32_t i = 0; i < an; i++) {
                    if (ab[i] != b) continue;
                    if (sstate) { r->spr_done(); sstate = 0; }
                    if (!any) { any = 1; g_last_material = 0xffffffffu; glDisable(GL_ALPHA_TEST); }
                    draw_instance(r, al[i], 1);
                }
                if (any) { bt_flush(); glEnable(GL_ALPHA_TEST); }
                continue;
            }
            for (int i = 0; i < nq; i++) {
                if (qb[i] != b) continue;
                int key = b > 0 ? (qk[i] & 1) : (qk[i] & 2);          /* b > 0: pass 0 = blended, 2 = additive; b = 0: pass 0 = early, 2 = late */
                if (pass == 0 ? !key : key) continue;
                SPR_STATE(); r->spr_draw(i);
            }
        }
        if (sstate) r->spr_done();
    }
    #undef SPR_STATE
    if (nq && r->spr_done) r->spr_done();
    g_last_material = 0xffffffffu; glEnable(GL_ALPHA_TEST); glDisable(GL_BLEND); glDepthMask(GL_TRUE);
    T[3] += win_time() - a; T[4] += win_time() - a;
}

void rnd_set_race(Renderer *r, const Trajectory *path)
{
    if (!path || !path->npoints) { for (int k = 0; k < 6; k++) r->race[k] = -1; }
    else gel_race_regions(r->gel, path->points, path->npoints, r->race);   /* 0x455f3d..0x455fed -> renderer+0xc0..+0xd4 */
    r->sec_dirty = 1; r->race_entry = NULL;
    if (wenv("WOODY_RACEVISLOG")) printf("  RACEVIS region list %d %d %d %d %d (%u points)\n", r->race[0], r->race[1], r->race[2], r->race[3], r->race[4], path ? path->npoints : 0);
    if (path && wenv("WOODY_RACEVISLOG")) for (uint32_t i = 0; i < path->npoints; i++) { Vec3 q = path->points[i];
        int32_t f = gel_floor_poly(r->gel, q); const float *pl = f >= 0 ? r->gel->polys[f].plane : NULL;
        printf("    point %2u (%.0f %.0f %.0f): floor group %d at y %.0f\n", i, q.x, q.y, q.z, gel_floor_group(r->gel, q),
               pl && pl[1] > 0 ? -(pl[0] * q.x + pl[2] * q.z + pl[3]) / pl[1] : q.y); }
}
void rnd_set_sky(Renderer *r, const uint32_t tex[5]) { if (r->have_sky) for (int f = 0; f < 5; f++) if (tex[f]) r->sky_tex[f] = tex[f]; }
