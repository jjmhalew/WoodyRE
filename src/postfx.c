/* postfx.c - edge smoothing of the 3D picture (PORT EXTRA, docs/DISPLAY.md 5). The original draws straight into its
 * DirectDraw back buffer without any antialiasing; both effects here are options of the port, off by default.
 *
 * With an effect on, postfx_begin binds an own target of the window's size, and the renderer draws the 3D picture into it
 * exactly as into the window (colour RGBA8, depth 24 + stencil 8 for the cast shadows):
 *   - MSAA (2x / 4x / 8x): the target is multisampled (renderbuffers); postfx_end resolves it into a single sampled texture
 *     (glBlitFramebuffer). Polygon edges get smooth, the colour-key cut-outs (foliage, fences) do not.
 *   - SMAA (low / medium / high / ultra = the presets of SMAA_PRESET_*): Jimenez et al., "SMAA: Enhanced Subpixel
 *     Morphological Antialiasing" (2012), the 1x mode of src/smaa/smaa.h (generated from iryoku/smaa by
 *     tools/smaa_embed.py): colour edge detection into an edges texture, blending weights from the edge shapes with the
 *     precomputed AreaTex / SearchTex lookups, then neighbourhood blending of the resolved picture into the window. It
 *     smooths the cut-outs too, and adds to MSAA.
 * Without SMAA the resolved picture is copied into the window (blit). Everything after postfx_end (the HUD, menus, text,
 * fades, the films) is drawn into the window directly and stays sharp. SMAA runs on the GL image as stored, bottom row
 * first: it then smooths the mirror image, which is just as valid - the lookups are indexed by edge shapes, not by screen
 * position, and the textures are uploaded in their own row order.
 * Desktop OpenGL (framebuffer objects, multisample renderbuffers, GLSL 1.30) or OpenGL ES 3.0: the Android and Switch builds
 * draw through the shim of src/gles (WOODY_GLES), which runs the same passes with the ES shading language (3.00) and also
 * puts the picture into the own target when only the ambient occlusion is on (gtao.c cannot read the window's depth
 * there). On an ES 2.0 context everything reports "not supported". */
#include "postfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#endif
#include "plat.h"
#include <GL/gl.h>
#include "stb/stb_image.h"
#include "smaa/smaa.h"

#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_SHADING_LANGUAGE_VERSION
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_MAX_SAMPLES
#define GL_MAX_SAMPLES 0x8D57
#endif
#ifndef GL_RG8
#define GL_RG 0x8227
#define GL_R8 0x8229
#define GL_RG8 0x822B
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef APIENTRY
#define APIENTRY GL_APIENTRY
#endif
#ifdef WOODY_GLES
#define OWN_DEPTH 1                           /* the depth of the window cannot be read: gtao.c needs the own target */
#else
#define OWN_DEPTH 0
#endif

