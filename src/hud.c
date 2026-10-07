/* hud.c - 2D layer: font (rck type 3), strings (type 2), HUD sprites (bank 0 images 61..64), text box 1080.
 * Everything in 640x480 virtual coordinates, origin top left. Colours are 0xAARRGGBB with RGB 0x80 = 1.0 (docs/HUD_TEXT.md 5.2). */
#include "hud.h"
#include "texpack.h"
#include "plat.h"
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

typedef struct { float w, adv; uint16_t x, y, wpx, hpx, page, junk; } Glyph;

static struct {
    int ok;
    GLuint img[4]; int img_w[4], img_h[4];                /* bank 0 images 61..64 */
    uint32_t nglyphs, npages, psize; float H, B, M; Glyph *gl; GLuint page[8];
    uint16_t **str; int nstr;                             /* bank 0 strings, 0-terminated u16 codes */
    float k;                                              /* current glyph scale = size / (H - B) */
    float blink;
    GLuint sky[5]; int nlevel_img;                        /* level bank images 0..4 in file row order (sky cube) */
    GLuint fx[40];                                        /* bank 0 effect images, slot = the index of the image in k_fx_img (fx_slot): 0, 4, 6 ribbon, flash, bolt (docs/PROJECTILES.md); 5, 10, 11 glow and the two death stars (docs/PERSO_DEATH.md 7); 12, 14, 31, 32 explosion flash, smoke, flame, exhaust glow (docs/ROCKET.md 5, PROJECTILES.md 5.3); 58 the wake on the water (docs/WATER.md 4.1); 18, 24 the fuse spark and the bomb smoke (docs/BOMB.md 3.4, 4.3); 57 the splash drop (docs/SPLASH.md 4); 7, 8, 9 the hit star; 33 the fire ring of the special attack (docs/PERSO_SPECIAL.md 3); 30 the storm bolt (docs/STORM.md 5); 13 the fireball spark (docs/PROJECTILES.md 5.5); 15, 16, 17 dust clouds, 25 wood splinter, 26 peck hole, 68, 69 snow print (docs/PARTICLES.md); 34..43 the skeleton of the lightning death (docs/PERSO_DEATH.md 4.2) */
    GLuint beam;                                          /* bank 0 image 1: the line texture */
    GLuint bonus[5]; float sr[3], su[3];                  /* bank 0 images 19, 21, 20, 46, 23 (jump table 0x479654) */
    GLuint env[4];                                        /* bank 0 images 53..56: the butterflies of the environment instances (0x47e050 picks one of the four) */
    GLuint bub[9];                                        /* bank 0 images 44..52: the speech bubble and its contents (0x478980); 46 is bonus[3] */
    GLuint logo; int logo_w, logo_h; float logo_v, menu_t;   /* level bank image 1 (the title logo in House.rck); fade value 0..5 */
    GLuint sheet; int sheet_w, sheet_h;                   /* level bank image 0 (House and the three hubs carry the same one): the save-slot panel, ring and cross */
    GLuint sheet2; int sheet2_w, sheet2_h;                /* level bank image 2: in House the clock / enemy-face sheet (= image 1 of the hubs), the column heads of page 4 */
    GLuint limg[16]; int limg_w[16], limg_h[16];          /* level bank images 0..15 as the 2D blit sees them (surfaces [0x5e8674]): the credits (page 0x20) draw 2..12 */
    uint16_t **lstr; int nlstr;                           /* level bank strings (refs 0x0102xxxx): the 252 names of Credits.rck */
    struct { int state, n; float t, size; uint32_t id[3]; float x[3], y[3]; float rect[4]; } box;
    float iris_kx, iris_ky;                               /* hud_iris: virtual units per round pixel on this window (1, 1 at 4:3) */
    float vx0, vx1;                                       /* the virtual x range the viewport shows: 0..640 at 4:3, wider on a wide view (docs/DISPLAY.md 3) */
    struct { uint32_t n, npages, psize; Glyph *gl; GLuint page[8]; } cf;   /* port extra: the Credits font (port_encode) */
} H;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* port-only text needs ASCII -> glyph code, but every language build generates its own font (glyph order = first
 * occurrence in its text, docs/HUD_TEXT.md 1.5; the Brazilian and Polish fonts even differ per level). The letters are
 * recognised by shape instead: FNV-1a of (w, h, alpha of the glyph cell) is identical for a letter in every font of the
 * English, Brazilian, Polish, Spanish and Russian CDs (tools/glyphmatch.py). Table = the English fonts' letters (incl. the
 * Credits extras), then k_shape_ext = the letters only the other CDs' fonts have (Unicode; identified by eye from glyph
 * sheets and checked by decoding each CD's own menu strings). The Cyrillic letters that look Latin (a, e, o, p, c, y, x,
 * A, B, E, H, M, O, P, C, T, X) ARE the Latin glyphs in the Russian fonts. */
static const struct { uint32_t h; uint16_t c; } k_shape_ext[] = {
        {0x86276e1bU,0x015b},{0xfdfdbc79U,0x0144},{0x5666a3beU,0x017b},{0x8900511aU,0x0119},{0x2fce737cU,0x0105},
        {0xfbab45f2U,0x0107},{0x2df9f308U,0x0142},{0x3228e4bdU,0x00f3},{0x4fa03bdaU,0x017a},{0x02ea0011U,0x0143},
        {0x8b36cff3U,0x0118},{0xb5117256U,0x00ae},{0x2b1272a5U,0x00e9},{0xd2bb5ef8U,0x00f4},{0x81d22146U,0x00e7},
        {0xbd5a61bfU,0x00b0},{0x731fea5aU,0x00e3},{0x72e1356bU,0x00cd},{0x278365dcU,0x00c7},{0xafee5546U,0x00c3},
        {0x0ab9e6e7U,0x00d4},{0xa3de7374U,0x00ea},{0x7b186affU,0x00f5},{0x3f2e3aafU,0x00fa},{0x48182390U,0x00ed},
        {0x6cea7f59U,0x00d5},{0xafaf7954U,0x00e1},{0x915e3bedU,0x00bf},{0x10ecf937U,0x00a1},{0x8d5f3955U,0x00c1},
        {0x509165fcU,0x00d3},{0xc26cb107U,0x00f1},{0x9db45e9bU,0x20ac},{0xacade00aU,0x044b},{0x294e3784U,0x0434},
        {0xaa244bdeU,0x043c},{0x35f25aa4U,0x043b},{0x9c76abb0U,0x041f},{0x0ded02c2U,0x0436},{0xe2bad563U,0x0438},
        {0x2050866fU,0x0442},{0x34473173U,0x044c},{0x73fd9206U,0x0414},{0x1db58ebcU,0x0413},{0xf954a73cU,0x0417},
        {0xf75be658U,0x0423},{0x04972acdU,0x041b},{0xbcea50bcU,0x042c},{0xcd33974aU,0x042b},{0xca016bacU,0x041a},
        {0xcdb678a6U,0x0447},{0xb61f10e8U,0x043d},{0xf57c4af6U,0x043a},{0xe8309ed2U,0x043f},{0x670e12fcU,0x0432},
        {0x571458fbU,0x044f},{0x4f7f3711U,0x0433},{0xc4d7cc47U,0x0437},{0x0ca9d2e5U,0x0431},{0xaaae36d6U,0x0418},
        {0x5f95ed44U,0x0429},{0x26186795U,0x0411},{0x9eaf35dbU,0x0448},{0x4c68ac46U,0x0439},{0x7b570485U,0x0427},
        {0xf17edf7dU,0x0426},{0xaafff1b3U,0x044e},{0x7ab3bf42U,0x044d},{0x6a25a67bU,0x0449},{0x7db0f09bU,0x042f},
        {0x195800a9U,0x0446},{0x3a80e868U,0x0419},{0x54dde26fU,0x0424},{0x2522e9e2U,0x0444} };
#define NLETTERS (128 + (int)(sizeof k_shape_ext / sizeof *k_shape_ext))
static int letter_slot(unsigned cp)                                   /* Unicode -> index into g_chr / g_cchr, -1 = no font has it */
{
    if (cp < 128) return (int)cp;
    for (int i = 0; i < NLETTERS - 128; i++) if (k_shape_ext[i].c == cp) return 128 + i;
    return -1;
}
enum { LANG_EN, LANG_PL, LANG_ES, LANG_PT, LANG_RU };
static int g_lang;                                                      /* port-only text's language (hud_load, hud_tr) */
static uint16_t g_chr[NLETTERS], g_cchr[NLETTERS]; static unsigned g_font_gen = 1;   /* letter -> code in the level / Credits font (0 = none); gen bumped per font */
static void font_letters(const uint8_t *d, const Glyph *gl, uint32_t n, uint32_t npages, uint32_t psize, uint16_t *chr)
{
    static const struct { uint32_t h; char c; } k_shape[] = {
        {0x98d3b8a0U,'0'},{0xbdc65688U,'1'},{0xb32459d0U,'2'},{0xbe5f8a37U,'3'},{0xa4070e8eU,'4'},{0x4554e012U,'5'},
        {0xa0f43f6aU,'6'},{0x8227416bU,'7'},{0x971a6192U,'8'},{0x942e419bU,'9'},{0x07a57639U,'Q'},{0xaf9b065fU,'u'},
        {0x48b78efcU,'i'},{0x9bb178edU,'t'},{0x7a283415U,'A'},{0x5411533aU,'r'},{0x84a581b5U,'e'},{0xbda078f4U,' '},
        {0x619b970aU,'y'},{0xb815af2dU,'o'},{0x90ec69f5U,'s'},{0x6ec0d998U,'?'},{0x4dcf38deU,'C'},{0xf14db5a9U,'n'},
        {0x56ff844cU,'Y'},{0xf876f87aU,'N'},{0x25ce3a12U,'%'},{0x4ce737c6U,'='},{0x0ce8cf45U,'/'},{0x449ad688U,':'},
        {0xac1e3713U,'+'},{0xc510446cU,'L'},{0x6c39dfb3U,'E'},{0xfe34c2afU,'R'},{0xb167b083U,'D'},{0x65b746c7U,'!'},
        {0x01263862U,'S'},{0xbbda1c2dU,'U'},{0xd3c01bceU,'T'},{0x708c91f0U,'O'},{0x53667e38U,'K'},{0x43d6dac7U,'B'},
        {0x7ddf8dd0U,'H'},{0xb6363acbU,'I'},{0x54ec587bU,'G'},{0xab8811acU,'F'},{0x6d8e1fe4U,'a'},{0x158dce47U,'g'},
        {0x507c4e9fU,'P'},{0x8b7fccbaU,'k'},{0x94cae43eU,'w'},{0x6b93ea1fU,'m'},{0x3b50ebcaU,'d'},{0x916663e4U,'l'},
        {0x8211d902U,'c'},{0xf09d07b5U,'v'},{0x0b017d81U,'W'},{0xe55c457bU,'p'},{0xd26489e0U,'V'},{0x20c16e73U,'X'},
        {0xf4d28980U,'M'},{0xdcc09bbfU,'f'},{0x7e88bf88U,'.'},{0xee3d9bd3U,'h'},{0x9fbe2719U,'('},{0xa30cd199U,')'},
        {0xdcef9392U,'q'},{0x3c1b961bU,'\''},{0x342b26adU,'b'},{0xff2804eeU,','},{0x1f5dcb64U,'$'},{0x884225bcU,'z'},
        {0x2df4cf1dU,'J'},{0x2a8cfd0bU,'x'},{0x36ecd6abU,'&'},{0x4fcbe4a0U,'-'},{0x5e5cd4d0U,'j'},{0x465e8f0cU,'"'},
        {0xba8de7c3U,'Z'} };
    memset(chr, 0, NLETTERS * sizeof *chr); g_font_gen++;
    const uint8_t *pix = d + 0x1c + n * 20;
    for (uint32_t i = 0; i < n; i++) {
        const Glyph *g = &gl[i]; if (g->page >= npages || g->x + g->wpx > psize || g->y + g->hpx > psize) continue;
        const uint8_t *pg = pix + (size_t)g->page * psize * psize * 4;
        uint32_t h = 0x811c9dc5u; h = (h ^ (uint8_t)g->wpx) * 0x01000193u; h = (h ^ (uint8_t)g->hpx) * 0x01000193u;
        for (int y = 0; y < g->hpx; y++) for (int x = 0; x < g->wpx; x++) h = (h ^ pg[((size_t)(g->y + y) * psize + g->x + x) * 4 + 3]) * 0x01000193u;
        for (size_t k = 0; k < sizeof k_shape / sizeof *k_shape; k++)
            if (k_shape[k].h == h) { if (!chr[(int)k_shape[k].c]) chr[(int)k_shape[k].c] = (uint16_t)(i + 1); break; }
        for (int k = 0; k < NLETTERS - 128; k++)
            if (k_shape_ext[k].h == h) { if (!chr[128 + k]) chr[128 + k] = (uint16_t)(i + 1); break; }
    }
}

/* walks an RKET bank (docs/RCK.md): calls cb(type, index, payload, size) for the wanted types; payloads of other types are skipped */
typedef void (*ItemCb)(int type, int index, const uint8_t *data, uint32_t size);
static int rck_walk(const char *path, unsigned want_mask, ItemCb cb)
{
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    uint8_t h[0x38];
    if (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "RKET", 4)) { fclose(f); return -1; }
    for (int type = 0; type < 4; type++) {
        uint32_t count = rd32(h + 8 + 0x18 + type * 4);
        for (uint32_t i = 0; i < count; i++) {
            uint8_t ih[8]; if (fread(ih, 1, 8, f) != 8) { fclose(f); return -1; }
            uint32_t size = rd32(ih);
            if (want_mask & (1u << type)) {
                uint8_t *d = malloc(size ? size : 1);
                if (fread(d, 1, size, f) != size) { free(d); fclose(f); return -1; }
                cb(type, (int)i, d, size); free(d);
            } else fseek(f, (long)size, SEEK_CUR);
        }
    }
    fclose(f); return 0;
}

static GLuint upload(const uint8_t *rgba, int w, int h)
{
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    uint64_t hash = tp_hash('I', rgba, (uint32_t)(w * h * 4), w, h);    /* port extra: a texture pack's PNG (texpack.c); w, h stay the original's for the layout */
    if (tp_replace(hash, TP_ASIS)) return t;
    tp_dump(hash, rgba, w, h);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

/* the bank 0 images the world effects use, loaded into H.fx in this order (the 16 of slot 10 used to be the footstep
 * mark of the reconstruction; 0x47cba0 is decompiled now, docs/PARTICLES.md 2) */
static const unsigned char k_fx_img[40] = { 0, 4, 6, 5, 10, 11, 12, 14, 31, 32, 16, 0x3a, 18, 24, 57, 7, 8, 9, 33, 30, 13,
                                            15, 17, 25, 26, 68, 69, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 255, 255, 29 };   /* 29: the motes of class 90 mode 1 (docs/AMBIENT.md) */
static int fx_slot(int image) { for (int i = 0; i < 40; i++) if (k_fx_img[i] == image) return i; return -1; }
static void common_item(int type, int index, const uint8_t *d, uint32_t size)
{
    static const int bonus_img[5] = { 19, 21, 20, 46, 23 };
    if (type == 1 && (index == 0 || index == 4 || index == 6 || index == 0x3a) && getenv("WOODY_FXLOG") && size >= 8) { int w = d[0] | d[1] << 8, h = d[2] | d[3] << 8; unsigned long sum = 0, sa = 0; for (int i = 0; i < w * h; i++) { sum += d[8 + i * 4] + d[9 + i * 4] + d[10 + i * 4]; sa += d[11 + i * 4]; } printf("fx image %d: %dx%d bpp %d mean rgb %.1f mean a %.1f", index, w, h, d[4], sum / (3.0 * w * h), sa / (1.0 * w * h)), puts(""); }
    if (type == 1 && fx_slot(index) >= 0) { GLuint keep = H.img[0]; int kw = H.img_w[0], kh = H.img_h[0]; H.img[0] = 0; common_item(1, 61, d, size); H.fx[fx_slot(index)] = H.img[0]; H.img[0] = keep; H.img_w[0] = kw; H.img_h[0] = kh; return; }
    if (type == 1 && index == 1) { GLuint keep = H.img[0]; int kw = H.img_w[0], kh = H.img_h[0]; H.img[0] = 0; common_item(1, 61, d, size); H.beam = H.img[0]; H.img[0] = keep; H.img_w[0] = kw; H.img_h[0] = kh; return; }
    if (type == 1 && index >= 44 && index <= 52 && index != 46) { GLuint keep = H.img[0]; int kw = H.img_w[0], kh = H.img_h[0]; H.img[0] = 0; common_item(1, 61, d, size); H.bub[index - 44] = H.img[0]; H.img[0] = keep; H.img_w[0] = kw; H.img_h[0] = kh; return; }
    if (type == 1 && index >= 53 && index <= 56) { GLuint keep = H.img[0]; int kw = H.img_w[0], kh = H.img_h[0]; H.img[0] = 0; common_item(1, 61, d, size); H.env[index - 53] = H.img[0]; H.img[0] = keep; H.img_w[0] = kw; H.img_h[0] = kh; return; }
    if (type == 1) for (int b = 0; b < 5; b++) if (index == bonus_img[b]) { GLuint keep = H.img[0]; int kw = H.img_w[0], kh = H.img_h[0]; H.img[0] = 0; common_item(1, 61, d, size); H.bonus[b] = H.img[0]; H.img[0] = keep; H.img_w[0] = kw; H.img_h[0] = kh; return; }
    if (type == 1 && index >= 61 && index <= 64 && size >= 8) {      /* i16 w, h; u16 bpp, alpha; BGRA, bottom row first (0x480780) */
        int w = (int16_t)(d[0] | d[1] << 8), h = (int16_t)(d[2] | d[3] << 8), bpp = d[4] | d[5] << 8;
        if (w <= 0 || h <= 0 || size < 8 + (uint32_t)w * h * 4) return;
        uint8_t *px = malloc((size_t)w * h * 4);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            const uint8_t *s = d + 8 + ((size_t)(h - 1 - y) * w + x) * 4; uint8_t *o = px + ((size_t)y * w + x) * 4;
            o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = bpp == 24 ? 255 : s[3];
        }
        H.img[index - 61] = upload(px, w, h); H.img_w[index - 61] = w; H.img_h[index - 61] = h; free(px);
    } else if (type == 2) {
        H.str = realloc(H.str, (size_t)(index + 1) * sizeof *H.str); H.nstr = index + 1;
        uint32_t n = size / 2; uint16_t *s = calloc(n + 1, 2); memcpy(s, d, (size_t)n * 2); H.str[index] = s;
    }
}

static void common_item(int type, int index, const uint8_t *d, uint32_t size);
static void level_item(int type, int index, const uint8_t *d, uint32_t size)
{
    if (type == 1) { H.nlevel_img = index + 1; }
    if (type == 1 && index < 16) { GLuint keep = H.img[0]; int kw = H.img_w[0], kh = H.img_h[0]; H.img[0] = 0; common_item(1, 61, d, size); H.limg[index] = H.img[0]; H.limg_w[index] = H.img_w[0]; H.limg_h[index] = H.img_h[0]; H.img[0] = keep; H.img_w[0] = kw; H.img_h[0] = kh; }
    if (type == 2) {                                                 /* level strings (0x43f440), the same pool format as bank 0 */
        H.lstr = realloc(H.lstr, (size_t)(index + 1) * sizeof *H.lstr); H.nlstr = index + 1;
        uint32_t n = size / 2; uint16_t *s = calloc(n + 1, 2); memcpy(s, d, (size_t)n * 2); H.lstr[index] = s; return;
    }
    if (type == 1 && index < 5 && size >= 8) {                       /* raw row order: file row 0 = v 0 = bottom of the cube face (docs/SKY.md) */
        int w = (int16_t)(d[0] | d[1] << 8), h = (int16_t)(d[2] | d[3] << 8);
        if (w > 0 && h > 0 && size >= 8 + (uint32_t)w * h * 4) {
            uint8_t *px = malloc((size_t)w * h * 4);
            for (int i = 0; i < w * h; i++) { px[i * 4] = d[8 + i * 4 + 2]; px[i * 4 + 1] = d[8 + i * 4 + 1]; px[i * 4 + 2] = d[8 + i * 4]; px[i * 4 + 3] = 255; }
            H.sky[index] = upload(px, w, h); free(px);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        }
    }
    if (type == 1 && index == 0) { GLuint keep = H.img[0]; int kw = H.img_w[0], kh = H.img_h[0]; H.img[0] = 0; common_item(1, 61, d, size); H.sheet = H.img[0]; H.sheet_w = H.img_w[0]; H.sheet_h = H.img_h[0]; H.img[0] = keep; H.img_w[0] = kw; H.img_h[0] = kh; }
    if (type == 1 && index == 1) { GLuint keep = H.img[0]; int kw = H.img_w[0], kh = H.img_h[0]; H.img[0] = 0; common_item(1, 61, d, size); H.logo = H.img[0]; H.logo_w = H.img_w[0]; H.logo_h = H.img_h[0]; H.img[0] = keep; H.img_w[0] = kw; H.img_h[0] = kh; return; }
    if (type == 1 && index == 2) { GLuint keep = H.img[0]; int kw = H.img_w[0], kh = H.img_h[0]; H.img[0] = 0; common_item(1, 61, d, size); H.sheet2 = H.img[0]; H.sheet2_w = H.img_w[0]; H.sheet2_h = H.img_h[0]; H.img[0] = keep; H.img_w[0] = kw; H.img_h[0] = kh; }
    if (type != 3 || index != 0 || size < 0x1c) return;              /* font 0x01030000 (0x43f9a0) */
    H.nglyphs = rd32(d); H.npages = rd32(d + 4); H.psize = rd32(d + 8);
    memcpy(&H.H, d + 0xc, 4); memcpy(&H.B, d + 0x14, 4); memcpy(&H.M, d + 0x18, 4);
    if (H.npages > 8 || size < 0x1c + H.nglyphs * 20 + H.npages * H.psize * H.psize * 4) { H.nglyphs = 0; return; }
    H.gl = malloc(H.nglyphs * sizeof *H.gl); memcpy(H.gl, d + 0x1c, H.nglyphs * 20);
    font_letters(d, H.gl, H.nglyphs, H.npages, H.psize, g_chr);
    const uint8_t *p = d + 0x1c + H.nglyphs * 20;
    for (uint32_t i = 0; i < H.npages; i++, p += H.psize * H.psize * 4) H.page[i] = upload(p, (int)H.psize, (int)H.psize);   /* RGBA, top row first */
}

