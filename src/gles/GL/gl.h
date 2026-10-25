/* GL/gl.h for OpenGL ES 2.0 / 3.0 (PORT EXTRA, the Android build: android/app/CMakeLists.txt puts src/gles first on the
 * include path, so every #include <GL/gl.h> of the engine lands here). The engine draws with the fixed-function pipeline of
 * desktop OpenGL 1.1; src/gles/gles2.c emulates the part of it the engine uses with one shader pair:
 * - the projection and modelview matrix stacks (load, multiply, translate, scale, ortho, push / pop)
 * - vertex colours and the current colour, two texture units with GL_MODULATE / GL_REPLACE / GL_ADD / GL_DECAL and
 *   GL_COMBINE (modulate, GL_RGB_SCALE 1 or 2: the MODULATE2X of the original), the alpha test
 * - client arrays (vertex, colour, texcoords of both units) as vertex attributes, glBegin / glEnd as client arrays
 *   (GL_QUADS as triangles, GL_POLYGON as a fan)
 * - glReadPixels into GL_RGB, GL_CLAMP (= CLAMP_TO_EDGE), glDrawElements with GL_UNSIGNED_INT on a bare ES 2.0
 * glPolygonMode (the F3 wireframe), lighting and fog are not there (the engine never turns the last two on).
 * For the shader passes of the Graphics page (gtao.c, postfx.c, ES 3.0 only): glUseProgram of an own program (glBegin then
 * feeds its attribute 0 the positions), glPushAttrib / glPopAttrib of the state they change, and plat_gl_proc hands out the
 * ES 2.0 / 3.0 entry points they ask for. */
#ifndef WOODY_GLES_GL_H
#define WOODY_GLES_GL_H
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#define WOODY_GLES 1                          /* the engine draws through this shim */

typedef double GLdouble;
/* the desktop / ES 1.1 names the engine uses */
#define GL_QUADS                 0x0007
#define GL_QUAD_STRIP            0x0008
#define GL_POLYGON               0x0009
#define GL_CLAMP                 0x2900
#define GL_LINE                  0x1B01
#define GL_FILL                  0x1B02
#define GL_MODELVIEW             0x1700
#define GL_PROJECTION            0x1701
#define GL_TEXTURE               0x1702
#define GL_VERTEX_ARRAY          0x8074
#define GL_NORMAL_ARRAY          0x8075
#define GL_COLOR_ARRAY           0x8076
#define GL_TEXTURE_COORD_ARRAY   0x8078
#define GL_ALPHA_TEST            0x0BC0
#define GL_LIGHTING              0x0B50
#define GL_FOG                   0x0B60
#define GL_NORMALIZE             0x0BA1
#define GL_COLOR_MATERIAL        0x0B57
#define GL_LINE_SMOOTH           0x0B20
#define GL_POINT_SMOOTH          0x0B10
#define GL_MULTISAMPLE           0x809D
#define GL_TEXTURE_ENV           0x2300
#define GL_TEXTURE_ENV_MODE      0x2200
#define GL_MODULATE              0x2100
#define GL_DECAL                 0x2101
#define GL_ADD                   0x0104
#define GL_COMBINE               0x8570
#define GL_COMBINE_RGB           0x8571
#define GL_COMBINE_ALPHA         0x8572
#define GL_RGB_SCALE             0x8573
#define GL_PRIMARY_COLOR         0x8577
#define GL_PREVIOUS              0x8578
#define GL_SRC0_RGB              0x8580
#define GL_SRC1_RGB              0x8581
#define GL_SRC0_ALPHA            0x8588
#define GL_SRC1_ALPHA            0x8589
#define GL_BLEND_SRC             0x0BE1
#define GL_BLEND_DST             0x0BE0
#define GL_ALL_ATTRIB_BITS       0xFFFFFFFF