/* own names (p_*): Mesa's gl.h already declares some of these as functions */
typedef char GLch;
static void   (APIENTRY *p_ActiveTexture)(GLenum);
static GLuint (APIENTRY *p_CreateShader)(GLenum);
static void   (APIENTRY *p_ShaderSource)(GLuint, GLsizei, const GLch *const *, const GLint *);
static void   (APIENTRY *p_CompileShader)(GLuint);
static void   (APIENTRY *p_GetShaderiv)(GLuint, GLenum, GLint *);
static void   (APIENTRY *p_GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLch *);
static void   (APIENTRY *p_DeleteShader)(GLuint);
static GLuint (APIENTRY *p_CreateProgram)(void);
static void   (APIENTRY *p_AttachShader)(GLuint, GLuint);
static void   (APIENTRY *p_BindAttribLocation)(GLuint, GLuint, const GLch *);
static void   (APIENTRY *p_LinkProgram)(GLuint);
static void   (APIENTRY *p_GetProgramiv)(GLuint, GLenum, GLint *);
static void   (APIENTRY *p_GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLch *);
static void   (APIENTRY *p_DeleteProgram)(GLuint);
static void   (APIENTRY *p_UseProgram)(GLuint);
static GLint  (APIENTRY *p_GetUniformLocation)(GLuint, const GLch *);
static void   (APIENTRY *p_Uniform1i)(GLint, GLint);
static void   (APIENTRY *p_Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
static void   (APIENTRY *p_GenFramebuffers)(GLsizei, GLuint *);
static void   (APIENTRY *p_DeleteFramebuffers)(GLsizei, const GLuint *);
static void   (APIENTRY *p_BindFramebuffer)(GLenum, GLuint);
static void   (APIENTRY *p_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static void   (APIENTRY *p_FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
static GLenum (APIENTRY *p_CheckFramebufferStatus)(GLenum);
static void   (APIENTRY *p_GenRenderbuffers)(GLsizei, GLuint *);
static void   (APIENTRY *p_DeleteRenderbuffers)(GLsizei, const GLuint *);
static void   (APIENTRY *p_BindRenderbuffer)(GLenum, GLuint);
static void   (APIENTRY *p_RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
static void   (APIENTRY *p_RenderbufferStorageMultisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static void   (APIENTRY *p_BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

static int g_msaa, g_smaa, g_depth;           /* the settings */
static int g_state;                           /* 0 = not tried yet, 1 = framebuffer objects there, -1 = this GL cannot */
static int g_msmax;                           /* GL_MAX_SAMPLES (0 = no multisample renderbuffers / blit) */
static int g_glsl130 = -1;                    /* GLSL 1.30 there (-1 = not asked yet) */
static int g_smaa_bad;                        /* the SMAA shaders failed once: SMAA stays off */
static int g_active, g_samples;               /* this frame draws into the target; its samples */
/* the targets, made for g_w x g_h with g_ms samples (g_ms < 0 = none yet) */
static int g_w, g_h, g_ms = -1, g_have_smaa;
static GLuint fb_ms, rb_ms_col, rb_ms_ds;     /* MSAA: the multisampled scene target */
static GLuint fb_col, tex_col, rb_ds;         /* the single sampled picture (the scene target itself without MSAA: + rb_ds) */
static GLuint fb_edge, tex_edge, fb_blend, tex_blend, tex_area, tex_search;
static int g_preset;                          /* the SMAA preset the programs were built for (0 = none) */
static GLuint pr_edge, pr_weight, pr_blend;
static GLint u_edge_m, u_edge_col, u_weight_m, u_weight_edge, u_weight_area, u_weight_search, u_blend_m, u_blend_col, u_blend_w;

static int init(void)
{
    #define GP(v, name) do { void (*f_)(void) = plat_gl_proc(name); memcpy(&v, &f_, sizeof f_); } while (0)
    GP(p_ActiveTexture, "glActiveTexture"); GP(p_CreateShader, "glCreateShader"); GP(p_ShaderSource, "glShaderSource");
    GP(p_CompileShader, "glCompileShader"); GP(p_GetShaderiv, "glGetShaderiv"); GP(p_GetShaderInfoLog, "glGetShaderInfoLog");
    GP(p_DeleteShader, "glDeleteShader"); GP(p_CreateProgram, "glCreateProgram"); GP(p_AttachShader, "glAttachShader"); GP(p_BindAttribLocation, "glBindAttribLocation");
    GP(p_LinkProgram, "glLinkProgram"); GP(p_GetProgramiv, "glGetProgramiv"); GP(p_GetProgramInfoLog, "glGetProgramInfoLog");
    GP(p_DeleteProgram, "glDeleteProgram"); GP(p_UseProgram, "glUseProgram"); GP(p_GetUniformLocation, "glGetUniformLocation");
    GP(p_Uniform1i, "glUniform1i"); GP(p_Uniform4f, "glUniform4f");
    GP(p_GenFramebuffers, "glGenFramebuffers"); GP(p_DeleteFramebuffers, "glDeleteFramebuffers"); GP(p_BindFramebuffer, "glBindFramebuffer");
    GP(p_FramebufferTexture2D, "glFramebufferTexture2D"); GP(p_FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    GP(p_CheckFramebufferStatus, "glCheckFramebufferStatus"); GP(p_GenRenderbuffers, "glGenRenderbuffers");
    GP(p_DeleteRenderbuffers, "glDeleteRenderbuffers"); GP(p_BindRenderbuffer, "glBindRenderbuffer"); GP(p_RenderbufferStorage, "glRenderbufferStorage");
    GP(p_RenderbufferStorageMultisample, "glRenderbufferStorageMultisample"); GP(p_BlitFramebuffer, "glBlitFramebuffer");
    #undef GP
    if (!p_ActiveTexture || !p_GenFramebuffers || !p_DeleteFramebuffers || !p_BindFramebuffer || !p_FramebufferTexture2D || !p_FramebufferRenderbuffer
        || !p_CheckFramebufferStatus || !p_GenRenderbuffers || !p_DeleteRenderbuffers || !p_BindRenderbuffer || !p_RenderbufferStorage || !p_BlitFramebuffer) {
        printf("postfx: this OpenGL has no framebuffer objects, no edge smoothing\n"); return -1; }
    GLint n = 0;
    if (p_RenderbufferStorageMultisample) { glGetIntegerv(GL_MAX_SAMPLES, &n); while (glGetError() != GL_NO_ERROR) { } }
    g_msmax = n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 0;
    return 1;
}
static int ready(void) { if (!g_state) g_state = init(); return g_state > 0; }

int postfx_msaa_max(void) { return ready() ? g_msmax : 0; }
int postfx_smaa_supported(void)
{
    if (!ready() || g_smaa_bad) return 0;
    if (g_glsl130 < 0) {
        const char *v = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION); int ma = 0, mi = 0;
#ifdef WOODY_GLES
        (void)v; (void)ma; (void)mi;              /* ready() = the blit is there = ES 3.0 (the shim): GLSL ES 3.00 */
        g_glsl130 = p_BindAttribLocation != NULL
#else
        g_glsl130 = v && sscanf(v, "%d.%d", &ma, &mi) == 2 && (ma > 1 || (ma == 1 && mi >= 30))
#endif
            && p_CreateShader && p_ShaderSource && p_CompileShader
            && p_GetShaderiv && p_GetShaderInfoLog && p_DeleteShader && p_CreateProgram && p_AttachShader && p_LinkProgram && p_GetProgramiv
            && p_GetProgramInfoLog && p_DeleteProgram && p_UseProgram && p_GetUniformLocation && p_Uniform1i && p_Uniform4f;
    }
    return g_glsl130;
}
void postfx_set(int msaa, int smaa, int depth) { g_msaa = msaa; g_smaa = smaa < 0 ? 0 : smaa > 4 ? 4 : smaa; g_depth = depth != 0; }
int postfx_samples(void) { return g_active ? g_samples : 0; }
int postfx_size(int *w, int *h) { *w = g_w; *h = g_h; return g_active; }

/* ---- SMAA programs: SMAA.hlsl between a prelude (version, language, preset, which half) and the entry point ---- */
static GLuint shader(GLenum type, const char *pre, const char *main_)
{
    const char *src[3] = { pre, k_smaa_src, main_ };
    GLuint s = p_CreateShader(type); GLint ok = 0; char log[2048];
    p_ShaderSource(s, 3, src, NULL); p_CompileShader(s); p_GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { p_GetShaderInfoLog(s, sizeof log, NULL, log); printf("postfx: SMAA shader: %s\n", log); p_DeleteShader(s); return 0; }
    return s;
}
static GLuint program(const char *preset, const char *vs, const char *fs)
{
    char pv[768], pf[768];
    static const char *k_pre = "%s#define SMAA_GLSL_3\n#define SMAA_PRESET_%s\n#define SMAA_RT_METRICS uMetrics\n"
                               "#define SMAA_INCLUDE_VS %d\n#define SMAA_INCLUDE_PS %d\nuniform vec4 uMetrics;\n%s";
#ifdef WOODY_GLES                             /* GLSL ES 3.00: the position from attribute 0 (glBegin of the shim), an own output */
    static const char *k_ver = "#version 300 es\nprecision highp float;\nprecision highp sampler2D;\n";
    static const char *k_vs_io = "in vec4 aPos;\n#define POS aPos\n", *k_fs_io = "out vec4 fragOut;\n#define FRAG fragOut\n";
#else
    static const char *k_ver = "#version 130\n", *k_vs_io = "#define POS gl_Vertex\n", *k_fs_io = "#define FRAG gl_FragColor\n";
#endif
    snprintf(pv, sizeof pv, k_pre, k_ver, preset, 1, 0, k_vs_io); snprintf(pf, sizeof pf, k_pre, k_ver, preset, 0, 1, k_fs_io);
    GLuint v = shader(GL_VERTEX_SHADER, pv, vs), f = v ? shader(GL_FRAGMENT_SHADER, pf, fs) : 0; GLint ok = 0; char log[2048];
    if (!v || !f) { if (v) p_DeleteShader(v); return 0; }
    GLuint p = p_CreateProgram(); p_AttachShader(p, v); p_AttachShader(p, f);
#ifdef WOODY_GLES
    p_BindAttribLocation(p, 0, "aPos");
#endif
    p_LinkProgram(p); p_GetProgramiv(p, GL_LINK_STATUS, &ok);
    p_DeleteShader(v); p_DeleteShader(f);
    if (!ok) { p_GetProgramInfoLog(p, sizeof log, NULL, log); printf("postfx: SMAA link: %s\n", log); p_DeleteProgram(p); return 0; }
    return p;
}
#define VS_UV "void fsq() { vUV = POS.xy * 0.5 + 0.5; gl_Position = vec4(POS.xy, 0.0, 1.0); }\n"
static const char *k_vs_edge = "out vec2 vUV; out vec4 vOff[3];\n" VS_UV "void main() { fsq(); SMAAEdgeDetectionVS(vUV, vOff); }\n";
static const char *k_fs_edge = "uniform sampler2D uColor; in vec2 vUV; in vec4 vOff[3];\n"
                               "void main() { FRAG = vec4(SMAAColorEdgeDetectionPS(vUV, vOff, uColor), 0.0, 0.0); }\n";
static const char *k_vs_weight = "out vec2 vUV; out vec2 vPix; out vec4 vOff[3];\n" VS_UV "void main() { fsq(); SMAABlendingWeightCalculationVS(vUV, vPix, vOff); }\n";
static const char *k_fs_weight = "uniform sampler2D uEdges, uArea, uSearch; in vec2 vUV; in vec2 vPix; in vec4 vOff[3];\n"
                                 "void main() { FRAG = SMAABlendingWeightCalculationPS(vUV, vPix, vOff, uEdges, uArea, uSearch, vec4(0.0)); }\n";
static const char *k_vs_blend = "out vec2 vUV; out vec4 vOff;\n" VS_UV "void main() { fsq(); SMAANeighborhoodBlendingVS(vUV, vOff); }\n";
static const char *k_fs_blend = "uniform sampler2D uColor, uBlend; in vec2 vUV; in vec4 vOff;\n"
                                "void main() { FRAG = vec4(SMAANeighborhoodBlendingPS(vUV, vOff, uColor, uBlend).rgb, 1.0); }\n";

static void tex_params(GLenum filter)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
static int smaa_programs(int preset)
{
    static const char *k_names[5] = { "", "LOW", "MEDIUM", "HIGH", "ULTRA" };
    if (preset == g_preset) return 1;
    if (pr_edge) p_DeleteProgram(pr_edge);
    if (pr_weight) p_DeleteProgram(pr_weight);
    if (pr_blend) p_DeleteProgram(pr_blend);
    pr_edge = pr_weight = pr_blend = 0; g_preset = 0;
    if (!(pr_edge = program(k_names[preset], k_vs_edge, k_fs_edge)) || !(pr_weight = program(k_names[preset], k_vs_weight, k_fs_weight))
        || !(pr_blend = program(k_names[preset], k_vs_blend, k_fs_blend))) { printf("postfx: SMAA shaders failed, SMAA is off\n"); g_smaa_bad = 1; return 0; }
    u_edge_m = p_GetUniformLocation(pr_edge, "uMetrics"); u_edge_col = p_GetUniformLocation(pr_edge, "uColor");
    u_weight_m = p_GetUniformLocation(pr_weight, "uMetrics"); u_weight_edge = p_GetUniformLocation(pr_weight, "uEdges");
    u_weight_area = p_GetUniformLocation(pr_weight, "uArea"); u_weight_search = p_GetUniformLocation(pr_weight, "uSearch");
    u_blend_m = p_GetUniformLocation(pr_blend, "uMetrics"); u_blend_col = p_GetUniformLocation(pr_blend, "uColor"); u_blend_w = p_GetUniformLocation(pr_blend, "uBlend");
    if (!tex_area) {                          /* the lookups, once: AreaTex RG (PNG grey + alpha), SearchTex R (PNG grey) */
        int w, h, c; unsigned char *a = stbi_load_from_memory(k_smaa_area_png, (int)sizeof k_smaa_area_png, &w, &h, &c, 2);
        unsigned char *s = stbi_load_from_memory(k_smaa_search_png, (int)sizeof k_smaa_search_png, &w, &h, &c, 1);
        if (!a || !s) { printf("postfx: SMAA lookup textures failed, SMAA is off\n"); stbi_image_free(a); stbi_image_free(s); g_smaa_bad = 1; return 0; }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glGenTextures(1, &tex_area); glBindTexture(GL_TEXTURE_2D, tex_area); tex_params(GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, SMAA_AREA_W, SMAA_AREA_H, 0, GL_RG, GL_UNSIGNED_BYTE, a);
        glGenTextures(1, &tex_search); glBindTexture(GL_TEXTURE_2D, tex_search); tex_params(GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, SMAA_SEARCH_W, SMAA_SEARCH_H, 0, GL_RED, GL_UNSIGNED_BYTE, s);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        stbi_image_free(a); stbi_image_free(s);
    }
    g_preset = preset; return 1;
}

/* ---- the targets ---- */
static void targets_free(void)
{
    GLuint f[4] = { fb_ms, fb_col, fb_edge, fb_blend }, r[3] = { rb_ms_col, rb_ms_ds, rb_ds }, t[3] = { tex_col, tex_edge, tex_blend };
    p_DeleteFramebuffers(4, f); p_DeleteRenderbuffers(3, r); glDeleteTextures(3, t);
    fb_ms = fb_col = fb_edge = fb_blend = rb_ms_col = rb_ms_ds = rb_ds = tex_col = tex_edge = tex_blend = 0; g_ms = -1; g_have_smaa = 0;
}
static GLuint colour_target(GLuint *tex, int w, int h)
{
    GLuint fb; glGenTextures(1, tex); glBindTexture(GL_TEXTURE_2D, *tex); tex_params(GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_GenFramebuffers(1, &fb); p_BindFramebuffer(GL_FRAMEBUFFER, fb); p_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    return fb;
}
static int complete(const char *what)
{
    GLenum st = p_CheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) printf("postfx: %s target incomplete (0x%x)\n", what, (unsigned)st);
    return st == GL_FRAMEBUFFER_COMPLETE;
}
static int targets(int w, int h, int ms, int smaa)
{
    if (w == g_w && h == g_h && ms == g_ms && smaa <= g_have_smaa) return 1;
    targets_free(); g_w = w; g_h = h;
    while (glGetError() != GL_NO_ERROR) { }
    int ok = 1;
    fb_col = colour_target(&tex_col, w, h);
    if (!ms) {                                /* the picture texture is the scene target: + depth / stencil */
        p_GenRenderbuffers(1, &rb_ds); p_BindRenderbuffer(GL_RENDERBUFFER, rb_ds); p_RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        p_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb_ds);
    }
    ok &= complete("picture");
    if (ms) {
        p_GenRenderbuffers(1, &rb_ms_col); p_BindRenderbuffer(GL_RENDERBUFFER, rb_ms_col); p_RenderbufferStorageMultisample(GL_RENDERBUFFER, ms, GL_RGBA8, w, h);
        p_GenRenderbuffers(1, &rb_ms_ds); p_BindRenderbuffer(GL_RENDERBUFFER, rb_ms_ds); p_RenderbufferStorageMultisample(GL_RENDERBUFFER, ms, GL_DEPTH24_STENCIL8, w, h);
        p_GenFramebuffers(1, &fb_ms); p_BindFramebuffer(GL_FRAMEBUFFER, fb_ms);
        p_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb_ms_col);
        p_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb_ms_ds);
        ok &= complete("MSAA");
    }
    if (smaa) { fb_edge = colour_target(&tex_edge, w, h); ok &= complete("SMAA edges"); fb_blend = colour_target(&tex_blend, w, h); ok &= complete("SMAA weights"); }
    p_BindRenderbuffer(GL_RENDERBUFFER, 0); p_BindFramebuffer(GL_FRAMEBUFFER, 0); glBindTexture(GL_TEXTURE_2D, 0);
    if (!ok || glGetError() != GL_NO_ERROR) { printf("postfx: no %dx%d target (%d samples), edge smoothing is off\n", w, h, ms); targets_free(); g_state = -1; return 0; }
    g_ms = ms; g_have_smaa = smaa;
    return 1;
}

void postfx_begin(int w, int h)
{
    g_active = 0;
    int own = g_depth && OWN_DEPTH;
    if ((!g_msaa && !g_smaa && !own) || w <= 0 || h <= 0 || !ready()) return;
    int ms = g_msaa > g_msmax ? g_msmax : g_msaa, smaa = g_smaa && postfx_smaa_supported();
    if (ms < 2) ms = 0;
    if ((!ms && !smaa && !own) || !targets(w, h, ms, smaa)) return;
    p_BindFramebuffer(GL_FRAMEBUFFER, ms ? fb_ms : fb_col);
    g_active = 1; g_samples = ms;
}

static void fsq(void) { glBegin(GL_TRIANGLES); glVertex2f(-1, -1); glVertex2f(3, -1); glVertex2f(-1, 3); glEnd(); }

void postfx_end(void)
{
    if (!g_active) return;
    g_active = 0;
    int w = g_w, h = g_h;
    if (g_samples) {                                                   /* resolve the samples into the picture texture */
        p_BindFramebuffer(GL_READ_FRAMEBUFFER, fb_ms); p_BindFramebuffer(GL_DRAW_FRAMEBUFFER, fb_col);
        p_BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    if (!g_smaa || !g_have_smaa || !smaa_programs(g_smaa)) {           /* no SMAA: the picture into the window */
        p_BindFramebuffer(GL_READ_FRAMEBUFFER, fb_col); p_BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        p_BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        p_BindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    p_BindFramebuffer(GL_FRAMEBUFFER, 0);                              /* first: the attribute stack keeps the draw buffer of the bound framebuffer */
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDisable(GL_ALPHA_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST); glDisable(GL_POLYGON_OFFSET_FILL); glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); glViewport(0, 0, w, h); glClearColor(0, 0, 0, 0);
    float m[4] = { 1.0f / w, 1.0f / h, (float)w, (float)h };
    /* 1. edges */
    p_BindFramebuffer(GL_FRAMEBUFFER, fb_edge); glClear(GL_COLOR_BUFFER_BIT);
    p_ActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex_col);
    p_UseProgram(pr_edge); p_Uniform4f(u_edge_m, m[0], m[1], m[2], m[3]); p_Uniform1i(u_edge_col, 0);
    fsq();
    /* 2. blending weights */
    p_BindFramebuffer(GL_FRAMEBUFFER, fb_blend); glClear(GL_COLOR_BUFFER_BIT);
    glBindTexture(GL_TEXTURE_2D, tex_edge);
    p_ActiveTexture(GL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D, tex_area);
    p_ActiveTexture(GL_TEXTURE0 + 2); glBindTexture(GL_TEXTURE_2D, tex_search);
    p_UseProgram(pr_weight); p_Uniform4f(u_weight_m, m[0], m[1], m[2], m[3]);
    p_Uniform1i(u_weight_edge, 0); p_Uniform1i(u_weight_area, 1); p_Uniform1i(u_weight_search, 2);
    fsq();
    /* 3. neighbourhood blending into the window */
    p_BindFramebuffer(GL_FRAMEBUFFER, 0);
    p_ActiveTexture(GL_TEXTURE0 + 2); glBindTexture(GL_TEXTURE_2D, 0);
    p_ActiveTexture(GL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D, tex_blend);
    p_ActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex_col);
    p_UseProgram(pr_blend); p_Uniform4f(u_blend_m, m[0], m[1], m[2], m[3]); p_Uniform1i(u_blend_col, 0); p_Uniform1i(u_blend_w, 1);
    fsq();
    p_UseProgram(0);
    p_ActiveTexture(GL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D, 0); p_ActiveTexture(GL_TEXTURE0);
    glPopAttrib();
}