/* port extra: every release's Credits font holds all glyphs of its level fonts (same typeface and metrics) plus the letters
 * their texts never use (j, Z, X, x, Q, ...: which ones differ per language), so port-only text takes those from it */
static void credits_item(int type, int index, const uint8_t *d, uint32_t size)
{
    if (type != 3 || index != 0 || size < 0x1c) return;
    uint32_t n = rd32(d), np = rd32(d + 4), ps = rd32(d + 8);
    if (np > 8 || size < 0x1c + n * 20 + np * ps * ps * 4) return;
    H.cf.n = n; H.cf.npages = np; H.cf.psize = ps;
    H.cf.gl = malloc(n * sizeof *H.cf.gl); memcpy(H.cf.gl, d + 0x1c, n * 20);
    font_letters(d, H.cf.gl, n, np, ps, g_cchr);
    const uint8_t *p = d + 0x1c + n * 20;
    for (uint32_t i = 0; i < np; i++, p += ps * ps * 4) H.cf.page[i] = upload(p, (int)ps, (int)ps);
}

int hud_load(const char *common_rck, const char *level_rck)
{
    hud_free();
    char scope[64]; snprintf(scope, sizeof scope, "%s", tp_scope_get()); tp_scope("Common");   /* texture dumps of bank 0 go to mods\dump\Common */
    int bad = rck_walk(common_rck, 6, common_item); tp_scope(scope);
    if (bad) return -1;
    if (rck_walk(level_rck, 8 | 4 | 2, level_item) || !H.nglyphs) return -1;   /* images, strings, font */
    char cred[512]; snprintf(cred, sizeof cred, "%s", level_rck);                 /* <Data>/<level>/<level>.rck -> <Data>/Credits/Credits.rck */
    char *e = cred + strlen(cred); while (e > cred && e[-1] != '/' && e[-1] != '\\') e--;
    snprintf(e, sizeof cred - (size_t)(e - cred), "../Credits/Credits.rck");
    memset(g_cchr, 0, sizeof g_cchr); rck_walk(cred, 8, credits_item);
    {   /* port extra: the CD's language for port-only text (hud_tr), by a letter only its fonts have: Polish ł, Russian д, Spanish ñ, Brazilian ã */
        static const struct { unsigned c; int lang; } k_tell[] = { { 0x142, LANG_PL }, { 0x434, LANG_RU }, { 0xf1, LANG_ES }, { 0xe3, LANG_PT } };
        int lang = 0;
        for (int i = 0; i < 4 && !lang; i++) { int s = letter_slot(k_tell[i].c); if (s >= 0 && (g_chr[s] || g_cchr[s])) lang = k_tell[i].lang; }
        g_lang = lang;
    }
    H.ok = 1; H.k = 17.0f / (H.H - H.B);
    return 0;
}

void hud_free(void)
{
    for (int i = 0; i < 4; i++) if (H.img[i]) glDeleteTextures(1, &H.img[i]);
    for (int i = 0; i < 8; i++) if (H.page[i]) glDeleteTextures(1, &H.page[i]);
    for (int i = 0; i < 8; i++) if (H.cf.page[i]) glDeleteTextures(1, &H.cf.page[i]);
    if (H.logo) glDeleteTextures(1, &H.logo); if (H.sheet) glDeleteTextures(1, &H.sheet); if (H.sheet2) glDeleteTextures(1, &H.sheet2);
    for (int i = 0; i < 5; i++) if (H.sky[i]) glDeleteTextures(1, &H.sky[i]);
    for (int i = 0; i < 5; i++) if (H.bonus[i]) glDeleteTextures(1, &H.bonus[i]);
    for (int i = 0; i < 4; i++) if (H.env[i]) glDeleteTextures(1, &H.env[i]);
    for (int i = 0; i < 9; i++) if (H.bub[i]) glDeleteTextures(1, &H.bub[i]);
    if (H.beam) glDeleteTextures(1, &H.beam);
    for (int i = 0; i < 40; i++) if (H.fx[i]) glDeleteTextures(1, &H.fx[i]);
    for (int i = 0; i < 16; i++) if (H.limg[i]) glDeleteTextures(1, &H.limg[i]);
    for (int i = 0; i < H.nlstr; i++) free(H.lstr[i]);
    for (int i = 0; i < H.nstr; i++) free(H.str[i]);
    free(H.lstr); free(H.str); free(H.gl); free(H.cf.gl); memset(&H, 0, sizeof H);
}

/* ---------------------------------------------------------------- drawing primitives */
static void set_col(uint32_t c)
{
    float r = ((c >> 16) & 255) / 128.0f, g = ((c >> 8) & 255) / 128.0f, b = (c & 255) / 128.0f;
    glColor4f(r > 1 ? 1 : r, g > 1 ? 1 : g, b > 1 ? 1 : b, (c >> 24) / 255.0f);
}

/* RectVirtual 0x480a10: corner colours top left, bottom left, bottom right, top right; tex 0 = blank */
static void quad(float x, float y, float w, float h, GLuint tex, float u0, float v0, float u1, float v1, uint32_t tl, uint32_t bl, uint32_t br, uint32_t tr)
{
    if (tex) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, tex); } else glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    set_col(tl); glTexCoord2f(u0, v0); glVertex2f(x, y);
    set_col(bl); glTexCoord2f(u0, v1); glVertex2f(x, y + h);
    set_col(br); glTexCoord2f(u1, v1); glVertex2f(x + w, y + h);
    set_col(tr); glTexCoord2f(u1, v0); glVertex2f(x + w, y);
    glEnd();
}

static const struct { float x, y, w, h; int img; } k_spr[16] = {     /* table 0x4ab658 */
    {0,0,64,64,0}, {63,0,64,64,0}, {0,63,64,64,0}, {63,63,64,64,0}, {0,0,94,94,1}, {0,0,94,94,2}, {0,0,63,90,3}, {0,90,23,23,3},
    {27,90,34,34,3}, {97,103,29,23,3}, {5,116,123,12,1}, {5,94,123,22,1}, {63,0,64,64,3}, {111,111,16,16,2}, {97,84,19,15,3}, {0,96,31,31,2},
};
static void sprite_part(int n, float x, float y, float wcrop, uint32_t cl, uint32_t cr)   /* 0x460480, scale 1 */
{
    int i = k_spr[n].img; if (!H.img[i]) return;
    float W = (float)H.img_w[i], Hh = (float)H.img_h[i], w = wcrop < k_spr[n].w ? wcrop : k_spr[n].w;
    quad(x, y, w, k_spr[n].h, H.img[i], k_spr[n].x / W, k_spr[n].y / Hh, (k_spr[n].x + w) / W, (k_spr[n].y + k_spr[n].h) / Hh, cl, cl, cr, cr);
}
static void sprite(int n, float x, float y) { sprite_part(n, x, y, 1e9f, 0xfe808080, 0xfe808080); }

static void font_size(float s) { H.k = s / (H.H - H.B); }                          /* SetSize 0x441a60 */
static float font_cell(void) { return (H.H + 2 * (H.M < 0 ? -H.M : H.M)) * H.k; }  /* 0x441980: 62k */
static float font_measure(const uint16_t *s)                                       /* 0x441b30 (width of the last line) */
{
    float w = 0;
    for (; *s; s++) { if (*s == 4000) w = 0; else if (*s <= H.nglyphs) w += H.gl[*s - 1].adv * H.k; else if (*s & 0x8000 && (*s & 0x7fff) - 1u < H.cf.n) w += H.cf.gl[(*s & 0x7fff) - 1].adv * H.k; }
    return w;
}
static void font_draw(float x, float y, const uint16_t *s, uint32_t col)           /* 0x43f890; y = top of the cell */
{
    float x0 = x;
    for (; *s; s++) {
        if (*s == 4000) { x = x0; y += H.H * H.k; continue; }
        const Glyph *g; const GLuint *pages; uint32_t np, ps;
        if (*s <= H.nglyphs) { g = &H.gl[*s - 1]; pages = H.page; np = H.npages; ps = H.psize; }
        else if (*s & 0x8000 && (*s & 0x7fff) - 1u < H.cf.n) { g = &H.cf.gl[(*s & 0x7fff) - 1]; pages = H.cf.page; np = H.cf.npages; ps = H.cf.psize; }   /* port text: a Credits glyph */
        else continue;
        float P = (float)ps;
        if (g->page < np) quad(x, y, g->w * H.k, g->hpx * H.k, pages[g->page], g->x / P, g->y / P, (g->x + g->wpx) / P, (g->y + g->hpx) / P, col, col, col, col);
        x += g->adv * H.k;
    }
}
/* the language of the port-only texts = the one of the CD whose data is loaded, told by the letters its fonts hold
 * (hud_load). Translations (PORT EXTRA) follow the wording of each CD's own menus: Polish "Wciśnij klawisz", Spanish
 * formal "Pulse una tecla" / "Mando", Brazilian "Aperte uma tecla", Russian "Нажми на кнопку". UTF-8; a letter that
 * CD's fonts lack falls back as port_glyph says (Brazilian "â", "ó"; Polish lower-case "ż"; Russian "Ш", "Э", "ё"). */
static const struct { const char *en, *tr[4]; } k_port_tr[] = {   /* tr = Polish, Spanish, Brazilian Portuguese, Russian */
    { "Display",          { "Obraz", "Pantalla", "Vídeo", "Изображение" } },
    { "Controls",         { "Sterowanie", "Controles", "Controles", "Управление" } },
    { "Aspect ratio",     { "Proporcje obrazu", "Relación de aspecto", "Proporção da tela", "Соотношение сторон" } },
    { "Wide",             { "Panoramiczne", "Panorámica", "Widescreen", "широкое" } },
    { "Window size",      { "Rozmiar okna", "Tamaño de ventana", "Tamanho da janela", "Размер окна" } },
    { "Fullscreen",       { "Pełny ekran", "Pantalla completa", "Tela cheia", "Полный экран" } },
    { "VSync",            { "Synchronizacja pionowa", "Sincronización vertical", "Sincronização vertical", "Верт. синхронизация" } },
    { "Frame rate limit", { "Limit klatek", "Límite de fotogramas", "Limite de quadros", "Лимит кадров" } },
    { "Graphics",         { "Grafika", "Gráficos", "Gráficos", "Графика" } },
    { "Ambient occlusion", { "Okluzja otoczenia", "Oclusión ambiental", "Oclusão de ambiente", "Фоновое затенение" } },
    { "Texture sharpness", { "Ostrość tekstur", "Nitidez de texturas", "Nitidez das texturas", "Четкость текстур" } },
    { "Edge smoothing",   { "Wygładzanie krawędzi", "Suavizado de bordes", "Suavização de bordas", "Сглаживание краев" } },
    { "Multisampling",    { "Multisampling", "Multimuestreo", "Multiamostragem", "Мультисэмплинг" } },
    { "Original",         { "Oryginalna", "Original", "Original", "Исходная" } },
    { "Low",              { "Niskie", "Bajo", "Baixo", "Низкое" } },
    { "Medium",           { "Średnie", "Medio", "Médio", "Среднее" } },
    { "High",             { "Wysokie", "Alto", "Alto", "Высокое" } },
    { "Ultra",            { "Ultra", "Ultra", "Ultra", "Ультра" } },
    { "Not supported",    { "Nieobsługiwane", "No compatible", "Não suportado", "Не поддерживается" } },
    { "Device:",          { "Urządzenie:", "Dispositivo:", "Dispositivo:", "Устройство:" } },
    { "Controller",       { "Pad", "Mando", "Controle", "Геймпад" } },
    { "Keyboard",         { "Klawiatura", "Teclado", "Teclado", "Клавиатура" } },
    { "Press a button",   { "Wciśnij przycisk", "Pulse un botón", "Aperte um botão", "Нажми кнопку" } },
    { "Press a key",      { "Wciśnij klawisz", "Pulse una tecla", "Aperte uma tecla", "Нажми клавишу" } },
    { "(none)",           { "(brak)", "(ninguno)", "(nenhum)", "(нет)" } },
    { "Defaults",         { "Domyślne", "Predeterminados", "Padrão", "По умолчанию" } },
    { "Walk forward:",    { "Naprzód:", "Avanzar:", "Para frente:", "Вперед:" } },
    { "Walk back:",       { "Do tyłu:", "Retroceder:", "Para trás:", "Назад:" } },
    { "Walk left:",       { "W lewo:", "Izquierda:", "Esquerda:", "Влево:" } },
    { "Walk right:",      { "W prawo:", "Derecha:", "Direita:", "Вправо:" } },
    { "Jump:",            { "Skok:", "Saltar:", "Pular:", "Прыжок:" } },
    { "Attack:",          { "Atak:", "Atacar:", "Atacar:", "Атака:" } },
    { "Special:",         { "Atak specjalny:", "Especial:", "Especial:", "Спецатака:" } },
    { "Duck:",            { "Kucanie:", "Agacharse:", "Abaixar:", "Пригнуться:" } },
    { "Look around:",     { "Rozglądanie:", "Mirar alrededor:", "Olhar ao redor:", "Осмотреться:" } },
    { "Camera behind:",   { "Kamera za postacią:", "Cámara detrás:", "Câmera atrás:", "Камера сзади:" } },
    { "Pause:",           { "Pauza:", "Pausa:", "Pausa:", "Пауза:" } },
    { "Camera speed:",    { "Szybkość kamery:", "Velocidad de cámara:", "Velocidade da câmera:", "Скорость камеры:" } },
    { "Left stick",       { "Lewa gałka", "Stick izquierdo", "Analógico esquerdo", "Левый стик" } },
    { "Space",            { "Spacja", "Espacio", "Espaço", "Пробел" } },
    { "Left arrow",       { "Strzałka w lewo", "Flecha izquierda", "Seta esquerda", "Стрелка влево" } },
    { "Right arrow",      { "Strzałka w prawo", "Flecha derecha", "Seta direita", "Стрелка вправо" } },
    { "Up arrow",         { "Strzałka w górę", "Flecha arriba", "Seta para cima", "Стрелка вверх" } },
    { "Down arrow",       { "Strzałka w dół", "Flecha abajo", "Seta para baixo", "Стрелка вниз" } },
    { "Left Ctrl",        { "Lewy Ctrl", "Ctrl izquierdo", "Ctrl esquerdo", "Левый Ctrl" } },
    { "Right Ctrl",       { "Prawy Ctrl", "Ctrl derecho", "Ctrl direito", "Правый Ctrl" } },
    { "Left Shift",       { "Lewy Shift", "Mayús izquierda", "Shift esquerdo", "Левый Shift" } },
    { "Right Shift",      { "Prawy Shift", "Mayús derecha", "Shift direito", "Правый Shift" } },
    { "Left Alt",         { "Lewy Alt", "Alt izquierdo", "Alt esquerdo", "Левый Alt" } },
    { "Right Alt",        { "Prawy Alt", "Alt derecho", "Alt direito", "Правый Alt" } } };
