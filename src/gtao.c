/* gtao.c - ground-truth ambient occlusion (PORT EXTRA, docs/DISPLAY.md 5). The original has no such pass: it lights the
 * world from the baked .lit polygons and the models per vertex (docs/LIGHTING.md), so this is purely an option of the port,
 * off by default.
 *
 * After the opaque world and models (rnd_frame, before the additive world faces, the water, the fade list and the sprites):
 *   1. the depth buffer of the 3D viewport is copied into a depth texture (glCopyTexSubImage2D, GL 1.4);
 *   2. a full-screen GLSL pass into an RGBA8 framebuffer object computes the visibility per pixel the way of Jimenez et al.,
 *      "Practical Real-Time Strategies for Accurate Indirect Occlusion" (2016), in the form of Intel's XeGTAO: view-space
 *      position and normal from the depth, SLICES directions around the view vector, per direction the highest horizon on
 *      both sides within RADIUS world units (with a distance falloff), the cosine-weighted visible arc against the normal
 *      projected into the slice. The slice rotation and the step offset come from a 4x4 Bayer tile;
 *   3. a 4x4 depth-aware box (exactly one noise tile) blurs it and multiplies it into the colour (blend ZERO, SRC_COLOR).
 * With MSAA on (postfx.c) the picture is drawn into a multisampled target: its depth is resolved into a depth / stencil
 * texture with glBlitFramebuffer instead, and the occlusion is multiplied into that target.
 * Desktop OpenGL (Windows / Linux compatibility contexts: GLSL 1.20 + framebuffer objects) or OpenGL ES 3.0 (the Android
 * and Switch builds, through the shim of src/gles: GLSL ES 1.00). ES cannot copy the window's depth, so there postfx.c draws
 * the picture into its own target while the option is on, and the whole depth / stencil of that target is blitted into
 * a texture of its size (a multisampled blit must keep the rectangle); the shaders find the viewport in it by uDMap. */
#include "gtao.h"
#include "postfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#endif
#include "plat.h"
#include <GL/gl.h>

#define RADIUS 120.0f                         /* world units (Woody is 193 tall) */
#define POWER  1.5f                           /* visibility^POWER: a little more contrast than the raw integral */

#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#ifndef GL_DEPTH_TEXTURE_MODE
#define GL_DEPTH_TEXTURE_MODE 0x884B
#endif
#ifndef GL_TEXTURE_COMPARE_MODE
#define GL_TEXTURE_COMPARE_MODE 0x884C
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH_STENCIL 0x84F9
#define GL_UNSIGNED_INT_24_8 0x84FA
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef APIENTRY
#define APIENTRY GL_APIENTRY
#endif
#ifdef WOODY_GLES
#define GLES 1
#else
#define GLES 0
#endif

