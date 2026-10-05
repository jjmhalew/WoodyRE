/* texup.c - the image steps around an AI upscaler for texture packs (make_hd_textures.bat, docs/TEXTURES.md 4).
 *
 *   texup pre  <dump dir> <work dir> <pack dir>   every texture of mods\dump\*\ once (by hash), not yet in the pack
 *                                                 (exit code 3 when there is none):
 *        work\in\<name>.png        the colour, opaque, with a margin of half its size on every side
 *        work\in\<name>.alpha.png  the alpha as grey, same margin (only for textures with transparency)
 *   (the upscaler turns work\in into work\out, same names, any whole factor)
 *   texup post <dump dir> <work dir> <pack dir>   crops the margin off again and writes <pack dir>\<name>.png
 *
 * Why the margin: an upscaler treats the image border as the end of the picture, so a texture that repeats (floors, walls)
 * gets a seam where it meets its own copy. Opaque textures get their own opposite edges as margin (wrap), textures with
 * transparency (sprites, HUD, colour-keyed leaves) the edge texels repeated (clamp). Why the alpha goes separately: the
 * colour under transparent texels is black in the dump, which an upscaler would smear into the edges; the colour is
 * first bled outwards from the opaque texels, and the alpha comes back from its own upscale - cut at 128 again where the
 * original was on/off only (the game's colour key, TP_KEY), as is where it had soft alpha (bank images). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#define mkdir_(p) _mkdir(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#define mkdir_(p) mkdir(p, 0755)
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../../src/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../src/stb/stb_image_write.h"

typedef struct { char path[600], name[64]; } Tex;
static Tex *g_tex; static int g_n, g_cap;

static int is_texture(const char *name)                 /* <w>x<h>_<16 hex>.png, as --dumptex names them */
{
    size_t n = strlen(name); int w, h;
    if (n < 25 || n >= 64 || strcmp(name + n - 4, ".png") || name[n - 21] != '_' || sscanf(name, "%dx%d_", &w, &h) != 2) return 0;
    for (size_t i = n - 20; i < n - 4; i++) if (!strchr("0123456789abcdefABCDEF", name[i])) return 0;
    return 1;
}
static void add(const char *dir, const char *name)
{
    for (int i = 0; i < g_n; i++) if (!strcmp(g_tex[i].name, name)) return;   /* the same texture in another level */
    if (g_n == g_cap) { g_cap = g_cap ? g_cap * 2 : 1024; g_tex = (Tex *)realloc(g_tex, (size_t)g_cap * sizeof *g_tex); }
    snprintf(g_tex[g_n].path, sizeof g_tex[g_n].path, "%s/%s", dir, name); snprintf(g_tex[g_n].name, sizeof g_tex[g_n].name, "%s", name); g_n++;
}
#ifdef _WIN32
static void scan(const char *dir, int depth)
{
    char pat[600]; WIN32_FIND_DATAA fd; snprintf(pat, sizeof pat, "%s/*", dir);
    HANDLE h = FindFirstFileA(pat, &fd); if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        char p[600]; snprintf(p, sizeof p, "%s/%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 2) scan(p, depth + 1); }
        else if (is_texture(fd.cFileName)) add(dir, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
#else
static void scan(const char *dir, int depth)
{
    DIR *d = opendir(dir); struct dirent *de; if (!d) return;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[600]; struct stat st; snprintf(p, sizeof p, "%s/%s", dir, de->d_name); if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 2) scan(p, depth + 1); }
        else if (is_texture(de->d_name)) add(dir, de->d_name);
    }
    closedir(d);
}
#endif
static int exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }
static void mkdirs(const char *path)                    /* every folder of the path (mods\textures\hd on a fresh install) */
{
    char p[600]; snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++) if (*c == '/' || *c == '\\') { char k = *c; *c = 0; mkdir_(p); *c = k; }
    mkdir_(p);
}
static void stem(const char *name, char *out, size_t n) { snprintf(out, n, "%.*s", (int)(strlen(name) - 4), name); }

/* the transparent texels take the average colour of their opaque (or already filled) neighbours, ring by ring */
static void bleed(uint8_t *px, int w, int h)
{
    uint8_t *done = (uint8_t *)malloc((size_t)w * h), *next = (uint8_t *)malloc((size_t)w * h); int left = 0;
    for (int i = 0; i < w * h; i++) { done[i] = px[i * 4 + 3] >= 128; left += !done[i]; }
    for (int pass = 0; left && pass < w + h; pass++) {
        memcpy(next, done, (size_t)w * h);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            int i = y * w + x; if (done[i]) continue;
            unsigned r = 0, g = 0, b = 0, c = 0;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int xx = x + dx, yy = y + dy; if (xx < 0 || yy < 0 || xx >= w || yy >= h || !done[yy * w + xx]) continue;
                const uint8_t *q = px + (yy * w + xx) * 4; r += q[0]; g += q[1]; b += q[2]; c++;
            }
            if (c) { px[i * 4] = (uint8_t)(r / c); px[i * 4 + 1] = (uint8_t)(g / c); px[i * 4 + 2] = (uint8_t)(b / c); next[i] = 1; left--; }
        }
        memcpy(done, next, (size_t)w * h);
    }
    free(done); free(next);
}
/* channel c of px (or the colour, c < 0) into an RGB image with margin pw / ph, wrapped or clamped */
static uint8_t *padded(const uint8_t *px, int w, int h, int pw, int ph, int wrap, int c)
{
    int W = w + 2 * pw, H = h + 2 * ph; uint8_t *o = (uint8_t *)malloc((size_t)W * H * 3);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int sx = x - pw, sy = y - ph;
        if (wrap) { sx = ((sx % w) + w) % w; sy = ((sy % h) + h) % h; }
        else { sx = sx < 0 ? 0 : sx >= w ? w - 1 : sx; sy = sy < 0 ? 0 : sy >= h ? h - 1 : sy; }
        const uint8_t *s = px + ((size_t)sy * w + sx) * 4; uint8_t *d = o + ((size_t)y * W + x) * 3;
        if (c < 0) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; } else d[0] = d[1] = d[2] = s[c];
    }
    return o;
}