const char *hud_tr(const char *en)
{
    if (g_lang == LANG_EN) return en;
    for (size_t i = 0; i < sizeof k_port_tr / sizeof *k_port_tr; i++) if (!strcmp(k_port_tr[i].en, en)) return k_port_tr[i].tr[g_lang - 1];
    return en;
}
static unsigned utf8_next(const char **s)                            /* one code point; a broken byte counts as itself */
{
    const unsigned char *p = (const unsigned char *)*s; unsigned c = *p++;
    if (c >= 0xc0 && c < 0xe0 && (p[0] & 0xc0) == 0x80) { c = (c & 0x1f) << 6 | (p[0] & 0x3f); p++; }
    else if (c >= 0xe0 && c < 0xf0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80) { c = (c & 0x0f) << 12 | (p[0] & 0x3f) << 6 | (p[1] & 0x3f); p += 2; }
    *s = (const char *)p; return c;
}
static unsigned cp_other_case(unsigned c)
{
    if (c < 128) return isupper((int)c) ? (unsigned)tolower((int)c) : (unsigned)toupper((int)c);
    if (c >= 0xc0 && c <= 0xfe && c != 0xd7 && c != 0xf7 && c != 0xdf) return c ^ 0x20;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177)) return c ^ 1;   /* Latin Extended-A: upper even ... */
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e)) return c & 1 ? c + 1 : c - 1;   /* ... or upper odd */
    if (c >= 0x410 && c <= 0x42f) return c + 0x20;
    if (c >= 0x430 && c <= 0x44f) return c - 0x20;
    if (c >= 0x400 && c <= 0x40f) return c + 0x50;
    if (c >= 0x450 && c <= 0x45f) return c - 0x50;
    return c;
}
static unsigned cp_latin_twin(unsigned c)                           /* a Cyrillic letter drawn with a Latin glyph (0 = none) */
{
    switch (c) {
    case 0x410: return 'A'; case 0x412: return 'B'; case 0x415: case 0x401: return 'E'; case 0x41a: return 'K'; case 0x41c: return 'M';
    case 0x41d: return 'H'; case 0x41e: return 'O'; case 0x420: return 'P'; case 0x421: return 'C'; case 0x422: return 'T';
    case 0x423: return 'Y'; case 0x425: return 'X';                 /* K and Y only when the font lacks its own Cyrillic one */
    case 0x430: return 'a'; case 0x435: case 0x451: return 'e'; case 0x43e: return 'o'; case 0x440: return 'p'; case 0x441: return 'c';
    case 0x443: return 'y'; case 0x445: return 'x';
    }
    return 0;
}
static unsigned cp_base(unsigned c)                                 /* a Latin letter without its accent (0 = not one) */
{
    static const char l1[] = "AAAAAAACEEEEIIII\0NOOOOO\0OUUUUY\0\0aaaaaaaceeeeiiii\0nooooo\0ouuuuy\0y";   /* 0xc0..0xff */
    static const char ea[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIi\0\0JjKk\0LlLlLlLlLlNnNnNn\0\0\0OoOoOo\0\0RrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZz";   /* 0x100..0x17e */
    if (c >= 0xc0 && c <= 0xff) return (unsigned char)l1[c - 0xc0];
    if (c >= 0x100 && c < 0x100 + sizeof ea - 1) return (unsigned char)ea[c - 0x100];
    return 0;
}
static uint16_t port_glyph1(unsigned c)
{
    int s = letter_slot(c); if (s < 0) return 0;
    return g_chr[s] ? g_chr[s] : g_cchr[s] ? (uint16_t)(0x8000 | g_cchr[s]) : 0;
}
/* the glyph of one letter: the level font's, else the Credits font's (0x8000 | code); else for Cyrillic its Latin twin or
 * the other case, for an accented Latin letter the bare one, for a plain letter the other case; else 0 */
static uint16_t port_glyph(unsigned c)
{
    uint16_t g = port_glyph1(c); if (g) return g;
    unsigned t = cp_latin_twin(c);
    if (t && (g = port_glyph1(t))) return g;
    if (c >= 0x400 && c < 0x460) {
        unsigned o = cp_other_case(c);
        if ((g = port_glyph1(o))) return g;
        if ((t = cp_latin_twin(o)) && (g = port_glyph1(t))) return g;
        return 0;
    }
    unsigned b = c < 128 ? c : cp_base(c);
    if (b && b != c && (g = port_glyph1(b))) return g;
    if (b && isalpha((int)b)) return port_glyph1(cp_other_case(b));
    return 0;
}
/* port-only text (docs/DISPLAY.md 4, port extra): the English key, translated (hud_tr), turned into the current font's codes
 * through g_chr (font_letters), again whenever another font was loaded. A letter the level font lacks (the English ones have
 * no "Z" or "j", the Polish no "X", the Russian no "J" or "z") comes from the Credits font as 0x8000 | its code, else
 * port_glyph's fallbacks, else the space. Ref = 0x7f000000 | slot. */
static struct { char a[128]; uint16_t u[64]; unsigned gen; } g_pstr[160]; static int g_npstr;   /* 0..127 kept for good, 128..159 the rewritable ones of hud_port_str_tmp */
static void port_encode(int i)
{
    int n = 0; uint16_t sp = g_chr[' '] ? g_chr[' '] : 18;
    for (const char *c = hud_tr(g_pstr[i].a); *c && n < 63; ) {
        uint16_t code = port_glyph(utf8_next(&c));
        g_pstr[i].u[n++] = code ? code : sp;
    }
    g_pstr[i].u[n] = 0; g_pstr[i].gen = g_font_gen;
}
static void port_codes(int i, const char *ascii)
{
    snprintf(g_pstr[i].a, sizeof g_pstr[i].a, "%s", ascii);
    port_encode(i);
}
uint32_t hud_port_str(const char *ascii)
{
    int i; for (i = 0; i < g_npstr; i++) if (!strcmp(g_pstr[i].a, ascii)) return 0x7f000000u | (uint32_t)i;
    if (g_npstr == 128) return 0x7f000000u;
    port_codes(i, ascii); g_npstr++;
    return 0x7f000000u | (uint32_t)i;
}
uint32_t hud_port_str_tmp(int k, const char *ascii)  /* slot k (0..31), rewritten on every call: for texts that keep changing (the Controls page) */
{
    if (k < 0 || k >= 32) return 0x7f000000u;
    port_codes(128 + k, ascii);
    return 0x7f000000u | (uint32_t)(128 + k);
}
static const uint16_t *hud_string(uint32_t ref)
{
    uint32_t i = ref & 0xffff;
    if ((ref >> 24) == 0x7f) {
        if (!((int)i < g_npstr || (i >= 128 && i < 160))) return NULL;
        if (g_pstr[i].gen != g_font_gen) port_encode((int)i);
        return g_pstr[i].u;
    }
    if ((ref >> 24) == 1) return (int)i < H.nlstr && H.lstr[i] ? H.lstr[i] : NULL;   /* level bank (the credits) */
    return (ref >> 24) == 0 && (int)i < H.nstr && H.str[i] ? H.str[i] : NULL;
}

static int number_codes(uint16_t *out, int v)                                       /* 0x441820: digits through Common string 0 "0123456789" */
{
    char b[16]; int n = snprintf(b, sizeof b, "%d", v < 0 ? 0 : v); const uint16_t *dig = hud_string(0);
    for (int i = 0; i < n; i++) out[i] = dig ? dig[b[i] - '0'] : (uint16_t)(b[i] - '0' + 1);
    out[n] = 0; return n;
}
static void number_sized(float cx, float cy, int v, float size)                     /* 0x4605b0 / 0x448660; the pop animations pass their own size */
{
    uint16_t s[16]; number_codes(s, v);
    font_size(v >= 100 ? size * 0.75f : size);
    font_draw(cx - font_measure(s) * 0.5f, cy - font_cell() * 0.5f, s, 0xfeff0000);
    font_size(17.0f);
}
static void number_centred(float cx, float cy, int v) { number_sized(cx, cy, v, 17.0f); }

/* ---------------------------------------------------------------- HUD animations (animator hud+0x30, docs/HUD_TEXT.md 4.6)
 * The five pickup flights (start 0x47b230, tick 0x47b4c0) with their trail (0x47b710), the number pop
 * (0x47c390 / 0x47c3d0), the growing plate and the sliding icon (0x47bf90 / 0x47c1a0), and the swarm of W's that
 * pays out 25 bonuses (0x47c5b0 / 0x47c620 / 0x47c7c0). Everything is linear in time and nothing ever fades: a
 * "pop" is a font size, a flight fades in by growing from zero. The animator draws on top of the static HUD
 * (0x447660 runs before 0x4480d0), and the static HUD leaves out whatever an animation has taken over. */
#define FLY_DUR   0.2f                                                /* PickupFly+0x4c, fixed in the ctor 0x47b170 */
#define SWARM_DUR 0.4f                                                /* one trip of one W, 0x47c760 */

static const float k_anchor0[4][2] = { {32,82}, {66,197}, {66,282}, {600,86} };                          /* 0x5d7b50 = slot[2i+1] + 16 */
static const float k_slot0[8][2] = { {16,16}, {16,66}, {16,136}, {50,181}, {16,221}, {50,266}, {544,30}, {584,70} };   /* 0x4b3a10 */
/* port extra (docs/DISPLAY.md 3): on a view wider than 4:3 the left column (slots 0..5, anchors 0..2) moves with the view's
 * left edge H.vx0 and the portrait / lives (slots 6, 7, anchor 3) with its right edge H.vx1; set by hud_begin_view */
static float k_anchor[4][2], k_slot[8][2];
static void hud_edges(void)
{
    float l = H.vx0, r = H.vx1 - 640.0f;
    for (int i = 0; i < 8; i++) { k_slot[i][0] = k_slot0[i][0] + (i < 6 ? l : r); k_slot[i][1] = k_slot0[i][1]; }
    for (int i = 0; i < 4; i++) { k_anchor[i][0] = k_anchor0[i][0] + (i < 3 ? l : r); k_anchor[i][1] = k_anchor0[i][1]; }
}

typedef struct { float x, y, size, t; int fresh, spawned; } SwarmW;
static struct {
    struct { int on, sprite, mode; float sx, sy, tx, ty, t, cur; } fly[6];    /* index = pickup kind 1..5 (0x448510) */
    struct { int on, phase, nph; float t, dur, s0, ds; } pop[4];              /* index = number anchor 0..3 */
    struct { int on, sprite; float cx, cy, w0, h0, dw, dh, t; } plate[3];     /* 1 = the $ plate, 2 = the charge plate */
    struct { int on, sprite; float x0, y0, dx, dy, t; } slide[3];
    struct { int on, to_life, count, popval, poprun, phase2; float counter, tx; SwarmW p[3]; } sw;
    struct { int mode; float t, life, x, y, w0, h0, vx, vy, dw, dh, len, tau, swing; } gh[128]; int ngh;
    int stage[3]; float hold[3];                                              /* kinds 2 and 3 run a 3-stage sequence with a 1.5 s hold */
    int latch;                                                                /* hud+0x10: the reward waits until the W pickup flight has landed */
    int mlives;                                                               /* hud+0x40: a life was lost (0x4622e0) */
    int mcharge; float mcharge_t;                                             /* hud+0x41: a charge was spent (0x462380 / 0x462020), its phase and hold timer */
    int prev_ok, prev_lives, prev_bonus, prev_charges, prev_unique; float prev_health;
    int state, c;                                                             /* hud+0 (0 game, 1 pause, 2 hidden) and hud+0xc (1172 keeps it at 2) */
    int f3b, f3c, f3d, f3e, f3f;                                              /* hud+0x3b..0x3f: $ counter in / out, pause HUD in / out, $ minus 1 */
    int f76, f77, f79, f7a, f7b, f7c, f7d, f7e, m54;                          /* animator +0x76..0x7e: the steps of those five, +0x54 the phase of the minus */
} A;

void hud_anim_reset(void) { memset(&A, 0, sizeof A); }

static float frnd(void) { return (float)rand() / (float)RAND_MAX; }
static float costab(int k) { return (float)cos(6.2831853 * (k & 511) / 512.0); }   /* the engine's 512-entry table at [0x5e823c] */

/* a sprite into an explicit destination rect (0x480a10; 0x460480 is the same thing with a uniform scale) */
static void sprite_rect(int n, float x, float y, float w, float h)
{
    if (n < 0 || n >= 16 || w <= 0 || h <= 0) return;
    int i = k_spr[n].img; if (!H.img[i]) return;
    float W = (float)H.img_w[i], Hh = (float)H.img_h[i];
    quad(x, y, w, h, H.img[i], k_spr[n].x / W, k_spr[n].y / Hh, (k_spr[n].x + k_spr[n].w) / W, (k_spr[n].y + k_spr[n].h) / Hh,
         0xfe808080, 0xfe808080, 0xfe808080, 0xfe808080);
}
/* a trail blob: bank 0 image 4, additive (0x47bba0 submits it with flag 4) */
static void fx_rect(float x, float y, float w, float h)
{
    if (!H.fx[1] || w <= 0 || h <= 0) return;
    glBlendFunc(GL_ONE, GL_ONE);
    quad(x, y, w, h, H.fx[1], 0, 0, 1, 1, 0xfe808080, 0xfe808080, 0xfe808080, 0xfe808080);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

/* ---- the number pop 0x47c390 / 0x47c3d0: an odd phase grows s0 -> s1, an even one shrinks back; red, no alpha */
static void pop_start(int a, int nph, float s0, float s1, float dur)
{
    A.pop[a].on = 1; A.pop[a].phase = 1; A.pop[a].nph = nph; A.pop[a].t = 0; A.pop[a].dur = dur; A.pop[a].s0 = s0; A.pop[a].ds = s1 - s0;
}
static int pop_tick(int a, int value, float dt)
{
    float f = A.pop[a].t / A.pop[a].dur, size;                        /* the ratio from BEFORE this frame, as in 0x47c3d0 */
    if (A.pop[a].phase <= A.pop[a].nph) {
        A.pop[a].t += dt;
        if (A.pop[a].t < A.pop[a].dur) size = (A.pop[a].phase & 1) ? A.pop[a].s0 + f * A.pop[a].ds : A.pop[a].s0 + A.pop[a].ds - f * A.pop[a].ds;
        else { A.pop[a].phase++; A.pop[a].t = 0; size = (A.pop[a].phase & 1) ? A.pop[a].s0 : A.pop[a].s0 + A.pop[a].ds; }
    } else size = A.pop[a].s0;
    number_sized(k_anchor[a][0], k_anchor[a][1], value, size);
    return A.pop[a].on = A.pop[a].phase <= A.pop[a].nph;
}

/* ---- the flying icon 0x47b230 / 0x47b4c0 ------------------------------------------------------------------- */
static int fly_start(int kind, const float *screen, int sprite, int slot, int mode)
{
    if (!screen) return 0;                                            /* behind the camera or off screen: 0x47b230 refuses and only the pop plays */
    A.fly[kind].on = 1; A.fly[kind].sprite = sprite; A.fly[kind].mode = mode; A.fly[kind].t = 0; A.fly[kind].cur = 0;
    A.fly[kind].sx = screen[0]; A.fly[kind].sy = screen[1];
    A.fly[kind].tx = k_slot[slot][0]; A.fly[kind].ty = k_slot[slot][1];
    return 1;
}
static void fly_trail(int kind, float dx, float dy, float w, float h)  /* 0x47b710: ghosts along the same line at a fixed step in animation time */
{
    int mode = A.fly[kind].mode;
    float step = mode ? 0.005f : 0.02f;
    while (A.fly[kind].cur < A.fly[kind].t) {
        float ut = A.fly[kind].cur, u = ut / FLY_DUR, r;
        float gx = A.fly[kind].sx + dx * u, gy = A.fly[kind].sy + dy * u, gw = w * u, gh = h * u;
        if (A.ngh < 128) {
            int i = A.ngh++;
            A.gh[i].mode = mode; A.gh[i].t = A.fly[kind].t - ut;        /* born already this old */
            r = costab((int)(frnd() * 180.0f)) + 1.0f;
            A.gh[i].w0 = r * gw * 0.25f; A.gh[i].h0 = r * gh * 0.25f;
            if (!mode) {                                               /* 0x47b800: a blob beside the icon that shrinks away in 0.3 s */
                r = costab((int)(frnd() * 180.0f)); A.gh[i].x = dx < 0 ? gx + r * 10.0f + gw * 0.25f : gx - gw * 0.25f - r * 10.0f;
                r = costab((int)(frnd() * 180.0f)); A.gh[i].y = dy < 0 ? gy + r * 10.0f + gh * 0.25f : gy - gh * 0.25f - r * 10.0f;
                A.gh[i].vx = dx / FLY_DUR * 0.2f; A.gh[i].vy = dy / FLY_DUR * 0.2f;
                A.gh[i].life = 0.3f; A.gh[i].dw = A.gh[i].w0 / 0.3f; A.gh[i].dh = A.gh[i].h0 / 0.3f;
            } else {                                                   /* 0x47ba10: two glows winding 3.5 turns around the flight line, crawling at a fifth of its speed */
                float ox = A.fly[kind].sx + (dx >= 0 ? -2.0f * gw : 2.0f * gw), oy = A.fly[kind].sy + (dy >= 0 ? -2.0f * gh : 2.0f * gh);
                float Dx = A.fly[kind].sx + dx - ox, Dy = A.fly[kind].sy + dy - oy, L = (float)sqrt(Dx * Dx + Dy * Dy);
                if (L < 1e-3f) { A.ngh--; A.fly[kind].cur = ut + step; continue; }
                A.gh[i].x = ox; A.gh[i].y = oy; A.gh[i].vx = Dx / L; A.gh[i].vy = Dy / L;
                A.gh[i].len = L; A.gh[i].tau = 5.0f * ut; A.gh[i].swing = w; A.gh[i].life = 0.5f;
            }
        }
        A.fly[kind].cur = ut + step;
    }
}
static int fly_tick(int kind, float dt)
{
    int sp = A.fly[kind].sprite; float w = k_spr[sp].w, h = k_spr[sp].h;
    float dx = A.fly[kind].tx + w * 0.5f - A.fly[kind].sx, dy = A.fly[kind].ty + h * 0.5f - A.fly[kind].sy;
    A.fly[kind].t += dt;
    if (A.fly[kind].t < FLY_DUR) {
        float u = A.fly[kind].t / FLY_DUR, cw = w * u, ch = h * u;
        fly_trail(kind, dx, dy, w, h);                                 /* 0x47b4d9: the trail is emitted before the icon moves */
        sprite_rect(sp, A.fly[kind].sx + dx * u - cw * 0.5f, A.fly[kind].sy + dy * u - ch * 0.5f, cw, ch);
        return 1;
    }
    sprite_rect(sp, A.fly[kind].tx, A.fly[kind].ty, w, h);             /* the last frame is exactly the static icon: a seamless hand-over */
    return A.fly[kind].on = 0;
}
static void ghosts_draw(float dt)                                      /* 0x47bba0 (mode 0) and 0x47bca0 (mode 1); both shrink to nothing, neither fades */
{
    for (int i = 0; i < A.ngh; i++) {
        float t = (A.gh[i].t += dt);
        if (t >= A.gh[i].life || (A.gh[i].mode && A.gh[i].tau + t > 1.0f)) { A.gh[i] = A.gh[--A.ngh]; i--; continue; }   /* mode 1 reaches the HUD: the original reflects the time, which kills it the next frame anyway */
        if (!A.gh[i].mode) {
            float X = A.gh[i].x + t * A.gh[i].vx, Y = A.gh[i].y + t * A.gh[i].vy;
            float W = A.gh[i].w0 - t * A.gh[i].dw, Hh = A.gh[i].h0 - t * A.gh[i].dh;
            fx_rect(X - W * 0.5f, Y - Hh * 0.5f, W, Hh);
        } else {
            float f = A.gh[i].tau + t;                                 /* = S / L, the fraction of the path this strand has reached (5*FLY_DUR = 1) */
            float S = A.gh[i].len * f, k = 1.0f - t / A.gh[i].life;
            float off = (float)sin(6.2831853 * ((int)(1800.0f * f) & 511) / 512.0) * A.gh[i].swing * 0.5f * f;   /* 1800/512 = 3.5 turns, opening to half the icon width */
            float cx = A.gh[i].x + A.gh[i].vx * S, cy = A.gh[i].y + A.gh[i].vy * S, nx = -A.gh[i].vy, ny = A.gh[i].vx;
            float W = A.gh[i].w0 * k, Hh = A.gh[i].h0 * k;
            fx_rect(cx + nx * off - W * 0.5f, cy + ny * off - Hh * 0.5f, W, Hh);
            fx_rect(cx - nx * off - W * 0.5f, cy - ny * off - Hh * 0.5f, W, Hh);
        }
    }
}

/* ---- the plate and the sliding icon, both linear over 0.2 s (0x47bff0 / 0x47c1e0) -------------------------- */
/* 0x47bf90(x, y, w0, h0, w1, h1): (x, y) is the slot, the top left of the full-size plate; the object keeps its
 * centre (x + 17, y + 17) = slot + half of sprite 8, so the grown plate lands exactly on the static one */
static void plate_start(int k, float x, float y, float w0, float h0, float w1, float h1)
{ A.plate[k].on = 1; A.plate[k].sprite = 8; A.plate[k].cx = x + k_spr[8].w * 0.5f; A.plate[k].cy = y + k_spr[8].h * 0.5f; A.plate[k].w0 = w0; A.plate[k].h0 = h0; A.plate[k].dw = w1 - w0; A.plate[k].dh = h1 - h0; A.plate[k].t = 0; }
static int plate_tick(int k, float dt)
{
    float u = A.plate[k].t / 0.2f, w, h;
    A.plate[k].t += dt;
    if (A.plate[k].t < 0.2f) { w = A.plate[k].w0 + u * A.plate[k].dw; h = A.plate[k].h0 + u * A.plate[k].dh; }
    else { w = A.plate[k].w0 + A.plate[k].dw; h = A.plate[k].h0 + A.plate[k].dh; }
    sprite_rect(A.plate[k].sprite, A.plate[k].cx - w * 0.5f, A.plate[k].cy - h * 0.5f, w, h);
    return A.plate[k].on = u < 1.0f;
}
static void slide_start(int k, int sprite, float x0, float y0, float x1, float y1)
{ A.slide[k].on = 1; A.slide[k].sprite = sprite; A.slide[k].x0 = x0; A.slide[k].y0 = y0; A.slide[k].dx = x1 - x0; A.slide[k].dy = y1 - y0; A.slide[k].t = 0; }
static int slide_tick(int k, float dt)
{
    float u = (A.slide[k].t += dt) / 0.2f; if (u > 1.0f) u = 1.0f;
    sprite_rect(A.slide[k].sprite, A.slide[k].x0 + u * A.slide[k].dx, A.slide[k].y0 + u * A.slide[k].dy, k_spr[A.slide[k].sprite].w, k_spr[A.slide[k].sprite].h);
    return A.slide[k].on = u < 1.0f;
}

/* ---- the $ counter in place of the bonus counter (message 1172: hud+0x3b in, +0x3c out, docs/HUD_TEXT.md 4.5) and
 * the extended HUD of the pause pages (+0x3d in, +0x3e out). Slide 0 / plate 0 are the W icon and its plate, slide,
 * plate and pop 1 the $, 2 the charge: the same objects the pickup sequences use. Each step flag is the return value
 * of its own tick, exactly as the animator keeps them (+0x76..+0x7e). */
#define SL(n) k_slot[n][0], k_slot[n][1]
static void dollar_in_start(void)                                       /* 0x4618d0 */
{
    plate_start(0, SL(1), 34.0f, 34.0f, 0, 0); slide_start(0, 4, SL(0), H.vx0 - k_spr[4].w, k_slot[0][1]);   /* the bonus plate shrinks, then the W leaves to the left */
    A.f7c = A.f79 = 1;
    slide_start(1, 3, H.vx0 - k_spr[3].w, k_slot[2][1], SL(2)); plate_start(1, SL(3), 0, 0, 34.0f, 34.0f);   /* the $ comes in from the left, its plate grows */
    pop_start(1, 2, 17.0f, 37.0f, 0.2f);
    A.f7a = A.f7d = A.f76 = 1;
}
static int dollar_in_tick(int value, float dt)                          /* 0x461820 */
{
    if (A.f7c) { sprite(4, SL(0)); A.f7c = plate_tick(0, dt); }
    else if (A.f79) A.f79 = slide_tick(0, dt);
    if (A.f7a) { A.f7a = slide_tick(1, dt); return 1; }
    if (A.f7d) { sprite(3, SL(2)); A.f7d = plate_tick(1, dt); return 1; }
    if (A.f76) { sprite(3, SL(2)); sprite(8, SL(3)); return pop_tick(1, value, dt); }   /* +0x76 is not cleared here */
    return 1;
}
static void dollar_out_start(void)                                      /* 0x461a70 */
{
    plate_start(0, SL(1), 0, 0, 34.0f, 34.0f); slide_start(0, 4, H.vx0 - k_spr[4].w, k_slot[0][1], SL(0));
    A.f7c = A.f79 = 1;
    slide_start(1, 3, SL(2), H.vx0 - k_spr[3].w, k_slot[2][1]); plate_start(1, SL(3), 34.0f, 34.0f, 0, 0);
    A.f7a = A.f7d = 1;
}
static int dollar_out_tick(float dt)                                    /* 0x461a00: the W slides back and its plate grows; the $ plate shrinks, then the $ leaves */
{
    if (A.f79) A.f79 = slide_tick(0, dt);
    else if (A.f7c) { sprite(4, SL(0)); A.f7c = plate_tick(0, dt); }
    if (A.f7d) { sprite(3, SL(2)); A.f7d = plate_tick(1, dt); return 1; }
    if (A.f7a) return slide_tick(1, dt);
    return 1;
}
static void pause_in_start(void)                                        /* 0x461c40: $ and charge come in together */
{
    slide_start(1, 3, H.vx0 - k_spr[3].w, k_slot[2][1], SL(2)); plate_start(1, SL(3), 0, 0, 34.0f, 34.0f); pop_start(1, 2, 17.0f, 37.0f, 0.2f);
    A.f76 = A.f7a = A.f7d = 1;
    slide_start(2, 6, H.vx0 - k_spr[6].w, k_slot[4][1], SL(4)); plate_start(2, SL(5), 0, 0, 34.0f, 34.0f); pop_start(2, 2, 17.0f, 37.0f, 0.2f);
    A.f77 = A.f7b = A.f7e = 1;
}
static int pause_in_tick(int dollars, int charges, float dt)            /* 0x461b60 */
{
    if (A.f7a) { A.f7a = slide_tick(1, dt); A.f7b = slide_tick(2, dt); return 1; }
    if (A.f7d) { sprite(3, SL(2)); sprite(6, SL(4)); A.f7d = plate_tick(1, dt); A.f7e = plate_tick(2, dt); return 1; }
    if (A.f76) { sprite(3, SL(2)); sprite(8, SL(3)); sprite(6, SL(4)); sprite(8, SL(5)); int r = pop_tick(1, dollars, dt); pop_tick(2, charges, dt); return r; }
    return 1;
}
static void pause_out_start(void)                                       /* 0x461e10: no pop, the plates shrink and then both icons leave */
{
    slide_start(1, 3, SL(2), H.vx0 - k_spr[3].w, k_slot[2][1]); plate_start(1, SL(3), 34.0f, 34.0f, 0, 0);
    A.f76 = A.f7a = A.f7d = 1;
    slide_start(2, 6, SL(4), H.vx0 - k_spr[6].w, k_slot[4][1]); plate_start(2, SL(5), 34.0f, 34.0f, 0, 0);
    A.f77 = A.f7b = A.f7e = 1;
}
static int pause_out_tick(float dt)                                     /* 0x461da0 */
{
    if (A.f7d) { sprite(3, SL(2)); sprite(6, SL(4)); A.f7d = plate_tick(1, dt); A.f7e = plate_tick(2, dt); return 1; }
    if (A.f7a) { int r = slide_tick(1, dt); slide_tick(2, dt); return r; }
    return 1;
}
/* 0x462330 / 0x461f70: a $ was spent (message 1171): the icon and plate appear at once, the OLD value pops
 * 17 -> 37 -> 17 and then shrinks to nothing (17 -> 0 in 0.2 s); nothing slides */
static void dollar_minus_start(void) { pop_start(1, 2, 17.0f, 37.0f, 0.2f); A.f76 = 1; A.m54 = 1; }
static int dollar_minus_tick(int value, float dt)
{
    if (A.m54 == 1) {
        sprite(3, SL(2)); sprite(8, SL(3));
        if (!(A.f76 = pop_tick(1, value + 1, dt))) { A.m54 = 2; pop_start(1, 1, 17.0f, 0.0f, 0.2f); }
        return 1;
    }
    if (A.m54 == 2) { sprite(3, SL(2)); sprite(8, SL(3)); return pop_tick(1, value + 1, dt); }
    return 1;
}
#undef SL

/* ---- the swarm of W's: 25 bonuses being paid out as a heart or as an extra life (0x47c5b0) ----------------- */
static void swarm_start(int to_life, float relx)
{
    memset(&A.sw, 0, sizeof A.sw);
    A.sw.on = 1; A.sw.to_life = to_life; A.sw.counter = 25.0f; A.sw.count = 1; A.sw.tx = relx;
    for (int i = 0; i < 3; i++) { A.sw.p[i].x = k_slot[0][0]; A.sw.p[i].y = k_slot[0][1]; A.sw.p[i].size = k_spr[4].w; A.sw.p[i].fresh = 1; }
    if (to_life) pop_start(3, 2, 17.0f, 37.0f, 0.2f);                  /* 0x460cc0 arms the lives pop for phase 2 right away */
}
static int swarm_tick(float dt)                                        /* 0x47c620 + 0x47c7c0 */
{
    const float span = k_spr[4].w - 23.0f;                             /* the W shrinks 94 -> 23 on the way */
    if (A.sw.counter <= 0.0f) { A.sw.on = 0; A.sw.poprun = 0; A.pop[0].on = 0; return 0; }   /* the original tests == 0.0f exactly; <= 0 cannot get stuck */
    float third = A.sw.tx / 3.0f;
    for (int i = 0; i < A.sw.count; i++) {
        SwarmW *p = &A.sw.p[i];
        if (p->x >= third && p->x < 2.0f * third && !p->spawned && A.sw.count < 3) { A.sw.count++; p->spawned = 1; }   /* the next W leaves once this one is a third of the way */
        if (A.sw.counter < 3.0f && p->fresh) continue;                 /* wind-down: do not launch a fresh W for the last two */
        int n = (int)A.sw.counter;
        if (n % 5 == 0 && !A.sw.poprun) { A.sw.popval = n - 5; pop_start(0, 1, 17.0f, 0.0f, 1.0f); A.sw.poprun = 1; }   /* 20, 15, 10, 5, 0, each shrinking away */
        if (A.sw.poprun) A.sw.poprun = pop_tick(0, A.sw.popval, dt);   /* ticked once per W in flight, exactly as the original */
        p->fresh = 0; p->t += dt;
        if (p->t < SWARM_DUR) { float u = p->t / SWARM_DUR; p->x = k_slot[0][0] + A.sw.tx * u; p->y = k_slot[0][1] + span * 0.5f * u; p->size = k_spr[4].w - span * u; }
        else { p->x = k_slot[0][0] + A.sw.tx; p->y = k_slot[0][1] + span * 0.5f; p->size = 23.0f; A.sw.counter -= 2.5f; }   /* ten landings pay out the 25 */
        sprite_rect(4, p->x, p->y, p->size, p->size);
        if (p->t >= SWARM_DUR) { p->x = k_slot[0][0]; p->y = k_slot[0][1]; p->size = k_spr[4].w; p->t = 0; p->fresh = 1; p->spawned = 0; }
    }
    return 1;
}

/* ---- starting an animation ---------------------------------------------------------------------------------- */
/* 0x448510: kind 1..5 for types 30, 36, 35, 34, 37; screen = the pickup projected into the 640x480 HUD, NULL when it is off screen */
void hud_anim_pickup(int kind, const float *screen, int face)
{
    if (!H.ok) return;
    switch (kind) {
    case 1:                                                            /* 0x461300: the character's own face flies to the portrait */
        pop_start(3, 2, 17.0f, 37.0f, 0.2f);
        fly_start(1, screen, face >= 0 && face < 3 ? face : 0, 6, 1);
        break;
    case 2:                                                            /* 0x461420: the $ item; its whole counter slides in, waits and leaves again */
        slide_start(1, 3, k_slot[2][0], k_slot[2][1], H.vx0 - k_spr[3].w, k_slot[2][1]);
        pop_start(1, 2, 17.0f, 37.0f, 0.2f);
        plate_start(1, k_slot[3][0], k_slot[3][1], 0, 0, 34.0f, 34.0f);
        fly_start(2, screen, 3, 2, 0);
        A.stage[1] = 1; A.hold[1] = 0;
        break;
    case 3:                                                            /* 0x461560: the charge, the same sequence one row lower */
        slide_start(2, 6, k_slot[4][0], k_slot[4][1], H.vx0 - k_spr[6].w, k_slot[4][1]);
        pop_start(2, 2, 17.0f, 37.0f, 0.2f);
        plate_start(2, k_slot[5][0], k_slot[5][1], 0, 0, 34.0f, 34.0f);
        fly_start(3, screen, 6, 4, 1);
        A.stage[2] = 1; A.hold[2] = 0;
        break;
    case 4:                                                            /* 0x4616a0: the big W to the bonus icon */
        pop_start(0, 2, 17.0f, 37.0f, 0.2f);
        fly_start(4, screen, 4, 0, 0);
        A.latch = 0;                                                   /* hud+0x10 is cleared here and set again by the reward */
        break;
    case 5:                                                            /* 0x461760: the race flag, same slot */
        pop_start(0, 2, 17.0f, 37.0f, 0.2f);
        fly_start(5, screen, 5, 0, 0);
        break;
    default: break;                                                    /* kind 6 (type 38) does nothing */
    }
}
/* 0x448380: the bonus counter went down, so 25 W's have just been paid out */
static void hud_anim_reward(int to_life, float health_old)
{
    swarm_start(to_life, to_life ? (k_spr[0].w - 23.0f) * 0.5f + k_slot[6][0] - k_slot[0][0]    /* 548.5: a 23 wide square centred on the portrait */
                                 : 559.0f + H.vx1 - 640.0f - 30.0f * (health_old + 1.0f) - k_slot[0][0]);   /* the slot of the heart that is coming in */
    A.latch = 1;
}

/* the animator itself: 0x4480d0, run after the static HUD so everything here draws on top */
static void hud_anim_tick(const HudState *s, float dt)
{
    int lives_hud = s->lives > 0 ? s->lives - 1 : 0;                   /* hud+0x20, the value the row shows */
    if (A.latch && !A.fly[4].on && !A.pop[0].on) A.latch = 0;          /* 0x44812b clears hud+0x10 with the flag; never deadlock if the flight never started */
    if (A.sw.on && !A.latch) {
        if (!A.sw.to_life) A.sw.on = swarm_tick(dt);                                         /* 0x460bb0: the heart variant is the swarm and nothing else */
        else if (!A.sw.phase2) {                                                             /* 0x460be0 phase 1 */
            if (!swarm_tick(dt)) A.sw.phase2 = 1;
            number_sized(k_anchor[3][0], k_anchor[3][1], lives_hud > 0 ? lives_hud - 1 : 0, 17.0f);
            A.sw.on = 1;
        } else {                                                                             /* phase 2: the lives number pops to its new value */
            number_sized(k_anchor[0][0], k_anchor[0][1], s->bonus > 0 ? s->bonus - 1 : 0, 17.0f);
            if (!pop_tick(3, lives_hud, dt)) A.sw.on = 0;
        }
    }
    if (A.fly[4].on || A.latch) {                                      /* 0x4611b0; 0x448115 feeds it a literal 25 while the latch is up, so the circle reads 24 and then pops 25 */
        int v = A.latch ? 25 : s->bonus;
        if (A.fly[4].on) { fly_tick(4, dt); number_sized(k_anchor[0][0], k_anchor[0][1], v > 0 ? v - 1 : 0, 17.0f); }
        else if (!A.pop[0].on || !pop_tick(0, v, dt)) A.latch = 0;     /* the whole animation, flight and pop, holds the reward back */
    }
    else if (A.fly[5].on) { fly_tick(5, dt); number_sized(k_anchor[0][0], k_anchor[0][1], s->bonus > 0 ? s->bonus - 1 : 0, 17.0f); }
    else if (A.pop[0].on && !A.sw.on) pop_tick(0, s->bonus == 0 && !s->race ? 25 : s->bonus, dt);   /* 0x4611b0: a counter that wrapped pops "25" */
    if (A.fly[1].on) { fly_tick(1, dt); number_sized(k_anchor[3][0], k_anchor[3][1], lives_hud > 0 ? lives_hud - 1 : 0, 17.0f); }
    else if (A.mlives) {                                                                     /* 0x461f00: a life lost, 17 -> 37 -> 17 and then away */
        if (!pop_tick(3, lives_hud + 1, dt)) { if (A.mlives == 1) { A.mlives = 2; pop_start(3, 1, 17.0f, 0.0f, 0.2f); } else A.mlives = 0; }
    } else if (A.pop[3].on && !A.sw.on) pop_tick(3, lives_hud, dt);
    for (int k = 1; k <= 2; k++) {                                     /* 0x460db0 / 0x460fb0: the $ and charge counters appear, hold 1.5 s and leave */
        int icon = k == 1 ? 3 : 6, slot_i = k == 1 ? 2 : 4, slot_p = k == 1 ? 3 : 5, value = k == 1 ? s->unique : s->charges;
        if (!A.stage[k] || (k == 1 && A.f3f)) continue;               /* 0x448174: the $ sequence waits while the "minus 1" runs; 0x447b18: outside the pause page 0x447660 draws no $ / charge row at all, so this sequence is the only thing showing them */
        if (A.stage[k] == 1) {
            if (A.fly[k + 1].on) fly_tick(k + 1, dt);
            if (A.plate[k].on) { plate_tick(k, dt); continue; }
            sprite(icon, k_slot[slot_i][0], k_slot[slot_i][1]); sprite(8, k_slot[slot_p][0], k_slot[slot_p][1]);
            if (A.pop[k].on) { pop_tick(k, value, dt); continue; }
            number_centred(k_anchor[k][0], k_anchor[k][1], value);
            plate_start(k, k_slot[slot_p][0], k_slot[slot_p][1], 34.0f, 34.0f, 0, 0); A.stage[k] = 2;
        } else if (A.stage[k] == 2) {
            sprite(icon, k_slot[slot_i][0], k_slot[slot_i][1]); sprite(8, k_slot[slot_p][0], k_slot[slot_p][1]);
            number_centred(k_anchor[k][0], k_anchor[k][1], value);
            if ((A.hold[k] += dt) > 1.5f) A.stage[k] = 3;
        } else {
            plate_tick(k, dt);
            if (!slide_tick(k, dt)) A.stage[k] = 0;
        }
    }
    if (A.f3b) {                                                       /* 0x4481bd: the $ counter comes in (1172); if 1172 stopped meanwhile it goes straight out again */
        if (!A.f3c) { A.f3b = dollar_in_tick(s->unique, dt); if (A.c < 2 && !A.f3b) { dollar_out_start(); A.f3c = 1; } }
    } else if (A.f3c) {                                                /* ... and leaves; 1172 again meanwhile brings it back in */
        A.f3c = dollar_out_tick(dt); if (A.c == 2 && !A.f3c) { dollar_in_start(); A.f3b = 1; }
    }
    if (A.f3d) A.f3d = pause_in_tick(s->unique, s->charges, dt);       /* 0x44821f: the pause page's extended HUD */
    if (A.f3e) A.f3e = pause_out_tick(dt);
    if (A.f3f && !A.stage[1]) A.f3f = dollar_minus_tick(s->unique, dt);   /* 0x448261, not while the $ pickup sequence (+0x38) runs */
    if (A.mcharge && !A.stage[2] && !A.fly[3].on) {                   /* 0x462020: only while the pickup sequence of the charge does not run; shows the OLD value */
        int old = s->charges + 1; const float *si = k_slot[4], *sp = k_slot[5];
        switch (A.mcharge) {
        case 1: if (!slide_tick(2, dt)) A.mcharge = 2; break;                                               /* the icon slides in */
        case 2: sprite(6, si[0], si[1]); if (!plate_tick(2, dt)) A.mcharge = 3; break;                      /* the round plate grows */
        case 3: sprite(6, si[0], si[1]); sprite(8, sp[0], sp[1]); if (!pop_tick(2, old, dt)) A.mcharge = 4; break;   /* 17 -> 37 -> 17 */
        case 4: sprite(6, si[0], si[1]); sprite(8, sp[0], sp[1]); number_centred(k_anchor[2][0], k_anchor[2][1], old);
                pop_start(2, 1, 17.0f, 0.0f, 0.2f); slide_start(2, 6, si[0], si[1], H.vx0 - k_spr[6].w, si[1]); plate_start(2, sp[0], sp[1], 34.0f, 34.0f, 0, 0);
                A.mcharge = 5; A.mcharge_t = 0; break;
        case 5: sprite(6, si[0], si[1]); sprite(8, sp[0], sp[1]); number_sized(k_anchor[2][0], k_anchor[2][1], old, 17.0f);
                if ((A.mcharge_t += dt) > 1.0f) A.mcharge = 6; break;
        case 6: sprite(6, si[0], si[1]); sprite(8, sp[0], sp[1]); if (!pop_tick(2, old, dt)) A.mcharge = 7; break;   /* the number shrinks away */
        case 7: sprite(6, si[0], si[1]); if (!plate_tick(2, dt)) A.mcharge = 8; break;
        default: if (!slide_tick(2, dt)) A.mcharge = 0; break;
        }
    }
    ghosts_draw(dt);
    /* the setters 0x448380 / 0x4482c0 watch the values themselves; the port does the same by comparing frames */
    if (A.prev_ok && !s->race) {
        if (s->bonus < A.prev_bonus && !A.sw.on) hud_anim_reward(!(A.prev_health < 5.0f), A.prev_health);
        if (s->lives < A.prev_lives && !A.mlives) { A.mlives = 1; pop_start(3, 2, 17.0f, 37.0f, 0.2f); }
        if (s->unique < A.prev_unique) { dollar_minus_start(); A.f3f = 1; }   /* 0x448340 -> 0x462330 */
        if (s->charges < A.prev_charges && !A.mcharge) {               /* 0x448300 -> 0x462380 */
            slide_start(2, 6, H.vx0 - k_spr[6].w, k_slot[4][1], k_slot[4][0], k_slot[4][1]); plate_start(2, k_slot[5][0], k_slot[5][1], 0, 0, 34.0f, 34.0f);
            pop_start(2, 2, 17.0f, 37.0f, 0.2f); A.mcharge = 1;
        }
    }
    A.prev_ok = 1; A.prev_lives = s->lives; A.prev_bonus = s->bonus; A.prev_health = s->health; A.prev_charges = s->charges; A.prev_unique = s->unique;
}

void hud_begin(int win_w, int win_h) { hud_begin_view(0, 0, win_w, win_h); }
/* port extra (docs/DISPLAY.md 3): the 480 virtual lines fill the view's height; a view wider than 4:3 shows more virtual x on
 * both sides of 0..640 instead of stretching it, so the menus keep their 4:3 layout, centred, and the in-game HUD keeps its
 * shapes but hugs the view's edges (hud_edges). A narrower view stretches 0..640 as before (the display code hands it a 4:3
 * box instead). */
void hud_begin_view(int vx, int vy, int vw, int vh)
{
    glViewport(vx, vy, vw, vh);
    H.vx0 = 0; H.vx1 = 640;
    if (vh > 0 && vw * 3 >= vh * 4) { float hw = 240.0f * vw / vh; H.vx0 = 320 - hw; H.vx1 = 320 + hw; H.iris_kx = H.iris_ky = 1; }
    else { float sx = vw / 640.0f, sy = vh / 480.0f, s = sx > sy ? sx : sy; H.iris_kx = sx > 0 ? s / sx : 1; H.iris_ky = sy > 0 ? s / sy : 1; }
    hud_edges();
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(H.vx0, H.vx1, 480, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_LIGHTING); glDisable(GL_ALPHA_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_FOG);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_FALSE);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}
void hud_end(void)
{
    glColor4f(1, 1, 1, 1); glDisable(GL_TEXTURE_2D); glDepthMask(GL_TRUE); glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW); glPopMatrix();
}