/* own names (p_*): Mesa's gl.h already declares some of these (glActiveTexture) as functions */
typedef char GLch;
static void   (APIENTRY *p_ActiveTexture)(GLenum);
static GLuint (APIENTRY *p_CreateShader)(GLenum);
static void   (APIENTRY *p_ShaderSource)(GLuint, GLsizei, const GLch *const *, const GLint *);
static void   (APIENTRY *p_CompileShader)(GLuint);
static void   (APIENTRY *p_GetShaderiv)(GLuint, GLenum, GLint *);
static void   (APIENTRY *p_GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLch *);
static GLuint (APIENTRY *p_CreateProgram)(void);
static void   (APIENTRY *p_AttachShader)(GLuint, GLuint);
static void   (APIENTRY *p_BindAttribLocation)(GLuint, GLuint, const GLch *);
static void   (APIENTRY *p_LinkProgram)(GLuint);
static void   (APIENTRY *p_GetProgramiv)(GLuint, GLenum, GLint *);
static void   (APIENTRY *p_GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLch *);
static void   (APIENTRY *p_UseProgram)(GLuint);
static GLint  (APIENTRY *p_GetUniformLocation)(GLuint, const GLch *);
static void   (APIENTRY *p_Uniform1i)(GLint, GLint);
static void   (APIENTRY *p_Uniform1f)(GLint, GLfloat);
static void   (APIENTRY *p_Uniform2f)(GLint, GLfloat, GLfloat);
static void   (APIENTRY *p_Uniform3f)(GLint, GLfloat, GLfloat, GLfloat);
static void   (APIENTRY *p_Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
static void   (APIENTRY *p_GenFramebuffers)(GLsizei, GLuint *);
static void   (APIENTRY *p_BindFramebuffer)(GLenum, GLuint);
static void   (APIENTRY *p_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static GLenum (APIENTRY *p_CheckFramebufferStatus)(GLenum);
static void   (APIENTRY *p_BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

static int g_on;                              /* the setting */
static int g_state;                           /* 0 = not tried yet, 1 = ready, -1 = this GL cannot */
static GLuint g_prog_ao, g_prog_mix, g_fbo, g_tex_depth, g_tex_ao; static int g_tw, g_th;
static GLuint g_fbo_ds, g_tex_ds; static int g_dw, g_dh;   /* MSAA / ES: the blitted depth / stencil (made on first use) */
static GLint u_ao_dmap, u_mix_dmap, u_ao_depth, u_ao_size, u_ao_z, u_ao_scale, u_ao_radius, u_mix_ao, u_mix_depth, u_mix_size, u_mix_origin, u_mix_z, u_mix_power;

#ifdef WOODY_GLES                             /* GLSL ES 1.00: the position from attribute 0 (glBegin of the shim) */
static const char *k_vs = "#version 100\nattribute vec4 aPos;\nvoid main() { gl_Position = aPos; }\n";
static const char *k_fs_pre = "#version 100\nprecision highp float;\nprecision highp sampler2D;\n";   /* (a sampler is lowp by default) */
#else
static const char *k_vs = "#version 120\nvoid main() { gl_Position = gl_Vertex; }\n";
static const char *k_fs_pre = "#version 120\n";
#endif

static const char *k_fs_ao =
    "uniform sampler2D uDepth;\n"
    "uniform vec4 uDMap;\n"              /* the viewport's origin in the depth texture, its size */
    "uniform vec2 uSize;\n"                   /* the viewport in pixels */
    "uniform vec3 uZ;\n"                      /* zn * zf, zf - zn, zf */
    "uniform vec2 uScale;\n"                  /* projection x / y scale */
    "uniform float uRadius;\n"
    "const float PI = 3.14159265, HALF_PI = 1.57079633;\n"
    "const int SLICES = 3, STEPS = 6;\n"
    "float lin(float d) { return uZ.x / (uZ.z - d * uZ.y); }\n"
    "float dep(vec2 uv) { return texture2D(uDepth, (uv * uSize + uDMap.xy) / uDMap.zw).r; }\n"
    "vec3 viewPos(vec2 uv, float d) { float L = lin(d); vec2 n = uv * 2.0 - 1.0; return vec3(n.x * L / uScale.x, n.y * L / uScale.y, -L); }\n"
    "vec3 posAt(vec2 uv) { return viewPos(uv, dep(uv)); }\n"
    "float fastAcos(float x) { float r = (-0.156583 * abs(x) + HALF_PI) * sqrt(1.0 - abs(x)); return x >= 0.0 ? r : PI - r; }\n"
    "float bayer2(vec2 v) { return mod(2.0 * v.x + 3.0 * v.y, 4.0); }\n"
    "void main() {\n"
    "    vec2 inv = 1.0 / uSize, uv = gl_FragCoord.xy * inv;\n"
    "    float d = dep(uv);\n"
    "    if (d >= 1.0) { gl_FragColor = vec4(1.0); return; }\n"
    "    vec3 P = viewPos(uv, d);\n"
    /* the normal from the neighbour on the same surface per axis (the smaller depth step) */
    "    vec3 l = posAt(uv - vec2(inv.x, 0.0)), r = posAt(uv + vec2(inv.x, 0.0)), b = posAt(uv - vec2(0.0, inv.y)), t = posAt(uv + vec2(0.0, inv.y));\n"
    "    vec3 dx = abs(r.z - P.z) < abs(P.z - l.z) ? r - P : P - l;\n"
    "    vec3 dy = abs(t.z - P.z) < abs(P.z - b.z) ? t - P : P - b;\n"
    "    vec3 V = normalize(-P), N = normalize(cross(dx, dy));\n"
    "    if (dot(N, V) < 0.0) N = -N;\n"
    "    float rpx = min(uRadius * uScale.y * 0.5 * uSize.y / -P.z, 0.25 * uSize.y);\n"   /* the radius in pixels, capped */
    "    if (rpx < 1.0) { gl_FragColor = vec4(1.0); return; }\n"
    "    vec2 c = mod(floor(gl_FragCoord.xy), 4.0);\n"
    "    float bay = 4.0 * bayer2(mod(c, 2.0)) + bayer2(floor(c * 0.5));\n"                /* 0..15, one 4x4 tile */
    "    float nSlice = (bay + 0.5) / 16.0, nStep = (mod(bay * 5.0 + 3.0, 16.0) + 0.5) / 16.0;\n"
    "    float fMul = -1.0 / (0.615 * uRadius), fAdd = (1.0 - 0.615) / 0.615 + 1.0;\n"    /* falloff over the outer 61.5 % */
    "    float vis = 0.0;\n"
    "    for (int s = 0; s < SLICES; s++) {\n"
    "        float phi = (float(s) + nSlice) * (PI / float(SLICES));\n"
    "        vec2 om = vec2(cos(phi), sin(phi));\n"
    "        vec3 dir = vec3(om, 0.0), ortho = dir - dot(dir, V) * V, axis = normalize(cross(ortho, V));\n"
    "        vec3 pN = N - axis * dot(N, axis); float pLen = length(pN);\n"
    "        if (pLen < 1e-4) { vis += 1.0; continue; }\n"
    "        float cosN = clamp(dot(pN, V) / pLen, 0.0, 1.0), n = sign(dot(ortho, pN)) * fastAcos(cosN);\n"
    "        float low0 = cos(n + HALF_PI), low1 = cos(n - HALF_PI), h0 = low0, h1 = low1;\n"
    "        for (int k = 0; k < STEPS; k++) {\n"
    "            float st = (float(k) + nStep) / float(STEPS); st *= st;\n"
    "            vec2 off = om * (st * rpx + 1.0) * inv;\n"
    "            vec3 d0 = posAt(uv + off) - P, d1 = posAt(uv - off) - P;\n"
    "            float l0 = length(d0), l1 = length(d1);\n"
    "            h0 = max(h0, mix(low0, dot(d0, V) / l0, clamp(l0 * fMul + fAdd, 0.0, 1.0)));\n"
    "            h1 = max(h1, mix(low1, dot(d1, V) / l1, clamp(l1 * fMul + fAdd, 0.0, 1.0)));\n"
    "        }\n"
    "        float a1 = n + clamp(fastAcos(h0) - n, -HALF_PI, HALF_PI), a0 = n + clamp(-fastAcos(h1) - n, -HALF_PI, HALF_PI), sn = sin(n);\n"
    "        vis += pLen * ((cosN + 2.0 * a0 * sn - cos(2.0 * a0 - n)) + (cosN + 2.0 * a1 * sn - cos(2.0 * a1 - n))) * 0.25;\n"
    "    }\n"
    "    gl_FragColor = vec4(vec3(clamp(vis / float(SLICES), 0.0, 1.0)), 1.0);\n"
    "}\n";

static const char *k_fs_mix =
    "uniform sampler2D uAO, uDepth;\n"
    "uniform vec2 uSize, uOrigin;\n"
    "uniform vec4 uDMap;\n"
    "uniform vec3 uZ;\n"
    "uniform float uPower;\n"
    "float lin(float d) { return uZ.x / (uZ.z - d * uZ.y); }\n"
    "float dep(vec2 uv) { return texture2D(uDepth, (uv * uSize + uDMap.xy) / uDMap.zw).r; }\n"
    "void main() {\n"
    "    vec2 inv = 1.0 / uSize, uv = (gl_FragCoord.xy - uOrigin) * inv;\n"
    "    float d = dep(uv);\n"
    "    if (d >= 1.0) { gl_FragColor = vec4(1.0); return; }\n"
    "    float Lc = lin(d), sum = 0.0, wsum = 0.0;\n"
    "    for (int j = -2; j < 2; j++) for (int i = -2; i < 2; i++) {\n"
    "        vec2 q = uv + vec2(float(i), float(j)) * inv;\n"
    "        float w = exp(-abs(lin(dep(q)) - Lc) / (0.04 * Lc));\n"
    "        sum += texture2D(uAO, q).r * w; wsum += w;\n"
    "    }\n"
    "    float ao = pow(sum / wsum, uPower);\n"
    "    gl_FragColor = vec4(ao, ao, ao, 1.0);\n"
    "}\n";

static GLuint shader(GLenum type, const char *pre, const char *main_)
{
    const char *src[2] = { pre, main_ };
    GLuint s = p_CreateShader(type); GLint ok = 0; char log[1024];
    p_ShaderSource(s, 2, src, NULL); p_CompileShader(s); p_GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { p_GetShaderInfoLog(s, sizeof log, NULL, log); printf("gtao: shader: %s\n", log); return 0; }
    return s;
}
static GLuint program(const char *fs)
{
    GLuint v = shader(GL_VERTEX_SHADER, "", k_vs), f = shader(GL_FRAGMENT_SHADER, k_fs_pre, fs); GLint ok = 0; char log[1024];
    if (!v || !f) return 0;
    GLuint p = p_CreateProgram(); p_AttachShader(p, v); p_AttachShader(p, f);
    if (GLES) p_BindAttribLocation(p, 0, "aPos");
    p_LinkProgram(p); p_GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { p_GetProgramInfoLog(p, sizeof log, NULL, log); printf("gtao: link: %s\n", log); return 0; }
    return p;
}

static int init(void)
{
    #define GP(v, name) do { void (*f_)(void) = plat_gl_proc(name); memcpy(&v, &f_, sizeof f_); } while (0)
    GP(p_ActiveTexture, "glActiveTexture"); GP(p_CreateShader, "glCreateShader"); GP(p_ShaderSource, "glShaderSource");
    GP(p_CompileShader, "glCompileShader"); GP(p_GetShaderiv, "glGetShaderiv"); GP(p_GetShaderInfoLog, "glGetShaderInfoLog");
    GP(p_CreateProgram, "glCreateProgram"); GP(p_AttachShader, "glAttachShader"); GP(p_LinkProgram, "glLinkProgram");
    GP(p_BindAttribLocation, "glBindAttribLocation");
    GP(p_GetProgramiv, "glGetProgramiv"); GP(p_GetProgramInfoLog, "glGetProgramInfoLog"); GP(p_UseProgram, "glUseProgram");
    GP(p_GetUniformLocation, "glGetUniformLocation"); GP(p_Uniform1i, "glUniform1i"); GP(p_Uniform1f, "glUniform1f");
    GP(p_Uniform2f, "glUniform2f"); GP(p_Uniform3f, "glUniform3f"); GP(p_Uniform4f, "glUniform4f");
    GP(p_GenFramebuffers, "glGenFramebuffers"); GP(p_BindFramebuffer, "glBindFramebuffer");
    GP(p_FramebufferTexture2D, "glFramebufferTexture2D"); GP(p_CheckFramebufferStatus, "glCheckFramebufferStatus");
    GP(p_BlitFramebuffer, "glBlitFramebuffer");
    #undef GP
    if (!p_ActiveTexture || !p_CreateShader || !p_ShaderSource || !p_CompileShader || !p_GetShaderiv || !p_GetShaderInfoLog || !p_CreateProgram
        || !p_AttachShader || !p_LinkProgram || !p_GetProgramiv || !p_GetProgramInfoLog || !p_UseProgram || !p_GetUniformLocation || !p_Uniform1i
        || !p_Uniform1f || !p_Uniform2f || !p_Uniform3f || !p_Uniform4f || !p_GenFramebuffers || !p_BindFramebuffer || !p_FramebufferTexture2D || !p_CheckFramebufferStatus) {
        printf("gtao: this OpenGL has no GLSL / framebuffer objects, ambient occlusion is off\n"); return -1; }
    if (GLES && (!p_BlitFramebuffer || !p_BindAttribLocation)) { printf("gtao: OpenGL ES 2.0, ambient occlusion is off\n"); return -1; }
    if (!(g_prog_ao = program(k_fs_ao)) || !(g_prog_mix = program(k_fs_mix))) { printf("gtao: shaders failed, ambient occlusion is off\n"); return -1; }
    u_ao_depth = p_GetUniformLocation(g_prog_ao, "uDepth"); u_ao_size = p_GetUniformLocation(g_prog_ao, "uSize"); u_ao_z = p_GetUniformLocation(g_prog_ao, "uZ");
    u_ao_scale = p_GetUniformLocation(g_prog_ao, "uScale"); u_ao_radius = p_GetUniformLocation(g_prog_ao, "uRadius");
    u_ao_dmap = p_GetUniformLocation(g_prog_ao, "uDMap"); u_mix_dmap = p_GetUniformLocation(g_prog_mix, "uDMap");
    u_mix_ao = p_GetUniformLocation(g_prog_mix, "uAO"); u_mix_depth = p_GetUniformLocation(g_prog_mix, "uDepth"); u_mix_size = p_GetUniformLocation(g_prog_mix, "uSize");
    u_mix_origin = p_GetUniformLocation(g_prog_mix, "uOrigin"); u_mix_z = p_GetUniformLocation(g_prog_mix, "uZ"); u_mix_power = p_GetUniformLocation(g_prog_mix, "uPower");
    glGenTextures(1, &g_tex_depth); glGenTextures(1, &g_tex_ao); p_GenFramebuffers(1, &g_fbo);
    return 1;
}

int gtao_supported(void) { if (!g_state) g_state = init(); return g_state > 0; }
void gtao_enable(int on) { g_on = on != 0; }

static void tex_params(void)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
static void quad(void) { glBegin(GL_QUADS); glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1); glEnd(); }

void gtao_frame(float zn, float zf, float sx, float sy)
{
    if (!g_on || !gtao_supported()) return;
    GLint vp[4], scene = 0; glGetIntegerv(GL_VIEWPORT, vp); glGetIntegerv(GL_FRAMEBUFFER_BINDING, &scene);   /* the window, or postfx.c's target */
    int w = vp[2], h = vp[3]; if (w <= 0 || h <= 0) return;
    /* the depth: the viewport copied (desktop), or blitted into a depth / stencil texture: under MSAA (one sample of each
     * pixel) and always on ES, there the whole target (dw x dh, the viewport at dx, dy in it) */
    int blit = postfx_samples() > 0 || GLES, dx = 0, dy = 0, dw = w, dh = h;
    if (GLES) { if (!postfx_size(&dw, &dh)) return; dx = vp[0]; dy = vp[1]; }   /* (postfx.c draws into its target while the option is on) */
    if (blit && !p_BlitFramebuffer) return;
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    p_ActiveTexture(GL_TEXTURE0);
    if (w != g_tw || h != g_th) {                                       /* (re)size the targets with the viewport */
        if (!GLES) {
            glBindTexture(GL_TEXTURE_2D, g_tex_depth); tex_params();
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE); glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_LUMINANCE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
        }
        glBindTexture(GL_TEXTURE_2D, g_tex_ao); tex_params();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        p_BindFramebuffer(GL_FRAMEBUFFER, g_fbo); p_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_tex_ao, 0);
        GLenum st = p_CheckFramebufferStatus(GL_FRAMEBUFFER); p_BindFramebuffer(GL_FRAMEBUFFER, (GLuint)scene);
        if (st != GL_FRAMEBUFFER_COMPLETE) { printf("gtao: framebuffer incomplete (0x%x), ambient occlusion is off\n", (unsigned)st); g_state = -1; glPopAttrib(); return; }
        g_tw = w; g_th = h;
    }
    if (blit && (dw != g_dw || dh != g_dh)) {                           /* a depth / stencil texture to blit into, in the target's format */
        if (!g_tex_ds) { glGenTextures(1, &g_tex_ds); p_GenFramebuffers(1, &g_fbo_ds); }
        glBindTexture(GL_TEXTURE_2D, g_tex_ds); tex_params();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        if (!GLES) glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_LUMINANCE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, dw, dh, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
        p_BindFramebuffer(GL_FRAMEBUFFER, g_fbo_ds); p_FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, g_tex_ds, 0);
#ifndef WOODY_GLES
        glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE);