static int pre(const char *work, const char *pack)
{
    char in[600], p[700], st[64]; int todo = 0;
    snprintf(in, sizeof in, "%s/in", work); mkdirs(in);
    for (int i = 0; i < g_n; i++) {
        snprintf(p, sizeof p, "%s/%s", pack, g_tex[i].name); if (exists(p)) continue;   /* already in the pack */
        int w, h, ch; uint8_t *px = stbi_load(g_tex[i].path, &w, &h, &ch, 4); if (!px) { printf("cannot read %s\n", g_tex[i].path); continue; }
        int alpha = 0; for (int k = 0; k < w * h; k++) if (px[k * 4 + 3] < 255) { alpha = 1; break; }
        int pw = (w + 1) / 2, ph = (h + 1) / 2; stem(g_tex[i].name, st, sizeof st);
        if (alpha) bleed(px, w, h);
        uint8_t *o = padded(px, w, h, pw, ph, !alpha, -1);
        snprintf(p, sizeof p, "%s/%s.png", in, st);
        if (!stbi_write_png(p, w + 2 * pw, h + 2 * ph, 3, o, (w + 2 * pw) * 3)) { printf("cannot write %s\n", p); free(o); stbi_image_free(px); return 1; }
        free(o);
        if (alpha) { o = padded(px, w, h, pw, ph, 0, 3); snprintf(p, sizeof p, "%s/%s.alpha.png", in, st); stbi_write_png(p, w + 2 * pw, h + 2 * ph, 3, o, (w + 2 * pw) * 3); free(o); }
        stbi_image_free(px); todo++;
    }
    printf("%d textures, %d new ones prepared in %s\n", g_n, todo, in);
    return todo ? 0 : 3;                                  /* 3 = the pack already has them all */
}
static int post(const char *work, const char *pack)
{
    char p[700], st[64]; int done = 0, miss = 0, bad = 0; mkdirs(pack);
    for (int i = 0; i < g_n; i++) {
        snprintf(p, sizeof p, "%s/%s", pack, g_tex[i].name); if (exists(p)) continue;
        int w, h, ch; uint8_t *src = stbi_load(g_tex[i].path, &w, &h, &ch, 4); if (!src) continue;
        int pw = (w + 1) / 2, ph = (h + 1) / 2, binary = 1, alpha = 0;
        for (int k = 0; k < w * h; k++) { uint8_t a = src[k * 4 + 3]; if (a < 255) alpha = 1; if (a && a < 255) binary = 0; }
        stbi_image_free(src); stem(g_tex[i].name, st, sizeof st);
        int W, H, c2; snprintf(p, sizeof p, "%s/out/%s.png", work, st);
        uint8_t *up = stbi_load(p, &W, &H, &c2, 3); if (!up) { miss++; continue; }
        int s = W / (w + 2 * pw); if (s < 1 || W != s * (w + 2 * pw) || H != s * (h + 2 * ph)) { printf("odd size %s\n", p); stbi_image_free(up); continue; }
        uint8_t *am = NULL; int AW = 0, AH = 0;
        if (alpha) { snprintf(p, sizeof p, "%s/out/%s.alpha.png", work, st); am = stbi_load(p, &AW, &AH, &c2, 1); if (!am || AW != W || AH != H) { if (am) stbi_image_free(am); stbi_image_free(up); miss++; continue; } }
        int ow = w * s, oh = h * s; uint8_t *o = (uint8_t *)malloc((size_t)ow * oh * 4);
        for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
            size_t si = (size_t)(y + ph * s) * W + (x + pw * s); uint8_t *d = o + ((size_t)y * ow + x) * 4;
            d[0] = up[si * 3]; d[1] = up[si * 3 + 1]; d[2] = up[si * 3 + 2];
            d[3] = !alpha ? 255 : binary ? (am[si] >= 128 ? 255 : 0) : am[si];
        }
        snprintf(p, sizeof p, "%s/%s", pack, g_tex[i].name);
        int ok;
        if (!alpha) { uint8_t *rgb = (uint8_t *)malloc((size_t)ow * oh * 3); for (int k = 0; k < ow * oh; k++) memcpy(rgb + k * 3, o + k * 4, 3); ok = stbi_write_png(p, ow, oh, 3, rgb, ow * 3); free(rgb); }
        else ok = stbi_write_png(p, ow, oh, 4, o, ow * 4);
        free(o); stbi_image_free(up); if (am) stbi_image_free(am);
        if (ok) done++; else { if (!bad++) printf("cannot write %s\n", p); }
    }
    printf("%d textures written to %s\n", done, pack);
    if (bad) { printf("%d textures could not be written\n", bad); return 1; }
    if (miss) printf("%d textures had no upscaled image: did the upscaler run? (the work folder is kept)\n", miss);
    return miss ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc != 5 || (strcmp(argv[1], "pre") && strcmp(argv[1], "post"))) { fprintf(stderr, "usage: texup pre|post <dump dir> <work dir> <pack dir>\n"); return 2; }
    scan(argv[2], 0);
    if (!g_n) { fprintf(stderr, "no textures in %s (run the game once with --dumptex all)\n", argv[2]); return 1; }
    return !strcmp(argv[1], "pre") ? pre(argv[3], argv[4]) : post(argv[3], argv[4]);
}