/* ---------------------------------------------------------------- HUD 0x447210 (docs/HUD_TEXT.md 4.4, draw order as there) */
static void hud_static(const HudState *s, float dt);
void hud_state(int st, int race)                                        /* 0x448450 */
{
    if (st == A.state) return;
    if (!race) {
        if (st == 1) { pause_in_start(); A.f3d = 1; A.f3e = 0; }
        else if (A.state == 1) { pause_out_start(); A.f3e = 1; A.f3d = 0; }
    }
    A.state = st;
}
void hud_draw(const HudState *s, float dt)
{
    if (!H.ok) return;
    if (s->dollar) { if (A.c == 0 && !A.f3c) { dollar_in_start(); A.f3b = 1; } A.c = 2; }   /* 0x4484a0, before the HUD (0x401e28) */
    if (A.state != 2) hud_static(s, dt);
    if (A.c > 0 && --A.c == 0 && !A.f3b) { dollar_out_start(); A.f3c = 1; }                /* 0x447232: no 1172 this frame -> out */
}
/* 0x447210: the bars, 0x447660 the sprites and numbers, 0x447d70 the power meter, 0x4480d0 the animations */
static void hud_static(const HudState *s, float dt)
{
    const float Y = 51, BH = 23, l = H.vx0, r = H.vx1 - 640.0f;                                           /* l, r: the view's edges (hud_edges) */
    quad(l, Y, 256, BH, 0, 0, 0, 0, 0, 0x800000ff, 0x800000ff, 0x000000ff, 0x000000ff);                   /* blue bar (0x4472d8) */
    if (!s->race) {
        quad(r + 384, Y, 27, BH, 0, 0, 0, 0, 0, 0x00ff0000, 0x00ff0000, 0x13ff0000, 0x13ff0000);
        for (int i = 1; i <= 5; i++) {                                                                      /* empty health slots, alpha ramp (x - 384) * 128 / 175 */
            float x = 556.0f - 29.0f * i; uint32_t al = (uint32_t)((x - 384) * 128 / 175), ar = (uint32_t)((x + 29 - 384) * 128 / 175);
            sprite_part(9, r + x, Y, 1e9f, al << 24 | 0x808080, ar << 24 | 0x808080);
        }
    } else quad(r + 384, Y, 172, BH, 0, 0, 0, 0, 0, 0x00ff0000, 0x00ff0000, 0x80ff0000, 0x80ff0000);
    quad(r + 556, Y, 84, BH, 0, 0, 0, 0, 0, 0x80ff0000, 0x80ff0000, 0x80ff0000, 0x80ff0000);
    sprite(s->face >= 0 && s->face < 3 ? s->face : 0, k_slot[6][0], k_slot[6][1]);                         /* slot 6 */
    sprite(8, k_slot[7][0], k_slot[7][1]);                                                                  /* slot 7, anchor A3 */
    if (!A.fly[1].on && !A.mlives && !(A.sw.on && A.sw.to_life && !A.latch)) number_centred(k_anchor[3][0], k_anchor[3][1], s->lives > 0 ? s->lives - 1 : 0);   /* 0x44771e */
    if (A.c == 0 && (s->race || (!A.f3b && !A.f3c))) {                                                      /* 0x447794: while 1172 holds the $ counter up there is no bonus row at all */
        sprite(s->race ? 5 : 4, k_slot[0][0], k_slot[0][1]);                                                    /* slot 0 */
        sprite(8, k_slot[1][0], k_slot[1][1]);                                                                  /* slot 1, A0: the icon and the plate stay, only the number moves out of the way */
        if (!A.sw.on && !A.fly[4].on && !A.fly[5].on) number_centred(k_anchor[0][0], k_anchor[0][1], s->bonus);  /* 0x44792e, 0x44794f */
        if (s->show_total && !A.sw.on) {                                                                        /* the "taken / total" line goes too while the reward is paid out */
            uint16_t t[40]; int n = number_codes(t, s->got); const uint16_t *sl = hud_string(9);                /* "/" */
            for (; sl && *sl && n < 20; sl++) t[n++] = *sl;
            number_codes(t + n, s->total);
            font_size(17.0f); font_draw(l + 110, Y + 12 - font_cell() * 0.5f, t, 0xfe808080);
        }
    }
    if (!s->race || A.c > 0) for (int i = 1; i <= (int)s->health && i <= 5; i++) {
        if (A.sw.on && !A.sw.to_life && i == (int)s->health) continue;                                       /* 0x447ad5: the heart the W's are bringing in is left out until they land */
        sprite(7, r + 559.0f - 29.0f * i, Y);
    }
    if (A.c > 0 ? !A.f3b && !A.f3c && !A.f3f : A.state == 1 && !A.f3d && !A.f3e && !s->race) {           /* 0x447c63 / 0x447b15 */
        sprite(3, k_slot[2][0], k_slot[2][1]); sprite(8, k_slot[3][0], k_slot[3][1]); number_centred(k_anchor[1][0], k_anchor[1][1], s->unique);   /* slots 2/3, A1 */
    }
    if (A.state == 1 && !A.f3d && !A.f3e && (A.c > 0 || !s->race)) {                                        /* the charge row only on the pause page */
        sprite(6, k_slot[4][0], k_slot[4][1]); sprite(8, k_slot[5][0], k_slot[5][1]); number_centred(k_anchor[2][0], k_anchor[2][1], s->charges);  /* slots 4/5, A2 */
    }
    if (s->power > 0) {                                                                                     /* power gauge 0x447d70 */
        if (s->power >= 1.0f) { H.blink += dt; if (H.blink >= 0.3f) H.blink -= 0.3f; sprite(10, l, 426); if (H.blink >= 0.15f) sprite(11, l, 421); }
        else { float w = s->power * 104.0f; sprite_part(10, l, 426, w, 0xfe808080, 0xfe808080); sprite_part(11, l, 421, w, 0xfe808080, 0xfe808080); }
    } else H.blink = 0;
    hud_anim_tick(s, dt);                                                                                   /* 0x4480d0 runs after 0x447660, so the animations draw on top */
}