void gles_begin(GLenum mode);
void gles_end(void);
void gles_vertex3f(GLfloat x, GLfloat y, GLfloat z);
void gles_texcoord2f(GLfloat s, GLfloat t);
void gles_multitexcoord2f(GLenum unit, GLfloat s, GLfloat t);
void gles_color4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void gles_tex_parameteri(GLenum target, GLenum pname, GLint param);
void gles_read_pixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *px);
void gles_draw_arrays(GLenum mode, GLint first, GLsizei count);
void gles_draw_elements(GLenum mode, GLsizei count, GLenum type, const void *idx);
void gles_enable_client_state(GLenum array);
void gles_disable_client_state(GLenum array);
void gles_client_active_texture(GLenum unit);
void gles_vertex_pointer(GLint size, GLenum type, GLsizei stride, const void *p);
void gles_color_pointer(GLint size, GLenum type, GLsizei stride, const void *p);
void gles_tex_coord_pointer(GLint size, GLenum type, GLsizei stride, const void *p);
void gles_enable(GLenum cap);
void gles_disable(GLenum cap);
GLboolean gles_is_enabled(GLenum cap);
void gles_active_texture(GLenum unit);
void gles_viewport(GLint x, GLint y, GLsizei w, GLsizei h);
void gles_blend_func(GLenum src, GLenum dst);
void gles_get_integerv(GLenum pname, GLint *v);
void gles_matrix_mode(GLenum mode);
void gles_load_identity(void);
void gles_load_matrixf(const GLfloat *m);
void gles_mult_matrixf(const GLfloat *m);
void gles_translatef(GLfloat x, GLfloat y, GLfloat z);
void gles_scalef(GLfloat x, GLfloat y, GLfloat z);
void gles_ortho(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f);
void gles_push_matrix(void);
void gles_pop_matrix(void);
void gles_tex_envi(GLenum target, GLenum pname, GLint v);
void gles_tex_envf(GLenum target, GLenum pname, GLfloat v);
void gles_get_tex_enviv(GLenum target, GLenum pname, GLint *v);
void gles_alpha_func(GLenum func, GLfloat ref);
void gles_use_program(GLuint p);
void gles_push_attrib(GLbitfield mask);
void gles_pop_attrib(void);
void (*gles_proc(const char *name))(void);   /* plat_gl_proc: the extension entry points the engine asks for */

#ifndef WOODY_GLES_IMPL
#define glBegin                   gles_begin
#define glEnd                     gles_end
#define glVertex2f(x, y)          gles_vertex3f((x), (y), 0.0f)
#define glVertex3f                gles_vertex3f
#define glVertex3fv(v)            gles_vertex3f((v)[0], (v)[1], (v)[2])
#define glTexCoord2f              gles_texcoord2f
#define glTexCoord2fv(v)          gles_texcoord2f((v)[0], (v)[1])
#define glColor3f(r, g, b)        gles_color4f((r), (g), (b), 1.0f)
#define glColor4f                 gles_color4f
#define glColor4fv(v)             gles_color4f((v)[0], (v)[1], (v)[2], (v)[3])
#define glOrtho(l, r, b, t, n, f) gles_ortho((GLfloat)(l), (GLfloat)(r), (GLfloat)(b), (GLfloat)(t), (GLfloat)(n), (GLfloat)(f))
#define glPolygonMode(face, mode) ((void)(face), (void)(mode))
#define glTexParameteri           gles_tex_parameteri
#define glReadPixels              gles_read_pixels
#define glDrawArrays              gles_draw_arrays
#define glDrawElements            gles_draw_elements
#define glEnableClientState       gles_enable_client_state
#define glDisableClientState      gles_disable_client_state
#define glClientActiveTexture     gles_client_active_texture
#define glVertexPointer           gles_vertex_pointer
#define glColorPointer            gles_color_pointer
#define glTexCoordPointer         gles_tex_coord_pointer
#define glEnable                  gles_enable
#define glDisable                 gles_disable
#define glIsEnabled               gles_is_enabled
#define glActiveTexture           gles_active_texture
#define glViewport                gles_viewport
#define glBlendFunc               gles_blend_func
#define glGetIntegerv             gles_get_integerv
#define glMatrixMode              gles_matrix_mode
#define glLoadIdentity            gles_load_identity
#define glLoadMatrixf             gles_load_matrixf
#define glMultMatrixf             gles_mult_matrixf
#define glTranslatef              gles_translatef
#define glScalef                  gles_scalef
#define glPushMatrix              gles_push_matrix
#define glPopMatrix               gles_pop_matrix
#define glTexEnvi                 gles_tex_envi
#define glTexEnvf                 gles_tex_envf
#define glGetTexEnviv             gles_get_tex_enviv
#define glAlphaFunc               gles_alpha_func
#define glUseProgram              gles_use_program
#define glPushAttrib              gles_push_attrib
#define glPopAttrib               gles_pop_attrib
#endif
#endif