#endif
        GLenum st = p_CheckFramebufferStatus(GL_FRAMEBUFFER); p_BindFramebuffer(GL_FRAMEBUFFER, (GLuint)scene);
        if (st != GL_FRAMEBUFFER_COMPLETE) { printf("gtao: depth target incomplete (0x%x), ambient occlusion is off\n", (unsigned)st); g_state = -1; glPopAttrib(); return; }
        g_dw = dw; g_dh = dh;
    }
    glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDisable(GL_ALPHA_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND); glDisable(GL_POLYGON_OFFSET_FILL); glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    /* 1. the depth */
    GLuint depth = blit ? g_tex_ds : g_tex_depth;
    if (blit) {
        p_BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)scene); p_BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbo_ds);
        if (GLES) p_BlitFramebuffer(0, 0, dw, dh, 0, 0, dw, dh, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        else p_BlitFramebuffer(vp[0], vp[1], vp[0] + w, vp[1] + h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    }
    glBindTexture(GL_TEXTURE_2D, depth);
    if (!blit) glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vp[0], vp[1], w, h);
    /* 2. the occlusion into the RGBA8 target */
    p_BindFramebuffer(GL_FRAMEBUFFER, g_fbo); glViewport(0, 0, w, h);
    p_UseProgram(g_prog_ao);
    p_Uniform1i(u_ao_depth, 0); p_Uniform2f(u_ao_size, (float)w, (float)h); p_Uniform3f(u_ao_z, zn * zf, zf - zn, zf);
    p_Uniform2f(u_ao_scale, sx, sy); p_Uniform1f(u_ao_radius, RADIUS); p_Uniform4f(u_ao_dmap, (float)dx, (float)dy, (float)dw, (float)dh);
    quad();
    /* 3. blur and multiply into the frame */
    p_BindFramebuffer(GL_FRAMEBUFFER, (GLuint)scene); glViewport(vp[0], vp[1], w, h);
    p_ActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, g_tex_ao); p_ActiveTexture(GL_TEXTURE0);
    p_UseProgram(g_prog_mix);
    p_Uniform1i(u_mix_ao, 1); p_Uniform1i(u_mix_depth, 0); p_Uniform2f(u_mix_size, (float)w, (float)h); p_Uniform2f(u_mix_origin, (float)vp[0], (float)vp[1]);
    p_Uniform3f(u_mix_z, zn * zf, zf - zn, zf); p_Uniform1f(u_mix_power, POWER); p_Uniform4f(u_mix_dmap, (float)dx, (float)dy, (float)dw, (float)dh);
    glEnable(GL_BLEND); glBlendFunc(GL_ZERO, GL_SRC_COLOR);
    quad();
    p_UseProgram(0);
    p_ActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0); p_ActiveTexture(GL_TEXTURE0);
    glPopAttrib();
}