/* ---------------------------------------------------------------- boss bar (docs/HUD_TEXT.md 4.4): object ctor 0x47abe0, start 0x47aca0,
 * drawer 0x47b0b0(cur, max). X = 559 - 3, row y 424; the max slots (sprite 14) with alpha 30..128 across the row and the red end
 * rect slide in from the right edge in 2.0 s (+0x24), then the Buzz face (sprite 12) grows in 0.2 s (0x47bf70 / 0x47bff0) and
 * only after that the cur balls (sprite 13) are drawn. t = seconds since the bar was switched on. */
static void hud_boss_row(int cur, int max, float t);
void hud_boss_bar(int cur, int max, float t)
{
    if (!H.ok || max <= 0) return;
    glPushMatrix(); glTranslatef(H.vx1 - 640.0f, 0, 0);                         /* port extra: against the view's right edge, as the HUD (hud_edges) */
    hud_boss_row(cur, max, t);
    glPopMatrix();
}
static void hud_boss_row(int cur, int max, float t)
{
    const float X = 556, Y = 424, W = 19.0f * max, D = W + (640.0f - X) + 2.0f;   /* +0x10 */
    float x = t < 2.0f ? X + D - t * D / 2.0f : X;
    for (int i = 1; i <= max; i++) {                                                                           /* 0x47ad10 */
        float xi = x - 19.0f * i - 2.0f; float al = (xi - X + W) * 98.0f / W + 30.0f, ar = (xi + 19.0f - X + W) * 98.0f / W + 30.0f;
        if (al < 0) al = 0; if (al > 255) al = 255; if (ar < 0) ar = 0; if (ar > 255) ar = 255;
        sprite_part(14, xi, Y, 1e9f, (uint32_t)al << 24 | 0x808080, (uint32_t)ar << 24 | 0x808080);
    }
    quad(x - 2.0f, Y, 640.0f - X, 15, 0, 0, 0, 0, 0, 0x80ff0000, 0x80ff0000, 0x80ff0000, 0x80ff0000);
    if (t < 2.0f) return;
    const float FX = 559, FY = 400; float g = (t - 2.0f) / 0.2f;
    if (g < 1.0f) { sprite_rect(12, FX + 32.0f * (1 - g), FY + 32.0f * (1 - g), 64.0f * g, 64.0f * g); return; }   /* 0x47bff0 still running: no balls yet */
    sprite(12, FX, FY);                                                                                        /* 0x47aee0 */
    for (int i = 1; i <= cur && i <= max; i++) sprite(13, X - 19.0f * i, Y);                                   /* 0x47afb0 */
}

/* ---------------------------------------------------------------- text box: message 1080 (0x456ed0 / 0x4571c0) */
void hud_text_reset(void) { H.box.state = 0; }

void hud_text_open(int halign, int valign, const uint32_t *ids, int n)
{
    if (!H.ok) return;
    memset(&H.box, 0, sizeof H.box);
    float size = 25;
    for (int i = 0; i < n && i < 3; i++) { if (ids[i] == 0xffffffffu) break; H.box.id[H.box.n++] = ids[i]; }
    for (int i = 0; i < H.box.n; i++) {
        const uint16_t *s = hud_string(H.box.id[i]); if (!s) continue;
        for (;;) { font_size(size); if (font_measure(s) < 640.0f) break; size -= 1; if (size < 17) break; }
    }
    font_size(size); H.box.size = size - 2;                          /* laid out at `size`, drawn 2 smaller (0x456fa7) */
    float cell = font_cell(), tot = H.box.n * cell, y = valign == 0 ? 240 - tot * 0.5f : valign == 1 ? 16 : 480 - tot - 16, top = y, minx = 640, maxx = 0;
    for (int i = 0; i < H.box.n; i++) {
        const uint16_t *s = hud_string(H.box.id[i]); float w = s ? font_measure(s) : 0;
        float x = halign == 0 ? 320 - w * 0.5f : halign == 1 ? 16 : 640 - w - 16;
        if (x < minx) minx = x; if (x + w > maxx) maxx = x + w;
        H.box.x[i] = x; H.box.y[i] = y; y += cell;
        if (H.box.id[i] == 0x20001) top = y;
    }
    H.box.rect[0] = (float)(int)(minx - 16); H.box.rect[1] = (float)(int)(top - 16); H.box.rect[2] = (float)(int)(maxx + 16); H.box.rect[3] = (float)(int)(y + 16);
    H.box.state = 1;
}

void hud_text_draw(int closed, float dt)
{
    if (!H.ok || !H.box.state) return;
    float a = 254;
    H.box.t += dt;
    if (H.box.state == 1) { a = 2 * H.box.t * 254; if (H.box.t >= 0.5f) { H.box.state = 2; a = 254; } }
    else if (H.box.state == 2) { if (closed) { H.box.state = 3; H.box.t = 0; } }
    if (H.box.state == 3) { a = (1 - 2 * H.box.t) * 254; if (H.box.t >= 0.5f) { H.box.state = 0; return; } }
    if (a < 0) a = 0;
    uint32_t col = (uint32_t)a << 24 | 0x808080, bg = (col >> 1) & 0x7f000000;
    quad(H.box.rect[0], H.box.rect[1], H.box.rect[2] - H.box.rect[0], H.box.rect[3] - H.box.rect[1], 0, 0, 0, 0, 0, bg, bg, bg, bg);
    font_size(H.box.size);
    for (int i = 0; i < H.box.n; i++) { const uint16_t *s = hud_string(H.box.id[i]); if (s) font_draw(H.box.x[i], H.box.y[i], s, col); }
}

/* ---------------------------------------------------------------- menu pages (docs/TITLE.md 5, MENU_NEWGAME.md 2, MENU_OPTIONS.md 3)
 * The common page class 0x445e30: 0x4464f0 ticks the blink phase once per frame, draws the page (vt[1], mostly the
 * list 0x446640), then the logo (0x446b00). The page logic itself lives in main_engine.c; these are its pieces. */
void hud_title_reset(void) { H.logo_v = 0; H.menu_t = 0; }
void hud_menu_tick(float dt) { H.menu_t += dt; if (H.menu_t > 0.5f) H.menu_t = 0; }   /* [0x5d7b1c], back to 0 above 0.5 (0x4b39a4) */
void hud_menu_blink(float t) { H.menu_t = t; }                                        /* up: 0, down: 0 or 0.25 (nothing to move to), slider step: 0.25 */
void hud_logo_off(void) { H.logo_v = 0; }                                             /* New game, Load game, pages 0x18/0x19/0x1e/0x1f */

static uint16_t g_row[64]; static int g_rown;
static void row_reset(void) { g_rown = 0; g_row[0] = 0; }
static void row_str(uint32_t ref)
{
    const uint16_t *s = hud_string(ref);
    for (; s && *s && g_rown < 62; s++) g_row[g_rown++] = *s;
    g_row[g_rown] = 0;
}
static void row_space(void)                                          /* Common string 40 is "a a": its middle code is the space glyph */
{
    const uint16_t *s = hud_string(40);
    if (s && s[0] && s[1] && g_rown < 62) { g_row[g_rown++] = s[1]; g_row[g_rown] = 0; }
}
static void row_num(int v)
{
    uint16_t d[16]; int n = number_codes(d, v);
    for (int i = 0; i < n && g_rown < 62; i++) g_row[g_rown++] = d[i];
    g_row[g_rown] = 0;
}
/* the item list 0x446640: one size S for the whole page, shrunk (-1, down to 15) until every item NAME is under 640
 * wide; from y = yfrac * 480 one cell per item. Headers (flag 2) always show, the other items only once the input
 * delay page+8 has run out (`ready`) - the loop stops at the first one. The selected item is left out while the
 * blink phase is under 0.25 s: the original has no cursor and no colour difference. A slider (flag 0x10) reads
 * "name value%" (0x4467e0: the space from string 40, the "%" is string 7) and is centred as a whole. */
void hud_menu_items(const MenuItem *it, int n, float yfrac, int sel, int ready)
{
    if (!H.ok) return;
    float S = 30.0f;                                                  /* 0x4b39a8 */
    for (int i = 0; i < n; i++) {
        const uint16_t *s = hud_string(it[i].id); if (!s) continue;
        while (S > 15.0f) { font_size(S); if (font_measure(s) < 640.0f) break; S -= 1.0f; }
    }
    /* port extra: a page longer than the screen (the Controls page) shrinks until its last row fits; every page of the
     * original already does */
    while (S > 12.0f) { font_size(S); if (yfrac * 480.0f + (n - 1) * font_cell() + S <= 476.0f) break; S -= 1.0f; }
    float y = yfrac * 480.0f;
    for (int i = 0; i < n; i++) {
        if (!(it[i].flags & 2) && !ready) break;
        font_size(it[i].flags & 0x20 ? S * 0.8f : S);
        row_reset(); row_str(it[i].id);
        if (it[i].flags & 0x10) { row_space(); row_num(it[i].value); row_str(7); }
        if (it[i].flags & 0x100) { row_space(); row_str((uint32_t)it[i].value); }   /* port extra: a choice, "name value-string" (docs/DISPLAY.md 4) */
        float w = font_measure(g_row);
        float x = (it[i].flags & 4) ? 640.0f - w - 6.4f : (it[i].flags & 8) ? 6.4f : (it[i].flags & 0x80) ? 160.0f - w * 0.5f : 320.0f - w * 0.5f;
        if (!(i == sel && H.menu_t < 0.25f)) font_draw(x, y, g_row, 0xff808080);
        y += font_cell();                                             /* CellH 0x441980 + Extra 0x441a50 (0) */
    }
    font_size(17.0f);
}

void hud_menu_page(const uint32_t *ids, int n, float yfrac, int sel, float dt)
{
    MenuItem it[8]; if (n > 8) n = 8;
    for (int i = 0; i < n; i++) { it[i].id = ids[i]; it[i].flags = 1; it[i].value = 0; }
    hud_menu_tick(dt);
    hud_menu_items(it, n, yfrac, sel, 1);
}

/* the logo 0x446b00, drawn after the page, only in House: level bank image 1, source 0,0,209,247 at (216,16) with
 * alpha trunc(50.8 v); pages 0 and 1 first add 10 dt (0x446ac0, up to 5), and every frame takes 5 dt off after the
 * draw. Net: 1 s in on pages 0 and 1, 1 s out on every other page. */
void hud_logo(int grow, float dt)
{
    if (!H.ok) return;
    if (grow) { H.logo_v += 10 * dt; if (H.logo_v > 5) H.logo_v = 5; }
    if (H.logo && H.logo_v > 0) {
        uint32_t c = (uint32_t)(50.8f * H.logo_v) << 24 | 0x808080;
        quad(216, 16, 209, 247, H.logo, 0, 0, 209.0f / H.logo_w, 247.0f / H.logo_h, c, c, c, c);
    }
    H.logo_v -= 5 * dt; if (H.logo_v < 0) H.logo_v = 0;
}

/* ---------------------------------------------------------------- the credits, menu page 0x20 (docs/CREDITS.md)
 * Page class vtable 0x4aa474 (0x20 B), enter 0x45bd60, draw 0x45bd90 + the roll 0x453930. The roll is the table 0x4b3d28
 * (253 records of 24 B {ref, float scale, colour 0xfeffffff, w, h, flags}, list object 0x5e59f4 vtable 0x4ab228): T = text,
 * level string `idx` at size 26 * s/10 (0x4ab23c); I = image, level image `idx` drawn w x h * s/10. Flags 0x40 = centred on
 * x 480 (text 464, 0x453ab1 takes 16 off), 8 = right-aligned to 624; the other placements (0x10, 4, 0x20) are unused. */
#define T(i, s, f) { i, s, 1 | f, 0, 0 }
#define I(i, s, w, h, f) { i, s, 2 | f, w, h }
static const struct { uint16_t idx; uint8_t s10, fl; uint16_t w, h; } k_cred[253] = {
    T(0,10,0x40), T(1,10,0x40), T(2,10,0x40), T(3,10,0x40), I(3,10,256,256,0x40), T(5,10,0x40), T(6,6,0x40), T(7,6,0x40), T(8,10,0x40), I(2,10,128,128,0x40), T(9,10,0x40), T(10,8,0x40),
    T(11,8,0x40), T(12,10,0x40), T(13,10,0x40), T(14,10,0x40), T(15,12,0x8), T(16,10,0x8), T(17,8,0x8), T(18,6,0x8), T(19,10,0x8), T(20,8,0x8), T(21,6,0x8), T(22,10,0x8),
    T(23,8,0x8), T(24,6,0x8), T(25,6,0x8), T(26,6,0x8), T(27,10,0x8), T(28,8,0x8), T(29,6,0x8), T(30,6,0x8), T(31,6,0x8), T(32,6,0x8), T(33,6,0x8), T(34,6,0x8),
    T(35,6,0x8), T(36,6,0x8), T(37,6,0x8), T(38,10,0x8), T(39,12,0x8), T(40,10,0x8), T(41,8,0x8), T(42,6,0x8), T(43,10,0x8), T(44,8,0x8), T(45,6,0x8), T(46,10,0x8),
    T(47,10,0x8), T(48,10,0x8), T(49,8,0x8), T(50,6,0x8), T(51,6,0x8), T(52,10,0x8), T(53,8,0x8), T(54,6,0x8), T(55,10,0x8), T(56,8,0x8), T(57,6,0x8), T(58,6,0x8),
    T(59,6,0x8), T(60,10,0x8), T(61,8,0x8), T(62,6,0x8), T(63,6,0x8), T(64,6,0x8), T(65,10,0x8), T(66,8,0x8), T(67,6,0x8), T(68,6,0x8), T(69,6,0x8), T(70,6,0x8),
    T(71,6,0x8), T(72,6,0x8), T(73,6,0x8), T(74,6,0x8), T(75,10,0x8), T(76,8,0x8), T(77,6,0x8), T(78,10,0x8), T(79,8,0x8), T(80,6,0x8), T(81,10,0x8), T(82,10,0x8),
    T(83,10,0x8), T(84,8,0x8), T(85,6,0x8), T(86,10,0x8), T(87,6,0x8), T(88,6,0x8), T(89,10,0x8), T(90,8,0x8), T(91,6,0x8), T(92,10,0x8), T(93,10,0x8), T(94,10,0x8),
    T(95,8,0x8), T(96,6,0x8), T(97,6,0x8), T(98,6,0x8), T(99,6,0x8), T(100,10,0x8), T(101,10,0x8), T(102,6,0x8), T(103,6,0x8), T(104,10,0x8), T(105,10,0x8), T(106,8,0x8),
    T(107,6,0x8), T(108,10,0x8), T(109,8,0x8), T(110,6,0x8), T(111,6,0x8), T(112,10,0x8), T(113,8,0x8), T(114,6,0x8), T(115,6,0x8), T(116,10,0x8), T(117,8,0x8), T(118,10,0x8),
    T(119,8,0x8), T(120,6,0x8), T(121,8,0x8), T(122,6,0x8), T(123,8,0x8), T(124,6,0x8), T(125,10,0x8), T(126,8,0x8), T(127,6,0x8), T(128,6,0x8), T(129,10,0x8), T(130,10,0x8),
    T(131,6,0x8), T(132,6,0x8), T(133,10,0x8), T(134,10,0x8), T(135,10,0x8), T(136,8,0x8), T(137,6,0x8), T(138,6,0x8), T(139,10,0x8), T(140,8,0x8), T(141,6,0x8), T(142,8,0x8),
    T(143,6,0x8), T(144,10,0x8), T(145,8,0x8), T(146,6,0x8), T(147,10,0x8), T(148,6,0x8), T(149,6,0x8), T(150,6,0x8), T(151,10,0x8), T(152,10,0x8), T(153,6,0x8), T(154,6,0x8),
    T(155,6,0x8), T(156,6,0x8), T(157,10,0x8), T(158,10,0x8), T(159,10,0x8), T(160,8,0x8), T(161,6,0x8), T(162,10,0x8), T(163,8,0x8), T(164,6,0x8), T(165,10,0x8), T(166,8,0x8),
    T(167,6,0x8), T(168,6,0x8), T(169,10,0x8), T(170,8,0x8), T(171,6,0x8), T(172,6,0x8), T(173,6,0x8), T(174,6,0x8), T(175,10,0x8), T(176,8,0x8), T(177,6,0x8), T(178,10,0x8),
    T(179,12,0x8), T(180,10,0x8), T(181,10,0x8), T(182,10,0x8), T(183,10,0x8), T(184,6,0x8), T(185,8,0x8), T(186,6,0x8), T(187,8,0x8), T(188,6,0x8), T(189,10,0x8), T(190,8,0x8),
    T(191,6,0x8), T(192,6,0x8), T(193,6,0x8), T(194,6,0x8), T(195,10,0x8), T(196,8,0x8), T(197,6,0x8), T(198,6,0x8), T(199,10,0x8), T(200,10,0x8), T(201,10,0x8), T(202,8,0x8),
    T(203,6,0x8), T(204,8,0x8), T(205,6,0x8), T(206,8,0x8), T(207,6,0x8), T(208,6,0x8), T(209,6,0x8), T(210,6,0x8), T(211,6,0x8), T(212,10,0x8), T(213,6,0x8), T(214,6,0x8),
    T(215,6,0x8), T(216,6,0x8), T(217,6,0x8), T(218,6,0x8), T(219,6,0x8), T(220,6,0x8), T(221,6,0x8), T(222,6,0x8), T(223,6,0x8), T(224,6,0x8), T(225,6,0x8), T(226,6,0x8),
    T(227,6,0x8), T(228,6,0x8), T(229,6,0x8), T(230,6,0x8), T(231,6,0x8), T(232,6,0x8), T(233,6,0x8), T(234,6,0x8), T(235,6,0x8), T(236,6,0x8), T(237,6,0x8), T(238,6,0x8),
    T(239,8,0x8), T(240,10,0x8), T(241,12,0x8), T(242,10,0x8), T(243,8,0x8), T(244,6,0x8), T(245,10,0x8), T(246,8,0x8), T(247,6,0x8), T(248,10,0x8), T(249,6,0x8), T(250,10,0x8),
    T(251,8,0x8),
};
#undef T
#undef I
static struct { int img; float t_img, off; } g_cr;             /* page +0x14 (image 0..2), +0x1c (its clock); the roll's offset [0x5e59fc] */
void hud_credits_enter(void) { memset(&g_cr, 0, sizeof g_cr); }  /* 0x45bd60: 0x4538f0(0) = this roll from its start, +0x14 = +0x1c = 0 */
void hud_gameover(void)                                         /* 0x45bbd0 */
{
    if (!H.ok) return;
    hud_rect(0xfe000000);                                        /* black panel, 640 x 480 */
    const uint16_t *s = hud_string(56);                          /* 0x20038 = Common 56 "GAME OVER", size 35, at (320 - w/2, 240 - h/2), h = 0 on one line */
    font_size(35.0f); if (s) font_draw(320.0f - font_measure(s) * 0.5f, 240.0f, s, 0xfeffffff);
}
void hud_credits(int prev_level, float dt)                     /* 0x45bd90, dt = the menu's [0x4b39a0] (the roll reads [[0x509adc]+0x38], the same frame time) */
{
    static const uint8_t img[9] = { 4, 5, 6, 7, 8, 9, 10, 11, 12 };   /* 0x4b5df8: three pictures per character */
    if (!H.ok) return;
    hud_rect(0xfe000000);                                        /* 0x45bdda: black panel, 640 x 480, blank surface */
    g_cr.t_img += dt; if (g_cr.t_img > 10.0f) { g_cr.t_img = 0; if (++g_cr.img == 3) g_cr.img = 0; }   /* 0x45bdeb: a new picture every 10 s */
    float v = g_cr.t_img; if (v > 9.0f) v = 1.0f - (v - 9.0f);   /* 1 s in, 8 s on, 1 s out */
    if (v > 1.0f) v = 1.0f; else if (v < 0.0f) v = 0.0f;
    int a = (int)(v * 254.0f), set = prev_level >= 11 && prev_level <= 17 ? 3 : prev_level >= 18 && prev_level <= 24 ? 6 : 0;   /* 0x45be7c: app+0x6c - 0xb, byte table 0x45bf94 */
    int k = img[set + g_cr.img]; uint32_t c = (uint32_t)a << 24 | 0x808080;
    if (a >= 2 && H.limg[k]) quad(32, 112, 256, 256, H.limg[k], 0, 0, 256.0f / H.limg_w[k], 256.0f / H.limg_h[k], c, c, c, c);   /* 0x45bee3, flag 8; RectVirtual skips alpha < 2 */
    const uint16_t *s = hud_string(131);                         /* 0x45bee8: Common 131 "THE END", size 35, at (160 - w/2, 360 - h/2), h = 0 on one line */
    font_size(35.0f); if (s) font_draw(160.0f - font_measure(s) * 0.5f, 360.0f, s, 0xfeffffff);
    g_cr.off += dt * 50.0f;                                      /* 0x453930: the roll climbs 50 units/s from y = 480 */
    float y = 480.0f - g_cr.off; int any = 0;
    for (int i = 0; i < 253; i++) {
        int draw = !(y < -480.0f); if (draw) any = 1;             /* 0x4539b5: drawn from y -480 on (off screen), ... */
        if (y > 496.0f) break;                                   /* ... and the loop stops at the first record below 496 */
        float sc = k_cred[i].s10 / 10.0f; int fl = k_cred[i].fl;
        if (fl & 1) {
            font_size(sc * 26.0f);
            const uint16_t *t = hud_string(0x01020000u | k_cred[i].idx); float w = t ? font_measure(t) : 0;
            float x = (fl & 0x10) ? 320.0f - w * 0.5f : (fl & 4) ? 16.0f : (fl & 8) ? 624.0f - w : (fl & 0x20) ? 176.0f - w * 0.5f : 464.0f - w * 0.5f;   /* 0x453a2f */
            if (draw && t) font_draw(x, y, t, 0xfeffffff);
            y += font_cell();                                    /* 0x441980 + 0x441a50 (extra spacing 0) */
        }
        if (fl & 2) {
            int w = (int)(k_cred[i].w * sc), h = (int)(k_cred[i].h * sc), n = k_cred[i].idx < 16 ? k_cred[i].idx : 0;
            int x = (fl & 0x10) ? 320 - w / 2 : (fl & 4) ? 16 : (fl & 8) ? 624 - w : (fl & 0x20) ? (int)(160.0f - w * 0.5f) : (int)(480.0f - w * 0.5f);   /* 0x453b2b: no -16 here */
            if (draw && H.limg[n]) quad((float)x, (float)(int)y, (float)w, (float)h, H.limg[n], 0, 0, (k_cred[i].w - 1.0f) / H.limg_w[n], (k_cred[i].h - 1.0f) / H.limg_h[n], 0xff808080, 0xff808080, 0xff808080, 0xff808080);   /* source 0,0,w-1,h-1 */
            y += (float)h;                                       /* + 2 * 0x441a50 = 0 */
        }
    }
    if (!any) g_cr.off = 0;                                      /* 0x453c43: everything above -480: the roll starts again from the bottom */
    font_size(17.0f);
}

/* the iris 0x4776d0 (docs/MENU_NEWGAME.md 2.7): an opaque black ring of 50 segments around (320, 240), inner radius
 * 0.99 * 480 * v, outer 0.99 * 480 - the corners are 400 away, so v = 0.85 shows nothing and v = 0 is all black.
 * 0x482cf0 clips to the virtual screen; the viewport does that here. The original ran at 4:3; on another window shape
 * the stretched virtual screen would make an ellipse, so the ring stays round and takes the larger of the two scales
 * (v = 1 still opens it completely). */
void hud_iris(float v)
{
    if (!H.ok) return;
    float r = 0.99f * 480.0f, kx = H.iris_kx ? H.iris_kx : 1, ky = H.iris_ky ? H.iris_ky : 1;
    const float ri = r * v;
    { float hw = H.vx1 - 320.0f, c = sqrtf(hw * hw + 240.0f * 240.0f) + 2.0f; if (c > r) r = c; }   /* a wide view (port extra): the ring reaches its corners ... */
    if (v >= 1.0f) return;                                                                            /* ... and v = 1 is open on any view (at 4:3 the ring is then empty anyway) */
    glDisable(GL_TEXTURE_2D); glColor4f(0, 0, 0, 1); glBegin(GL_QUADS);
    for (int k = 0; k < 50; k++) {
        float a0 = k * (6.2831853f / 50), a1 = (k + 1) * (6.2831853f / 50), c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
        glVertex2f(320 + ri * kx * c0, 240 + ri * ky * s0); glVertex2f(320 + ri * kx * c1, 240 + ri * ky * s1);
        glVertex2f(320 + r * kx * c1, 240 + r * ky * s1);   glVertex2f(320 + r * kx * c0, 240 + r * ky * s0);
    }
    glEnd();
}

/* a flat colour over the whole virtual screen: 0x80000000 is the half-black backdrop of a menu page in a level (0x404f1a) */
void hud_rect(uint32_t argb) { if (H.ok) quad(H.vx0, 0, H.vx1 - H.vx0, 480, 0, 0, 0, 0, 0, argb, argb, argb, argb); }   /* the whole view, also the sides of a wide one */
/* the BlackBox texts (docs/BLACKBOX.md 6): SetSize 0x441a60(size), font+0x60 = col, Font_Draw 0x43f890 with the pen at x, y (top of
 * the cell); ref = a Common string, or 0 for the digits of num (0x441820). Returns the pen x after the text. */
float hud_pen_text(float x, float y, float size, uint32_t col, uint32_t ref, int num)
{
    uint16_t buf[16]; const uint16_t *s = buf;
    if (!H.ok) return x;
    if (ref) s = hud_string(ref); else number_codes(buf, num);
    if (!s) return x;
    font_size(size); font_draw(x, y, s, col); x += font_measure(s); font_size(17.0f);
    return x;
}
/* port extra (docs/DISPLAY.md 3): black outside the view box vx, vy, vw, vh (GL origin bottom left) - the pillar- or letterbox bars */
void hud_bars(int win_w, int win_h, int vx, int vy, int vw, int vh)
{
    if (vx <= 0 && vy <= 0 && vw >= win_w && vh >= win_h) return;
    glViewport(0, 0, win_w, win_h); glEnable(GL_SCISSOR_TEST); glClearColor(0, 0, 0, 1);
    int r[4][4] = { { 0, 0, vx, win_h }, { vx + vw, 0, win_w - vx - vw, win_h }, { 0, 0, win_w, vy }, { 0, vy + vh, win_w, win_h - vy - vh } };
    for (int i = 0; i < 4; i++) if (r[i][2] > 0 && r[i][3] > 0) { glScissor(r[i][0], r[i][1], r[i][2], r[i][3]); glClear(GL_COLOR_BUFFER_BIT); }
    glDisable(GL_SCISSOR_TEST);
}

static void fit_size(const uint16_t *s, float S, float maxw, float minS) { font_size(S); while (S > minS && font_measure(s) > maxw) font_size(S -= 1.0f); }   /* 0x45dc90 */
static void sheet_quad(float x, float y, float w, float h, float sx, float sy, float sw, float sh, uint32_t c, int additive)
{
    if (!H.sheet) return;
    if (additive) glBlendFunc(GL_ONE, GL_ONE);                        /* flag 4: the neon panel and the ring have alpha 0 in the data */
    quad(x, y, w, h, H.sheet, sx / H.sheet_w, sy / H.sheet_h, (sx + sw) / H.sheet_w, (sy + sh) / H.sheet_h, c, c, c, c);
    if (additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

/* the save-slot list of pages 2 and 5 (docs/MENU_LOAD.md 2.2, 3): four panels in the corners, sliding in from the
 * sides, and the page title (25 "Select game" / 24 "Select save") coming up from below. */
void hud_slot_list(const HudSlots *s, float dt)
{
    static const float SX[4] = { 16, 488, 16, 488 }, SY[4] = { 48, 48, 324, 324 };   /* table 0x4ab420 */
    static float blink;                                                                /* page+0x40 */
    if (!H.ok) return;
    blink += dt; if (blink > 0.6f) blink = 0;
    for (int i = 0; i < 4; i++) {                                                      /* 0x45d530(i + 1, xoff) */
        int sel = s->sel == i + 1; uint32_t c = sel ? 0xfe808080 : 0xfe202020, tc = c;
        float x = SX[i] + ((i & 1) ? -s->slide : s->slide), y = SY[i];
        if (!sel) sheet_quad(x, y, 137, 108, 0, 0, 137, 108, c, 1);
        else if (blink > 0.3f) sheet_quad(x, y, 137, 108, 0, 0, 137, 108, 0xfe808080, 1);
        else tc = 0xfe202020;
        sheet_quad(x + 45, y + 50, 49, 49, 0, 108, 49, 49, c, 1);                     /* the gold ring */
        if (s->pct[i] && H.img[0]) {                                                   /* the faces: Common image 61, alpha (flag 8) */
            float W = (float)H.img_w[0], Hh = (float)H.img_h[0];
            quad(x + 33, y - 7, 64, 64, H.img[0], 0, 0, 64 / W, 64 / Hh, c, c, c, c);                                   /* Woody */
            if (s->open[i] & 1) quad(x - 4, y + 8, 64, 64, H.img[0], 0, 63 / Hh, 64 / W, 127 / Hh, c, c, c, c);          /* Knothead */
            if (s->open[i] & 2) quad(x + 69, y + 7, 64, 64, H.img[0], 63 / W, 0, 127 / W, 64 / Hh, c, c, c, c);         /* Splinter */
        }
        row_reset(); row_str(s->pct[i] ? 26 + i : 18);                                /* "Save N" / "FREE" (0x45d3c0) */
        fit_size(g_row, 28, 137, 10);
        float w = font_measure(g_row), ly = i < 2 ? SY[i] + 108 : SY[i] - font_cell();
        font_draw(x + 68.5f - w * 0.5f, ly, g_row, tc);
        row_reset(); row_num(s->pct[i]); row_str(7);                                    /* "NN%", red, also "0%" on a free slot */
        font_size(s->pct[i] >= 100 ? 12.0f : 16.0f);
        font_draw(x + 69.5f - font_measure(g_row) * 0.5f, y + 75 - font_cell() * 0.5f, g_row, sel ? 0xfeff1400 : 0xfe3f0500);
        if (s->cross && !s->pct[i]) sheet_quad(x + 20, y, 108, 108, 0, 160, 63, 63, 0xfe808080, 0);   /* page 2: the red cross over a free slot (0x45e050) */
    }
    row_reset(); row_str(s->title);
    fit_size(g_row, 30, 330, 10);
    float w = font_measure(g_row), h = font_cell(), x = 320 - w * 0.5f, y = 415 - s->slide;
    font_draw(x, y, g_row, 0xfe808080);                                                /* first plain, then orange-red slightly up and left over it */
    font_draw(x - 0.05f * h, y - 0.05f * h, g_row, 0xfe801400);
    font_size(17.0f);
}

/* ---------------------------------------------------------------- page 3, the world-select carousel (docs/MENU_LOAD.md 4.6) */
static void row_centred(float cx, float y, uint32_t col) { font_draw(cx - font_measure(g_row) * 0.5f, y, g_row, col); }
void hud_carousel(const HudCarousel *c)
{
    if (!H.ok) return;
    float off = c->off;
    if (c->stats) {
        /* 0x45f6f0: the portrait, the lives plate, the health dots (0x45ea00), $ and charge icons with their plates */
        sprite(c->face, 16 + off, 113); sprite(8, 56 + off, 153);
        for (int i = 0; i < (int)c->health; i++) sprite(7, 31 + off + 24.0f * i, 78);
        sprite(3, 22 + off, 213); sprite(8, 56 + off, 258); sprite(6, 22 + off, 308); sprite(8, 56 + off, 353);
        /* 0x45eb00 -> 0x45f2a0: red, size 17 centred on the plates; a value >= 100 sets 30 x 0.75 and that size stays for the next ones */
        const int v[3] = { c->lives - 1, c->unique, c->charges }; const float cy[3] = { 169, 274, 369 }; float S = 17.0f;   /* 16 below each plate */
        for (int i = 0; i < 3; i++) {
            uint16_t s[16]; number_codes(s, v[i]); if (v[i] >= 100) S = 22.5f; font_size(S);
            font_draw(72 + off - font_measure(s) * 0.5f, cy[i] - font_cell() * 0.5f, s, 0xfeff0000);
        }
        /* 0x45f790(-off): centred on x 565 + off', every line shrunk to <= 115 wide */
        float cx = 565 - off;
        row_reset(); row_str(43); fit_size(g_row, 20, 115, 10); row_centred(cx, 110, 0xfeffffff);          /* "Game cleared" */
        float y = 110 + font_cell();
        row_reset(); row_num(c->pct); row_str(7); fit_size(g_row, 50, 115, 10); row_centred(cx, y, 0xfeff1400);   /* "NN%" */
        if (c->pct < 100) {
            row_reset(); row_str(44); fit_size(g_row, 20, 115, 10); row_centred(cx, 240, 0xfeffffff);      /* "Location" */
            row_reset(); if (c->world) row_str(c->world); fit_size(g_row, 25, 115, 10); row_centred(cx, 270, 0xfeff1400);
            y = 270 + font_cell();
            row_reset(); if (c->part) row_str(c->part); row_centred(cx, y, 0xfeff1400);                                 /* measured at the world's size, no fit of its own */
        }
    }
    if (c->list && c->items) hud_menu_items(c->items, c->nitems, c->yfrac, c->list_sel, 1);   /* 0x446640 */
    {   /* 0x45fe30: size 30, the grey copy 5 % of a cell right and down, the orange-red one on top */
        row_reset(); row_str(c->name); font_size(30.0f);
        float w = font_measure(g_row), h = font_cell(), x = 320 - w * 0.5f, y = 16 + off;
        font_draw(x + 0.05f * h, y + 0.05f * h, g_row, 0xfe808080);
        font_draw(x, y, g_row, 0xfe801400);
    }
    {   /* 0x45fac0: HUD sprite 15 (Common image 63, 0,96,31,31) doubled to 62x62, additive (flag 4); the left one mirrored */
        int i = k_spr[15].img;
        if (H.img[i]) {
            float W = (float)H.img_w[i], Hh = (float)H.img_h[i], u0 = (k_spr[15].x + 0.5f) / W, v0 = (k_spr[15].y + 0.5f) / Hh, u1 = (k_spr[15].x + k_spr[15].w - 0.5f) / W, v1 = (k_spr[15].y + k_spr[15].h - 0.5f) / Hh;   /* half a texel in: the row above is opaque white, which the bilinear filter smeared into a line over the arrow at 2x */
            uint32_t gr = (uint32_t)(int)(c->arrow_r + 0.5f) & 255, gl = (uint32_t)(int)(c->arrow_l + 0.5f) & 255;
            uint32_t cr = 0xfe000000u | gr << 16 | gr << 8 | gr, cl = 0xfe000000u | gl << 16 | gl << 8 | gl;
            glBlendFunc(GL_ONE, GL_ONE);
            quad(470 + c->arrow_s, 209, 62, 62, H.img[i], u0, v0, u1, v1, cr, cr, cr, cr);
            quad(108 - c->arrow_s, 209, 62, 62, H.img[i], u1, v0, u0, v1, cl, cl, cl, cl);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
    }
    {   /* 0x45e8a0: 130 "Total Score :" size 15 at (16, 410), the sum 0x450a10 one cell below, both white */
        row_reset(); row_str(130); font_size(15.0f); font_draw(16, 410, g_row, 0xfeffffff);
        row_reset(); row_num(c->total); font_draw(16, 410 + font_cell(), g_row, 0xfeffffff);
    }
    font_size(17.0f);
}

/* ---------------------------------------------------------------- results screen, menu page 0x1e (docs/RESULTS.md)
 * The page object of vtable 0x4aa934: no panel, no backdrop and no OK item. Once shown (0x454560) a black iris closes
 * round the centre of the screen (1.0 -> 0.37 in 0.5 s), "RESULTS" slides in from the left, "HIGH SCORE" from the right
 * and the level name with "CLEARED!!" from below; then a column of lines on the left counts up one after the other
 * (0x454700 / race 0x454860): time, "+", enemies, "+", W bonuses, TOTAL, $. Each line slides in from -200 in 0.2 s,
 * then "=" on x 85 and a number counting up at 5000 points/s, right aligned so that the final value starts at x 100;
 * the time at which a line is done is the start of the next one. */
static struct {
    int shown, iris_on;                         /* +0x5c, +0x38 */
    float t, delay;                             /* +0x28 (time since enter / show / hide), +8 (input delay) */
    float iv0, iv1, it;                         /* iris 0x4776b0: from, to, time */
    float clock, end[6];                        /* +0x3c, +0x40 (start of line 0) and +0x44..+0x54 (-1 = not done) */
    int counting;                               /* a counter asked for the tick loop this frame (0x468e40) */
} RS;
void hud_results_enter(void)                    /* 0x4544b0 (+ 0x45b8c0: SoundFx 0x3f and the 0.5 s input delay are the caller's / here) */
{
    memset(&RS, 0, sizeof RS); RS.delay = 0.5f; RS.iv0 = RS.iv1 = 1.0f;
    for (int i = 1; i < 6; i++) RS.end[i] = -1.0f;
}
void hud_results_show(void) { if (RS.shown) return; RS.shown = 1; RS.t = 0; RS.delay = 0.5f; RS.iv0 = 1.0f; RS.iv1 = 0.37f; RS.it = 0; RS.iris_on = 1; }   /* 0x454560 -> 0x4544f0 */
void hud_results_hide(void) { if (!RS.shown) return; RS.shown = 0; RS.t = 0; RS.delay = 0.5f; RS.iv0 = 0.37f; RS.iv1 = 1.0f; RS.it = 0; RS.iris_on = 1; }   /* 0x454580 -> 0x454530 */
int hud_results_confirm(void)                   /* 0x4545a0; 1 = everything has been counted (result 5) */
{
    if (RS.delay > 0) return 0;
    if (RS.end[5] > -1) return RS.shown;
    if (RS.t > 0.5f) { float k = RS.clock; RS.clock += 60.0f; for (int i = 0; i < 6; i++) RS.end[i] = k; }   /* still counting: all lines done at once */
    return 0;
}

static const float k_res_row[9][2] = { { 16, 16 }, { 45, 106 }, { 45, 215 }, { 45, 341.44f }, { 45, 291.44f }, { 45, 375 }, { 85, 405 }, { 624, 16 }, { 320, 415 } };   /* 0x4b5748 (0x4543c0) */
static void res_text(float x, float y, uint32_t col, int shadow)                  /* g_row; the shadow is the same text in white 0.05 cell down right, first */
{
    if (shadow) { float d = 0.05f * font_cell(); font_draw(x + d, y + d, g_row, 0xfe808080); }
    font_draw(x, y, g_row, col);
}
static void res_icon(int n, float xoff)                                           /* the six icons of 0x4549c0 (table 0x454f08), positions from 0x4542f0, all centred on x 45 */
{
    /* every rect goes to RectVirtual 0x480a10 as ints: fistp (round to nearest even, control word 0x007F) of x + xoff, y, w, h (0x454dd2..0x454e25) */
    switch (n) {
    case 0: case 1:                                                               /* clock (51, 0, 36, 36) -> (27, 75) / enemy face (0, 0, 50, 50) -> (20, 170): hub bank image 1 (0x4ab260 / 0x4ab274 = 0x01010001), additive (flag 4) */
        if (!H.logo) break;
        { float sx = n ? 0 : 51, sw = n ? 50 : 36, x = n ? 20 : 27, y = n ? 170 : 75;
          glBlendFunc(GL_ONE, GL_ONE);
          quad((float)lrintf(x + xoff), y, sw, sw, H.logo, sx / H.logo_w, 0, (sx + sw) / H.logo_w, sw / H.logo_h, 0xfe808080, 0xfe808080, 0xfe808080, 0xfe808080);
          glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
        break;
    case 2: sprite_rect(4, (float)lrintf(9.28f + xoff), 275, 71, 71); break;     /* the big W: bank 0 image 62 (0, 0, 94, 94) at 45 - 94 * 0.38 = 9.28, 94 * 0.76 = 71.44 -> 71, flag 8 */
    case 3: sprite_rect(5, (float)lrintf(9.28f + xoff), 225, 71, 71); break;     /* the flag (race): image 63, same size */
    case 4: glBlendFunc(GL_ONE, GL_ONE); quad((float)lrintf(16 + xoff), 370, 140, 2, 0, 0, 0, 0, 0, 0xfe808080, 0xfe808080, 0xfe808080, 0xfe808080); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;   /* the bar above TOTAL: no texture, (16, 370, 140, 2), flag 4 */
    case 5: sprite_rect(3, (float)lrintf(20.68f + xoff), 410, 49, 49); break;    /* $: image 61 (63, 63, 64, 64) at 45 - 64 * 0.38 = 20.68, 64 * 0.76 = 48.64 -> 49, flag 8 */
    }
}
static float res_slide(float start) { float d = RS.clock - start; return d < 0.2f ? (0.2f - d) * -1000.0f : 0; }   /* 0x454f20 */
static void res_line(float xoff, int n, float size, uint32_t col, int shadow)    /* 0x4549c0: icon n, the label in g_row (empty: none) on row n + 1 */
{
    font_size(size); res_icon(n, xoff);
    if (g_rown) res_text(45 - font_measure(g_row) * 0.5f + xoff, k_res_row[n + 1][1], col, shadow);
}
static float res_count(float start, int value, int row, float size, uint32_t col, int shadow)   /* 0x454f60: "=" and the counter; the time it is done or -1 */
{
    if (start + 0.2f > RS.clock) return -1.0f;
    font_size(size);
    const float x = k_res_row[6][0], y = k_res_row[row][1];
    row_reset(); row_str(8); font_draw(x - font_measure(g_row) * 0.5f, y, g_row, 0xfe808080);
    row_reset(); row_num(value); float right = x + font_measure(g_row) + 15.0f;
    int n = (int)lrintf((RS.clock - start - 0.2f) * 5000.0f); float done = -1.0f;
    if (n > value) { n = value; done = RS.clock; }
    row_reset(); row_num(n); res_text(right - font_measure(g_row), y, col, shadow);
    if (done == -1.0f) RS.counting = 1;
    return done;
}
static void res_plus(int k) { font_size(15.0f); row_reset(); row_str(11); font_draw(45 - font_measure(g_row) * 0.5f, k_res_row[k][1] + 30.0f, g_row, 0xfe808080); }   /* 0x454920 */
static void res_ratio(int a, int b) { row_reset(); row_num(a); row_str(9); row_num(b); }                                              /* "a/b" */
static int res_bonus(int got, int total) { int v = got * 100; return got == total ? v + v / 2 : v; }                                 /* 0x453d50 / 0x453d20 */
static float res_total(const HudResults *r, float start)                          /* 0x455580 */
{
    float S = 15.0f; const uint16_t *s = hud_string(14);
    if (s) { fit_size(s, S, 65.0f, 10.0f); S = H.k * (H.H - H.B); }
    row_reset(); row_str(14); res_line(res_slide(start), 4, S, 0xfeff0000, 1);
    int v = r->race ? res_bonus(r->st[3], r->st[1]) : (r->time < 1800 ? 1800 - (int)r->time : 0) * 10 + res_bonus(r->st[2], r->st[0]) + res_bonus(r->st[3], r->st[1]);
    return res_count(start, v, 5, S, 0xfe808080, 0);
}
static float res_dollar(const HudResults *r, float start)                          /* 0x455650: the number of new unique items, big and red */
{
    row_reset(); res_line(res_slide(start), 5, 15.0f, 0xfe808080, 0);            /* 0x455690: icon 5, no label (str 0), size 15 */
    if (start + 0.2f <= RS.clock) { font_size(40.0f); row_reset(); row_str(8); font_draw(k_res_row[6][0] - font_measure(g_row) * 0.5f, k_res_row[6][1], g_row, 0xfe808080); }
    return res_count(start + 0.4f, r->cats, 6, 40.0f, 0xfeff0000, 1);
}
static float res_time(const HudResults *r, float start)                          /* 0x455160: "m:ss" (t = _ftol(app+0x84), a leading 0 below 10 s, ":" = string 10), value 0x453d70 */
{
    int t = (int)r->time;                                                         /* _ftol */
    row_reset(); row_num(t / 60); row_str(10); if (t % 60 < 10) row_num(0); row_num(t % 60);
    res_line(res_slide(start), 0, 15.0f, 0xfe808080, 0);
    return res_count(start, (t < 1800 ? 1800 - t : 0) * 10, 1, 15.0f, 0xfe808080, 0);
}
static float res_ratio_line(int got, int tot, float start, int icon, int row)     /* 0x4552b0 (enemies, icon 1, row 2), 0x4553a0 (W, 2, 3), 0x455490 (flag, 3, 4): "got/tot" (itoa, string 9 "/") */
{
    res_ratio(got, tot); res_line(res_slide(start), icon, 15.0f, 0xfe808080, 0);
    return res_count(start, res_bonus(got, tot), row, 15.0f, 0xfe808080, 0);
}
static void res_lines(const HudResults *r, float dt)                              /* 0x454700 (normal) / 0x454860 (race) */
{
    RS.clock += dt;                                                               /* [0x509adc]+0x38 */
    float *e = RS.end;                                                            /* e[0] = +0x40 .. e[5] = +0x54 */
    if (!r->race) {
        /* each line runs every frame; its result is stored only while the slot is still -1 (0x454716 / 0x454777 / 0x4547c6 / 0x454802).
         * The "+" between the lines (0x454920) is drawn in the else branch, i.e. only from the frame AFTER the line above was done */
        if (e[1] == -1.0f) e[1] = res_time(r, e[0]); else { res_plus(1); res_time(r, e[0]); }
        if (e[1] > -1.0f) { if (e[2] == -1.0f) e[2] = res_ratio_line(r->st[2], r->st[0], e[1], 1, 2); else { res_plus(2); res_ratio_line(r->st[2], r->st[0], e[1], 1, 2); } }
        if (e[2] > -1.0f) { float d = res_ratio_line(r->st[3], r->st[1], e[2], 2, 3); if (e[3] == -1.0f) e[3] = d; }
    } else {                                                                      /* the flag line starts at +0x48 = -1: no slide, the counter is as good as done */
        float d = res_ratio_line(r->st[3], r->st[1], e[2], 3, 4); if (e[3] == -1.0f) e[3] = d;
    }
    if (e[3] > -1.0f) { float d = res_total(r, e[3]); if (e[4] == -1.0f) e[4] = d; }
    if (e[4] > -1.0f) e[5] = res_dollar(r, e[4]);                                 /* 0x454848 / 0x454909: +0x54 is overwritten EVERY frame (the clock of this frame once done) */
}
static void res_name(int level, uint32_t *a, uint32_t *b)                        /* 0x4559b0, table 0x455b60: 47 Space / 48 Pirate / 49 House / 50 Mini Game, 51..54 Part A..D, 55 Race */
{
    static const unsigned char k[24][2] = {
        { 47, 51 }, { 47, 52 }, { 48, 51 }, { 48, 52 }, { 48, 53 }, { 49, 51 }, { 49, 52 }, { 49, 53 }, { 49, 54 },   /* W1A .. W3D (2..10) */
        { 1, 1 }, { 47, 51 }, { 47, 55 }, { 48, 51 }, { 48, 55 }, { 49, 51 }, { 49, 55 },                             /* KWS, K1A .. K3R (11..17) */
        { 1, 1 }, { 47, 51 }, { 47, 55 }, { 48, 51 }, { 48, 55 }, { 49, 51 }, { 49, 55 }, { 50, 1 } };               /* SWS, S1A .. S3R, BlackBox (18..25) */
    *a = *b = 1;
    if (level >= 2 && level <= 25) { *a = k[level - 2][0]; *b = k[level - 2][1]; }
}
int hud_results_draw(const HudResults *r, float dt)
{
    RS.counting = 0;
    if (!H.ok) return 0;
    RS.t += dt; if (RS.delay > 0) RS.delay -= dt;
    if (RS.iris_on) { RS.it += dt; float f = RS.it / 0.5f; if (f > 1) f = 1; hud_iris(RS.iv0 - (RS.iv0 - RS.iv1) * f); }   /* 0x477920, round (320, 240) */
    if (!RS.shown) return 0;
    float off;
    if (RS.t <= 0.5f) off = (0.5f - RS.t) * -300.0f / 0.5f;                       /* -300 -> 0; the lines wait */
    else { off = 0; res_lines(r, dt); }
    font_size(25.0f); row_reset(); row_str(13); res_text(16 + off, 16, 0xfe800000, 1);                                  /* RESULTS 0x455790 */
    font_size(18.0f); row_reset(); row_str(17);                                                                          /* HIGH SCORE 0x455850 */
    { float wl = font_measure(g_row); font_draw(624 - wl - off, 16, g_row, 0xfe808080);
      row_reset(); row_num(r->best); font_draw(624 - wl * 0.5f - off - font_measure(g_row) * 0.5f, 16 + font_cell(), g_row, 0xfe808080); }   /* the best score saved BEFORE this run */
    uint32_t na, nb; res_name(r->level, &na, &nb);                                                                       /* 0x455bc0 */
    row_reset(); row_str(na); row_space(); row_str(nb);
    { float S = 15.0f; font_size(S); while (S > 10.0f && font_measure(g_row) > 400.0f) font_size(S -= 1.0f);
      font_draw(320 - font_measure(g_row) * 0.5f, 415 - off, g_row, 0xfe808080);
      float y = 415 + font_cell() - off;
      font_size(20.0f); row_reset(); row_str(12); res_text(320 - font_measure(g_row) * 0.5f, y, 0xfe801400, 1); }     /* CLEARED!! */
    font_size(17.0f);
    return RS.counting;
}

/* ---------------------------------------------------------------- page 4, the high scores of one character (docs/MENU_LOAD.md 4.8)
 * vt[17] 0x45bff0: a = the slide (600 -> 0), b = the backdrop scale (0 -> 1), both over the panel's 0.5 s. First the
 * title logo as a dark, half transparent plate growing out of the centre (0x45c090(b)), then the three column heads and
 * the rows sliding in from the left (0x45c3d0(-a)), last "HIGH SCORES" with the character's face on either side
 * coming down from above (0x45c150(a)). The page itself is black: its iris target +0x30 is 0, so the panel base
 * 0x45b990 lays a full black rect instead of the ring. */
static void score_row(float off, const HudScoreRow *r, float row)                   /* 0x45c510 */
{
    const uint32_t wh = 0xfeffffff;
    float y = 105.0f + 37.0f * row;                                                 /* 0x4ab35c + 0x4ab3dc * row */
    quad(off, y, 256, 22, 0, 0, 0, 0, 0, 0x800000ff, 0x800000ff, 0x000000ff, 0x000000ff);   /* 0x45d1a0: the HUD's blue bar, 22 high */
    font_size(18.0f);
    const uint16_t *s40 = hud_string(40); uint16_t sp[2] = { s40 && s40[0] ? s40[1] : 0, 0 };   /* word [str40 + 2]: the space */
    float wsp = font_measure(sp), ty = y + (22.0f - font_cell()) * 0.5f;
    row_reset(); row_str(r->world); row_space(); row_str(r->part);                  /* "Space Part A" at x 20 */
    float x = 20.0f + off; font_draw(x, ty, g_row, wh); x += font_measure(g_row);  /* Font::Draw moves the pen */
    x += 3.0f * wsp; if (x < 220.0f + off) x = 220.0f + off;                        /* three spaces on, but at least x 220 */
    row_reset(); row_num(r->best); row_space(); row_str(46);                        /* "1234 points" */
    font_draw(x, ty, g_row, wh);
    if (!r->race) {                                                                 /* K1R K2R K3R S1R S2R S3R: no time and no enemies */
        int t = (int)r->time;                                                       /* _ftol */
        row_reset(); row_num(t / 60); row_str(10); if (t % 60 < 10) row_num(0); row_num(t % 60);
        font_draw(off + 580.0f - font_measure(g_row) * 0.5f, ty, g_row, wh);        /* x * font VW / 640 = x */
        row_reset(); row_num(r->en_got); row_str(9); row_num(r->en_tot);
        font_draw(off + 520.0f - font_measure(g_row) * 0.5f, ty, g_row, wh);
    }
    row_reset(); row_num(r->w_got); row_str(9); row_num(r->w_tot);
    font_draw(off + 440.0f - font_measure(g_row) * 0.5f, ty, g_row, wh);
}
void hud_scores(const HudScores *s)
{
    if (!H.ok) return;
    const float a = s->slide, b = s->grow; const uint32_t g = 0xfe808080;
    if (H.logo && b > 0) {                                                          /* 0x45c090: House image 1 (0, 0, 209, 247) into 315 x 372, colour 0x80202020, flag 8 */
        const uint32_t c = 0x80202020;
        quad(320.0f - 157.5f * b, 240.0f - 186.0f * b, 315.0f * b, 372.0f * b, H.logo, 0, 0, 209.0f / H.logo_w, 247.0f / H.logo_h, c, c, c, c);
    }
    /* 0x45c3d0(-a): 24 x 24 heads at y 80 over the columns 580 / 520 / 440: the clock and the enemy face from House
     * image 2 (additive, the sheet has alpha 0) and the W ball from Common image 64 (0, 89, 24, 24, alpha) */
    if (H.sheet2) {
        const float W = (float)H.sheet2_w, Hh = (float)H.sheet2_h;
        glBlendFunc(GL_ONE, GL_ONE);
        quad(568.0f - a, 80, 24, 24, H.sheet2, 52 / W, 0, 88 / W, 36 / Hh, g, g, g, g);
        quad(508.0f - a, 80, 24, 24, H.sheet2, 0, 0, 51 / W, 51 / Hh, g, g, g, g);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    if (H.img[3]) { const float W = (float)H.img_w[3], Hh = (float)H.img_h[3]; quad(428.0f - a, 80, 24, 24, H.img[3], 0, 89 / Hh, 24 / W, 113 / Hh, g, g, g, g); }
    for (int i = 0; i < s->nrows && i < 9; i++) score_row(-a, &s->row[i], (float)i);   /* 0x45ca40 / 0x45cd80 / 0x45cf90 */
    {   /* 0x45c150(a): 45 "HIGH SCORES" size 28 orange-red at (320 - w/2, 30 - a); 0x45c230: the face 10 left of it and mirrored 10 right of it, y 16 - a */
        font_size(28.0f); row_reset(); row_str(45);
        float w = font_measure(g_row);
        font_draw(320.0f - w * 0.5f, 30.0f - a, g_row, 0xfeff1400);
        int f = s->face >= 0 && s->face < 3 ? s->face : 0, i = k_spr[f].img;
        sprite(f, 320.0f - w * 0.5f - k_spr[f].w - 10.0f, 16.0f - a);
        if (H.img[i]) {
            const float W = (float)H.img_w[i], Hh = (float)H.img_h[i];
            quad(320.0f + w * 0.5f + 10.0f, 16.0f - a, k_spr[f].w, k_spr[f].h, H.img[i], (k_spr[f].x + k_spr[f].w) / W, k_spr[f].y / Hh, k_spr[f].x / W, (k_spr[f].y + k_spr[f].h) / Hh, g, g, g, g);
        }
    }
    font_size(17.0f);
}

/* ---------------------------------------------------------------- world sprites: the lists +0x1c8 / +0x1cc
 * Every world sprite, line and quad of the original goes through 0x481560 (the sprite 0x470f10 at 0x4719f3, the line 0x471a10
 * at 0x471eb2, the fire ring 0x47a4c0), and every call opens a batch of its own (0x481a20 / 0x481d8c): submit flag 8 = mode 2
 * on list renderer+0x1c8 (alpha blended), flag 4 = mode 3 on +0x1cc (additive ONE/ONE), sort depth batch+0x10 = the view z of
 * the first vertex that is left after the side-plane clip (0x481d7f). The lists are flushed by 0x428d00 (0x4299b6, after the
 * water) together with the fade list, in depth buckets (docs/MODEL_RENDER.md 10). So nothing is drawn here: each call records
 * one quad, and the renderer draws the records from its bucket loop (rnd_sorted -> hud_wq_*). Bank 0 images are created with
 * texture flags +0x44 = 0 (0x47f926), so the alpha test of 0x428d00 (colour-key bit, 0x429102) is off for all of them. */
#ifndef GL_COMBINE_ARB
#define GL_COMBINE_ARB 0x8570
#define GL_COMBINE_RGB_ARB 0x8571
#define GL_COMBINE_ALPHA_ARB 0x8572
#define GL_RGB_SCALE_ARB 0x8573
#endif
typedef struct { float p[4][3], t[4][2], c[4][4]; GLuint tex; uint8_t add, x2, poff, early; } WQuad;
static WQuad *g_wq; static int g_nwq, g_wqcap, g_wq_early;
static int gl_combine(void);
static WQuad *wq_new(GLuint tex, int add)
{
    if (g_nwq == g_wqcap) { int n = g_wqcap ? g_wqcap * 2 : 256; WQuad *q = (WQuad *)realloc(g_wq, (size_t)n * sizeof *q); if (!q) return NULL; g_wq = q; g_wqcap = n; }
    WQuad *q = &g_wq[g_nwq++]; memset(q, 0, sizeof *q);
    q->tex = tex; q->add = (uint8_t)(add != 0); q->early = (uint8_t)g_wq_early;
    return q;
}
static void wq_vtx(WQuad *q, int i, float x, float y, float z, float u, float v) { q->p[i][0] = x; q->p[i][1] = y; q->p[i][2] = z; q->t[i][0] = u; q->t[i][1] = v; }
static void wq_rgba(WQuad *q, int i, float r, float g, float b, float a) { q->c[i][0] = r; q->c[i][1] = g; q->c[i][2] = b; q->c[i][3] = a; }
/* the colour of all four corners. Additive (0x481e5e): byte a*c*128 under MODULATE2X = texture x c x a. Alpha blended (flag 8,
 * 0x481ab2): colour byte c*255, alpha a*255 under MODULATE2X = texture x 2c (GL_COMBINE with RGB_SCALE 2, or 2c clamped) */
static void wq_colour(WQuad *q, const float *c, float a)
{
    for (int i = 0; i < 4; i++) {
        if (q->add) wq_rgba(q, i, c[0] * a, c[1] * a, c[2] * a, 1.0f);
        else if (gl_combine()) { q->x2 = 1; wq_rgba(q, i, c[0] > 1 ? 1 : c[0], c[1] > 1 ? 1 : c[1], c[2] > 1 ? 1 : c[2], a); }
        else wq_rgba(q, i, c[0] * 2 > 1 ? 1 : c[0] * 2, c[1] * 2 > 1 ? 1 : c[1] * 2, c[2] * 2 > 1 ? 1 : c[2] * 2, a);
    }
}
static void wq_plain(WQuad *q) { for (int i = 0; i < 4; i++) wq_rgba(q, i, 1, 1, 1, 1); }   /* the default colour 0x4b7a84 (0.5, 1) on the blended path = the plain texture */
void hud_world_sprites_begin(const float *right, const float *up)
{
    memcpy(H.sr, right, sizeof H.sr); memcpy(H.su, up, sizeof H.su);
    g_nwq = 0; g_wq_early = 1;
}
/* the bonus halos are submitted by the instance Updates (0x42b400 -> 0x479530), before the world draw 0x42b380 creates the
 * model batches; everything after this call (Perso, effects 0x46d040) comes after them. Only bucket 0 merges the lists in
 * creation order (0x429240), so this is where the difference can show */
void hud_world_sprites_late(void) { g_wq_early = 0; }
int  hud_wq_count(void) { return g_nwq; }
void hud_wq_quad(int i, float v[4][3], int *blended, int *early)
{
    const WQuad *q = &g_wq[i]; memcpy(v, q->p, sizeof q->p);
    *blended = !q->add; *early = q->early;
}
void hud_wq_draw(int i)
{
    const WQuad *q = &g_wq[i];
    if (q->tex) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, q->tex); } else glDisable(GL_TEXTURE_2D);
    if (q->add) glBlendFunc(GL_ONE, GL_ONE); else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (q->x2) {
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_ARB); glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_MODULATE);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA_ARB, GL_MODULATE); glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_ARB, 2.0f);
    } else { glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE); if (gl_combine()) glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_ARB, 1.0f); }
    if (q->poff) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-1.0f, -4.0f); } else glDisable(GL_POLYGON_OFFSET_FILL);
    glBegin(GL_QUADS);
    for (int k = 0; k < 4; k++) { glColor4fv(q->c[k]); glTexCoord2fv(q->t[k]); glVertex3fv(q->p[k]); }
    glEnd();
}
void hud_wq_done(void)
{
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE); if (gl_combine()) glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_ARB, 1.0f);
    glDisable(GL_POLYGON_OFFSET_FILL); glColor4f(1, 1, 1, 1); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glEnable(GL_TEXTURE_2D);
}
/* pickup halos (0x479530 -> 0x470f10 with flags 0x1b, 0x479631: camera facing, alpha blended, default colour) */
void hud_world_sprite(int n, const float *pos, float size)
{
    if (!H.ok || n < 0 || n >= 5 || !H.bonus[n]) return;
    float h = size * 0.70710678f, c[3] = { pos[0], pos[1] + 50.0f, pos[2] };        /* +50: 0x4a9030; h = size/sqrt(2), see hud_world_fx */
    WQuad *q = wq_new(H.bonus[n], 0); if (!q) return;
    wq_plain(q);
    wq_vtx(q, 0, c[0] - H.sr[0] * h + H.su[0] * h, c[1] - H.sr[1] * h + H.su[1] * h, c[2] - H.sr[2] * h + H.su[2] * h, 0, 0);
    wq_vtx(q, 1, c[0] - H.sr[0] * h - H.su[0] * h, c[1] - H.sr[1] * h - H.su[1] * h, c[2] - H.sr[2] * h - H.su[2] * h, 0, 1);
    wq_vtx(q, 2, c[0] + H.sr[0] * h - H.su[0] * h, c[1] + H.sr[1] * h - H.su[1] * h, c[2] + H.sr[2] * h - H.su[2] * h, 1, 1);
    wq_vtx(q, 3, c[0] + H.sr[0] * h + H.su[0] * h, c[1] + H.sr[1] * h + H.su[1] * h, c[2] + H.sr[2] * h + H.su[2] * h, 1, 0);
}
/* one wing of a butterfly (0x47d440 / 0x470f10): a square of 2*half units in the plane of u and v, centred on c. It is
 * NOT camera-facing - the two wings share a hinge along v (the flight direction) and swing about it. Corner angles
 * 45/135/225/315 degrees (0x470f94, table2[18] = 64) with the UV set of case 4 (0x470e96). Between
 * hud_world_sprites_begin/end, like the pickups: the original submits it with the same mode 0x28. */
void hud_world_wing(int n, const float *c, const float *u, const float *v, float half)
{
    if (!H.ok || n < 0 || n >= 4 || !H.env[n]) return;
    WQuad *q = wq_new(H.env[n], 0); if (!q) return;
    wq_plain(q);
    wq_vtx(q, 0, c[0] + (v[0] + u[0]) * half, c[1] + (v[1] + u[1]) * half, c[2] + (v[2] + u[2]) * half, 1, 0);
    wq_vtx(q, 1, c[0] + (-v[0] + u[0]) * half, c[1] + (-v[1] + u[1]) * half, c[2] + (-v[2] + u[2]) * half, 1, 1);
    wq_vtx(q, 2, c[0] - (v[0] + u[0]) * half, c[1] - (v[1] + u[1]) * half, c[2] - (v[2] + u[2]) * half, 0, 1);
    wq_vtx(q, 3, c[0] + (v[0] - u[0]) * half, c[1] + (v[1] - u[1]) * half, c[2] + (v[2] - u[2]) * half, 0, 0);
}
/* the comic speech bubble 0x478980 (docs/PERSO_DEATH.md 4.1): bank 0 image 44 = the balloon, 45..52 = what is in it.
 * Sprite flags 0x49: camera facing (bit 0), alpha blended instead of additive (bit 3), mirror flags applied (bit 6);
 * mirror value 2 (0x470d80 case 2) swaps u, so the tail points the other way. Standard colour (bit 1 off) = white.
 * `size` is the half diagonal, as for every sprite (0x470fee). Between hud_world_sprites_begin/end. */
void hud_world_bubble(int image, const float *pos, float size, int mirror)
{
    GLuint t = image == 46 ? H.bonus[3] : image >= 44 && image <= 52 ? H.bub[image - 44] : 0;
    if (!H.ok || !t || size <= 0) return;
    float h = size * 0.70710678f, u0 = mirror ? 1.0f : 0.0f, u1 = 1.0f - u0;
    WQuad *q = wq_new(t, 0); if (!q) return;
    wq_plain(q);
    wq_vtx(q, 0, pos[0] - H.sr[0] * h + H.su[0] * h, pos[1] - H.sr[1] * h + H.su[1] * h, pos[2] - H.sr[2] * h + H.su[2] * h, u0, 0);
    wq_vtx(q, 1, pos[0] - H.sr[0] * h - H.su[0] * h, pos[1] - H.sr[1] * h - H.su[1] * h, pos[2] - H.sr[2] * h - H.su[2] * h, u0, 1);
    wq_vtx(q, 2, pos[0] + H.sr[0] * h - H.su[0] * h, pos[1] + H.sr[1] * h - H.su[1] * h, pos[2] + H.sr[2] * h - H.su[2] * h, u1, 1);
    wq_vtx(q, 3, pos[0] + H.sr[0] * h + H.su[0] * h, pos[1] + H.sr[1] * h + H.su[1] * h, pos[2] + H.sr[2] * h + H.su[2] * h, u1, 0);
}
void hud_world_sprites_end(void) { }                                               /* the records are drawn by rnd_sorted */


int hud_sky_images(uint32_t out[5])
{
    static const int order[5] = { 3, 0, 1, 2, 4 };
    if (H.nlevel_img < 5) return 0;
    for (int f = 0; f < 5; f++) out[f] = H.sky[order[f]];
    return 1;
}

/* The UV sets of 0x470d80(S, mode, which) (jump table 0x470ef4, modes 0..6) for the sprite corners k0..k3 at 45, 135, 225,
 * 315 deg: which = 0 writes the sprite's vertex set S+0x00..0xc0 (k1 = +0x40 gets eax, k0 = +0x00 edx, k3 = +0xc0 esi, k2 =
 * +0x80 edi); which = 1 writes the LINE's vertex set S+0x100..0x1c0 of 0x471a10 (v0 = +0x100 eax, v3 = +0x1c0 edx, v2 =
 * +0x180 esi, v1 = +0x140 edi, 0x470d91..0x470da3), so line vertex v gets the entry of sprite corner (v + 1) & 3. Mode 5
 * writes nothing (0x470eeb); every other mode also stores itself in S+0x200 (the sprite's "last set", 0x470e00..0x470ee1). */
static const float k_uvset[7][4][2] = {
    { {1,0}, {0,0}, {0,1}, {1,1} }, { {1,1}, {0,1}, {0,0}, {1,0} }, { {0,0}, {1,0}, {1,1}, {0,1} }, { {0,1}, {1,1}, {1,0}, {0,0} },
    { {1,1}, {1,0}, {0,0}, {0,1} }, { {1,0}, {0,0}, {0,1}, {1,1} }, { {0,0}, {0,1}, {1,1}, {1,0} } };
/* The line's UV set PERSISTS: S is one shared object, the ctor 0x470d60 sets both vertex sets to mode 0, and the only
 * call that changes the line's set is the lightning bolt's 0x470d80(kind, 1) (0x46d932) before each of its segments. The
 * reset after a line (0x471eb7: 0x470d80(0, 1) when S+0x204 != 0) never fires, because nothing writes S+0x204. So every
 * later textured line - the storm's rod arcs, the rain streaks, the laser beams, the hit-star speed lines - is drawn
 * with the uv mirror of the LAST bolt segment until the next bolt. v0/v1 = the start's two sides, v2/v3 the end's (0x471b80..
 * 0x471cd5): u runs along the line, v across it. */
static int g_line_uv;
static void world_line_uv(const float *a, const float *b, const float *eye, float hw, const float *rgb, float alpha_a, float alpha_b, GLuint tex, int mode);
static void world_line(const float *a, const float *b, const float *eye, float hw, const float *rgb, float alpha_a, float alpha_b, GLuint tex)
{
    world_line_uv(a, b, eye, hw, rgb, alpha_a, alpha_b, tex, -1);
}
/* mode = a uv mode of 0x470d80 to set first (the bolt), -1 = keep the current set (every other line) */
static void world_line_uv(const float *a, const float *b, const float *eye, float hw, const float *rgb, float alpha_a, float alpha_b, GLuint tex, int mode)
{
    if (mode >= 0 && mode <= 6 && mode != 5) g_line_uv = mode;
    if (!H.ok) return;
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, m[3] = { (a[0] + b[0]) * 0.5f - eye[0], (a[1] + b[1]) * 0.5f - eye[1], (a[2] + b[2]) * 0.5f - eye[2] };
    float s[3] = { d[1] * m[2] - d[2] * m[1], d[2] * m[0] - d[0] * m[2], d[0] * m[1] - d[1] * m[0] };      /* perpendicular to the segment and to the view ray */
    float l = (float)sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]); if (l < 1e-6f) return;
    for (int i = 0; i < 3; i++) s[i] *= hw / l;
    WQuad *q = wq_new(tex, 1); if (!q) return;                                    /* submit flags 0x24 (0x471e74): additive ONE/ONE, list +0x1cc */
    const float (*t)[2] = k_uvset[g_line_uv];                                      /* mode 0: v0 (0,0) v1 (0,1) v2 (1,1) v3 (1,0) */
    wq_vtx(q, 0, a[0] - s[0], a[1] - s[1], a[2] - s[2], t[1][0], t[1][1]); wq_rgba(q, 0, rgb[0] * alpha_a, rgb[1] * alpha_a, rgb[2] * alpha_a, 1);
    wq_vtx(q, 1, a[0] + s[0], a[1] + s[1], a[2] + s[2], t[2][0], t[2][1]); wq_rgba(q, 1, rgb[0] * alpha_a, rgb[1] * alpha_a, rgb[2] * alpha_a, 1);
    wq_vtx(q, 2, b[0] + s[0], b[1] + s[1], b[2] + s[2], t[3][0], t[3][1]); wq_rgba(q, 2, rgb[0] * alpha_b, rgb[1] * alpha_b, rgb[2] * alpha_b, 1);
    wq_vtx(q, 3, b[0] - s[0], b[1] - s[1], b[2] - s[2], t[0][0], t[0][1]); wq_rgba(q, 3, rgb[0] * alpha_b, rgb[1] * alpha_b, rgb[2] * alpha_b, 1);
}
void hud_world_beam(const float *a, const float *b, const float *eye, float hw, const float *rgb, float alpha_a, float alpha_b) { world_line(a, b, eye, hw, rgb, alpha_a, alpha_b, H.beam); }
void hud_world_line(const float *a, const float *b, const float *eye, float hw, const float *rgb, float alpha_a, float alpha_b) { world_line(a, b, eye, hw, rgb, alpha_a, alpha_b, 0); }
void hud_world_quad(int image, const float v[4][3], const float uv[4][2], const float rgb[4][3])
{
    int k = fx_slot(image); if (!H.ok || k < 0 || !H.fx[k]) return;
    WQuad *q = wq_new(H.fx[k], 1); if (!q) return;                                 /* 0x47a73c / 0x47a75d: 0x481560 with flag 4 */
    for (int i = 0; i < 4; i++) { wq_vtx(q, i, v[i][0], v[i][1], v[i][2], uv[i][0], uv[i][1]); wq_rgba(q, i, rgb[i][0], rgb[i][1], rgb[i][2], 1); }
}
void hud_world_streak(int image, const float *a, const float *b, const float *eye, float hw, const float *rgb, float alpha_a, float alpha_b) { int k = fx_slot(image); if (k >= 0 && H.fx[k]) world_line(a, b, eye, hw, rgb, alpha_a, alpha_b, H.fx[k]); }
void hud_world_streak_flip(int image, const float *a, const float *b, const float *eye, float hw, const float *rgb, float alpha_a, float alpha_b, int flip) { int k = fx_slot(image); if (k >= 0 && H.fx[k]) world_line_uv(a, b, eye, hw, rgb, alpha_a, alpha_b, H.fx[k], flip); }

#ifndef GL_COMBINE_ARB
#define GL_COMBINE_ARB 0x8570
#define GL_COMBINE_RGB_ARB 0x8571
#define GL_COMBINE_ALPHA_ARB 0x8572
#define GL_RGB_SCALE_ARB 0x8573
#endif
static int gl_combine(void)
{
    static int c = -1;
    if (c < 0) { const char *ext = (const char *)glGetString(GL_EXTENSIONS), *ver = (const char *)glGetString(GL_VERSION);
                 c = (ext && strstr(ext, "GL_ARB_texture_env_combine")) || (ver && (ver[0] > '1' || (ver[0] == '1' && ver[2] >= '3'))); }
    return c;
}
/* sprite flag 8 (0x4719bc: submit flag 8, 0x481a05): the colour byte is c*255 and the alpha byte a*255, drawn under COLOROP
 * MODULATE2X (0x429758, LIGHTING.md 1.5 step 6), so the texture is scaled by 2c (clamped): c = 0.5 is neutral. Without a
 * MODULATE2X device 0x471224 doubles c and clamps it to 1 instead (same result up to the clamp). Recorded by wq_colour,
 * drawn by hud_wq_draw. */
/* S+0x208, the position field of the one shared sprite object [0x5e823c]+0xb00: every effect writes it before it calls
 * 0x470f10, drawn or not, and it keeps the last value. The skeleton flash registers its light there (0x477db3). */
static float g_spr_pos[3];
void hud_last_sprite_pos(float out[3]) { out[0] = g_spr_pos[0]; out[1] = g_spr_pos[1]; out[2] = g_spr_pos[2]; }
void hud_world_fx(int image, const float *pos, float size, float turns, const float *rgb, float alpha)
{
    g_spr_pos[0] = pos[0]; g_spr_pos[1] = pos[1]; g_spr_pos[2] = pos[2];
    int n = fx_slot(image), blend = image == 10 || image == 11 || image == 24; if (!H.ok || n < 0 || !H.fx[n] || alpha <= 0 || size <= 0) return;   /* the stars and the bomb smoke (sprite flag 8) are alpha blended, the rest additive */
    /* 0x470fee..0x4710b3: every corner is (size*cos t, size*sin t) with t = rot +- 45 deg, so `size` is the half
     * DIAGONAL, not the half width: the half width is size/sqrt(2) and the side is 1.4142*size */
    float h = size * 0.70710678f, c = (float)cos(turns * 6.2831853f) * h, s = (float)sin(turns * 6.2831853f) * h, r[3], u[3];
    for (int i = 0; i < 3; i++) { r[i] = H.sr[i] * c + H.su[i] * s; u[i] = H.su[i] * c - H.sr[i] * s; }
    WQuad *q = wq_new(H.fx[n], !blend); if (!q) return;                          /* flag 8: texture x 2c, alpha a; additive: texture x c x a */
    wq_colour(q, rgb, alpha);
    wq_vtx(q, 0, pos[0] - r[0] + u[0], pos[1] - r[1] + u[1], pos[2] - r[2] + u[2], 0, 0);
    wq_vtx(q, 1, pos[0] - r[0] - u[0], pos[1] - r[1] - u[1], pos[2] - r[2] - u[2], 0, 1);
    wq_vtx(q, 2, pos[0] + r[0] - u[0], pos[1] + r[1] - u[1], pos[2] + r[2] - u[2], 1, 1);
    wq_vtx(q, 3, pos[0] + r[0] + u[0], pos[1] + r[1] + u[1], pos[2] + r[2] + u[2], 1, 0);
}
/* the same quad, but lying in the plane with normal `n` (0x4717d7 builds it on S+0x230..0x238 when sprite flag bit 0
 * is off). The flash of an explosion is nine of these, each on its own normal (docs/PROJECTILES.md 5.3), so which way
 * round the two in-plane axes point does not matter: any pair perpendicular to `n` gives the same square. */
void hud_world_fx_plane(int image, const float *pos, const float *n, float size, const float *rgb, float alpha)
{
    g_spr_pos[0] = pos[0]; g_spr_pos[1] = pos[1]; g_spr_pos[2] = pos[2];
    int k = fx_slot(image); if (!H.ok || k < 0 || !H.fx[k] || alpha <= 0 || size <= 0) return;
    float N[3] = { n[0], n[1], n[2] }, l = (float)sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
    if (l < 1e-6f) return;
    for (int i = 0; i < 3; i++) N[i] /= l;
    float ax[3] = { 1, 0, 0 }; if (fabs(N[0]) > 0.9f) { ax[0] = 0; ax[2] = 1; }
    float u[3] = { ax[1] * N[2] - ax[2] * N[1], ax[2] * N[0] - ax[0] * N[2], ax[0] * N[1] - ax[1] * N[0] };
    l = (float)sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]); if (l < 1e-6f) return;
    float h = size * 0.70710678f;
    for (int i = 0; i < 3; i++) u[i] *= h / l;
    float v[3] = { (N[1] * u[2] - N[2] * u[1]), (N[2] * u[0] - N[0] * u[2]), (N[0] * u[1] - N[1] * u[0]) };
    WQuad *q = wq_new(H.fx[k], image != 24); if (!q) return;                    /* image 24 = the smoke ring of a bomb (0x4767d3, flag 0xa: alpha blended, texture x 2c) */
    wq_colour(q, rgb, alpha);
    wq_vtx(q, 0, pos[0] - u[0] + v[0], pos[1] - u[1] + v[1], pos[2] - u[2] + v[2], 0, 0);
    wq_vtx(q, 1, pos[0] - u[0] - v[0], pos[1] - u[1] - v[1], pos[2] - u[2] - v[2], 0, 1);
    wq_vtx(q, 2, pos[0] + u[0] - v[0], pos[1] + u[1] - v[1], pos[2] + u[2] - v[2], 1, 1);
    wq_vtx(q, 3, pos[0] + u[0] + v[0], pos[1] + u[1] + v[1], pos[2] + u[2] + v[2], 1, 0);
}
/* ---- the sprite primitive 0x470f10(S = [0x5e823c]+0xb00, flags) itself (docs/PARTICLES.md 1) -----------------------
 * The effects decompiled in docs/PARTICLES.md fill S and call this with their own flags, so the port takes those as
 * they are instead of choosing a look per image the way hud_world_fx does:
 *   bit 0 (1)    camera facing (0x4714ea); otherwise the quad lies in a plane:
 *   bit 5 (0x20) ... spanned by the caller's R = basis[0..2] and F = basis[3..5] (S+0x23c / S+0x248, 0x4715d5);
 *                without it, the plane with normal basis[0..2] (S+0x230, 0x4717d7) on the axes of 0x471ee0
 *   bit 1 (2)    own colour and alpha S+0x214..0x220 (0x4710bd); otherwise 0x4b7a84 = (0.5, 0.5, 0.5, 1)
 *   bit 2 (4)    rotation S+0x224 in 1/512 turn (0x470f39); otherwise the corners sit at 45/135/225/315 degrees
 *   bit 3 (8)    alpha blended, texture x 2c (MODULATE2X), alpha a; otherwise additive ONE/ONE, texture x c x a (0x4719b7)
 *   bit 6 (0x40) UV set `mirror` of 0x470d80: 0 plain, 1 v flipped, 2 u flipped, 3 both, 4 and 6 turned a quarter
 * `size` is the half DIAGONAL (0x470fee: corner k at size * (cos t, sin t), t = rot + 64 + 128k for the square mode 0x12;
 * mode 0x1a is a 1:2 upright quad, t = rot +- 90); a negative size turns the quad half a turn, as the original's does. */
static void plane_axes(const float *n, float *u, float *v)          /* 0x471ee0: column 0 = u, column 1 = v, column 2 = n */
{
    if (fabs(n[0]) < 0.001f && 1.0f - fabs(n[1]) < 0.001f && fabs(n[2]) < 0.001f) {   /* (almost) straight up or down */
        float l = (float)sqrt(n[1] * n[1] + n[2] * n[2]); if (l <= 0) l = 1;
        v[0] = 0; v[1] = -n[2] / l; v[2] = n[1] / l;
        u[0] = v[1] * n[2] - v[2] * n[1]; u[1] = v[2] * n[0]; u[2] = -v[1] * n[0];      /* v x n */
    } else {
        float l = (float)sqrt(n[0] * n[0] + n[2] * n[2]); if (l <= 0) l = 1;
        u[0] = n[2] / l; u[1] = 0; u[2] = -n[0] / l;                                    /* level, across the normal */
        v[0] = n[1] * u[2] - n[2] * u[1]; v[1] = n[2] * u[0] - n[0] * u[2]; v[2] = n[0] * u[1] - n[1] * u[0];   /* n x u */
    }
}
void hud_world_spr_mode(int mode, int image, const float *pos, float size, int rot, const float *rgb, float alpha, int flags, const float *basis, int mirror)
{
    g_spr_pos[0] = pos[0]; g_spr_pos[1] = pos[1]; g_spr_pos[2] = pos[2];
    static const float def[4] = { 0.5f, 0.5f, 0.5f, 1.0f };                             /* 0x4b7a84 */
    const float (*uv)[4][2] = k_uvset;                                                 /* 0x470d80 cases 0..6 for the corners at 45, 135, 225, 315 deg */
    int k = fx_slot(image); if (!H.ok || k < 0 || !H.fx[k] || size == 0) return;
    const float *c = (flags & 2) ? rgb : def; float a = (flags & 2) ? alpha : def[3];
    if (a <= 0) return;
    float X[3], Y[3];
    if (flags & 1) { memcpy(X, H.sr, sizeof X); memcpy(Y, H.su, sizeof Y); }
    else if (flags & 0x20) { if (!basis) return; memcpy(X, basis, sizeof X); memcpy(Y, basis + 3, sizeof Y); }
    else { if (!basis) return; plane_axes(basis, X, Y); }
    int m = (flags & 0x40) && mirror >= 0 && mirror <= 6 ? mirror : 0, r = (flags & 4) ? rot : 0;
    int base = mode == 0x1a ? 90 : mode == 0x13 ? 37 : 64;                             /* [0x5e823c]+0x800[mode] (0x4024bb): atan(2^(mode/8 - mode%8)) in 1/512 turn; 0x13 = 2:1 wide (the race board's flames) */
    WQuad *q = wq_new(H.fx[k], !(flags & 8)); if (!q) return;                     /* 0x4719b2: flag 8 -> submit 8 (list +0x1c8), else 4 (+0x1cc) */
    q->poff = !(flags & 1);                                                            /* a print on the floor or a hole in a wall is coplanar with it (port: polygon offset) */
    wq_colour(q, c, a);
    for (int i = 0; i < 4; i++) {                                                      /* corners at r + base, r - base + 256, r + base + 256, r - base + 512 */
        int ang = (i & 1) ? r - base + 256 * (i == 1 ? 1 : 2) : r + base + 256 * (i >> 1);
        float t = 6.2831853f * (float)(ang & 511) / 512.0f, cx = size * (float)cos(t), cy = size * (float)sin(t);
        wq_vtx(q, i, pos[0] + X[0] * cx + Y[0] * cy, pos[1] + X[1] * cx + Y[1] * cy, pos[2] + X[2] * cx + Y[2] * cy, uv[m][i][0], uv[m][i][1]);
    }
}
void hud_world_spr(int image, const float *pos, float size, int rot, const float *rgb, float alpha, int flags, const float *basis, int mirror)
{
    hud_world_spr_mode(0x12, image, pos, size, rot, rgb, alpha, flags, basis, mirror);
}
void hud_world_ribbon(const float *a, const float *b, const float *eye, float hw, const float *rgb_a, const float *rgb_b)
{
    if (!H.ok) return;
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, m[3] = { (a[0] + b[0]) * 0.5f - eye[0], (a[1] + b[1]) * 0.5f - eye[1], (a[2] + b[2]) * 0.5f - eye[2] };
    float s[3] = { d[1] * m[2] - d[2] * m[1], d[2] * m[0] - d[0] * m[2], d[0] * m[1] - d[1] * m[0] };
    float l = (float)sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]); if (l < 1e-6f) return;
    for (int i = 0; i < 3; i++) s[i] *= hw / l;
    WQuad *q = wq_new(H.fx[0], 1); if (!q) return;                                /* the line primitive: additive, list +0x1cc */
    wq_vtx(q, 0, a[0] - s[0], a[1] - s[1], a[2] - s[2], 0, 0); wq_rgba(q, 0, rgb_a[0], rgb_a[1], rgb_a[2], 1);
    wq_vtx(q, 1, a[0] + s[0], a[1] + s[1], a[2] + s[2], 0, 1); wq_rgba(q, 1, rgb_a[0], rgb_a[1], rgb_a[2], 1);
    wq_vtx(q, 2, b[0] + s[0], b[1] + s[1], b[2] + s[2], 1, 1); wq_rgba(q, 2, rgb_b[0], rgb_b[1], rgb_b[2], 1);
    wq_vtx(q, 3, b[0] - s[0], b[1] - s[1], b[2] - s[2], 1, 0); wq_rgba(q, 3, rgb_b[0], rgb_b[1], rgb_b[2], 1);
}
