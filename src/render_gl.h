/* render_gl.h - minimal OpenGL 1.1 renderer for the reimplemented engine; the window is Win32 + WGL (render_gl.c) on Windows
 * and SDL2 (plat_sdl.c) elsewhere. */
#ifndef WOODY_RENDER_GL_H
#define WOODY_RENDER_GL_H
#include "level.h"

typedef struct {
    Vec3 pos; float yaw, pitch;               /* radians; yaw 0 looks along +z (D3D convention) */
    float fov_deg;
    int letterbox;                            /* camera mode 4: 16:9 strip, shifted up (0x41f910) */
} FreeCamera;

typedef struct {
    int width, height;
    int keys[256];                            /* current key state (VK codes) */
    int mouse_dx, mouse_dy, mouse_right;      /* window pixels moved with the right button held (the F5 free camera) */
    int raw_dx, raw_dy;                       /* relative mouse counts since the last win_poll (WM_INPUT, only while the window is in the
                                               * foreground), the port's stand-in for DirectInput's DIMOUSESTATE lX / lY (docs/INPUT.md 1.3) */
    int quit, focused;                        /* focused: the window is in front (pads and the mouse only count then) */
    unsigned dev_changes;                     /* counts WM_DEVICECHANGE: a pad may have come or gone (src/pad.c looks again) */
    void *hwnd, *hdc, *hglrc;                 /* Win32: HWND, HDC, HGLRC; SDL: SDL_Window *, -, SDL_GLContext */
    int vx, vy;                               /* port extra (docs/DISPLAY.md 3): rnd_frame draws into the box vx, vy, width, height of the real window */
} Window;

int  win_open(Window *w, const char *title, int width, int height);
/* port extras (docs/DISPLAY.md 3): full = borderless on the window's monitor, else a client of width x height (shrunk to the
 * work area, aspect kept); interval = the swap interval (1 = vsync, 0 = off; returns -1 without WGL_EXT_swap_control) */
void win_mode(Window *w, int width, int height, int full);
int  win_vsync(int interval);
void win_poll(Window *w);                     /* pumps messages, updates keys/mouse */
void win_swap(Window *w);
void win_close(Window *w);
void win_title(Window *w, const char *title);
double win_time(void);                        /* seconds, high resolution */

/* One texture group's share of the world, baked once. `idx` is what the current frame actually draws out of it:
 * the triangles of the faces the visibility pass kept (see rnd_frame). */
struct WorldBatch { uint32_t group; uint32_t ntris; float *pos; float *uv; uint8_t *col;
                    uint32_t *idx, nidx, idx_cap; uint32_t *face; };   /* face: per triangle, only for the light batches */
/* Where a world face ended up in the batches, so the visibility pass can put its triangles back in. */
struct FaceBatch { uint32_t tri0, ntris, group, zone; uint8_t lit, sky; };   /* zone = the .gel section-3 group of the face; sky = a sky-group face (never drawn) */

typedef struct {
    TexFile *tex; GelFile *gel; InsFile *ins;
    /* world geometry baked into vertex arrays per texture group */
    struct WorldBatch *batches; uint32_t nbatches;
    /* static lighting from the .lit (docs/LIGHTING.md): lit faces are drawn in passes (ambient fill, additive light
     * polygons, texture x2), everything else in one pass */
    const LitFile *lit; struct WorldBatch *litb;      /* lit faces per texture group (same count as batches) */
    struct WorldBatch lightb[16]; uint32_t light_tex[16];   /* light polygons per generated radial texture */
    int show_light; float *face_bound;                 /* per world face: centre xyz + radius (cast shadow receivers) */
    uint32_t *face_fan;                                /* per world face: first index in fan_idx of its (nverts - 2) triangles */
    uint32_t *fan_idx;                                 /* the triangle fans of every face as GL_TRIANGLES indices into gel->verts */
    float last_time;
    int have_sky; uint32_t sky_tex[5]; float sky_hu, sky_hv;   /* sky cube (docs/SKY.md): +z, +x, -z, -x, top */
    int show_world, show_instances, wireframe;
    /* visibility (0x42a980 / 0x42ac10): which sectors of the .gel the camera can see, and the faces that go with them */
    const VisFile *vis;                                /* .vis potentially visible sets, NULL when the level has none */
    int cull;                                          /* 0 = draw the whole level, 1 = frustum only, 2 = frustum + .vis */
    struct FaceBatch *face_batch;                      /* per world face */
    uint32_t *face_stamp, stamp_gen;                   /* the frame stamp of poly+4: one face is collected once */
    uint8_t *sec_vis, *sec_prev; int pvs_on, sec_dirty;/* per sector: visible now / last frame */
    uint32_t drawn_tris, total_tris; uint32_t nsec_vis;
    /* race levels (Perso subtype 4/5, 0x401c36): the region list renderer+0xc0 of SetRaceInfo 0x455dc0; with race[0] != -1
     * 0x42a980 draws only the zone (.gel group) of the floor under the camera and the list entry after it (docs/RACE.md 2.1) */
    int32_t race[6]; int race_vis; int32_t race_zone[2], race_prev[2]; const void *race_entry; int race_sky, sky_on;
    Vec3 face_eye; int face_eye_ok;                    /* camera of the last index-list build: 0x42b6c0 culls every world face the camera is not in front of */
    /* drawn after the models and before the fade list: texture list 8 of the flush 0x4293f0 (docs/LIGHTING.md 1.5),
     * where the water surfaces of class 60 go (water.c) */
    void (*post_models)(const TexFile *tex, Vec3 eye);
    uint8_t *model_blend;                              /* per .ins model: 1 = a drawn mesh node has a polygon of a blended group (list +0x1cc), built on the first frame */
    Instance **links; uint32_t nlinks, links_cap;     /* message 34 pairs (volume instance, hidden instance), level+0x50 / +0x4c (docs/INSTANCE.md 10.1) */
    /* the per-frame instance list world+0x60/+0x64 (0x42a980 / 0x42a840, docs/INSTANCE.md 4.1): rebuilt by rnd_instance_list */
    Instance **list; uint32_t nlist, list_cap; int list_on;
    uint8_t *list_sec, *list_grp;                      /* sector stamp +4 / group stamp +0 of this frame: the pairs of the camera's .vis list */
    /* the sector chains sector+0x44 (0x407790 pushes in front) in the order 0x42a840 walks them, the clock frame counter
     * [[0x509adc]] (once per frame per instance, 0x43eeee); the .col object list of every kd leaf (0x4271e0: cell+0x40
     * count, +0x44 refs (mask << 16 | object index)) lives in the GelFile (gel_col_load, gel_col_cell): the type-1 objects
     * of the camera's leaf are clocked first (0x42aa0b), and the collision queries walk the same lists */
    Instance **chain; uint32_t nchain; uint32_t frame; int chain_ok; uint32_t link_done;
    /* called once per frame for every instance that is drawn, after the visibility pass and before any model is drawn:
     * the render-colour vtbl[26] of the class (the lightning rod 0x452010 updates its glow only when drawn) */
    void (*on_drawn)(Instance *inst);
    /* the Perso's instance, set by the game every frame: 0x42b380(World, Perso) draws it first, outside the list, whatever
     * its cell - also while a cinematic plays it (scripted, flag 0x20 cleared by 0x44eab0), when it is in no listed chain */
    Instance *perso;
    /* the world sprites of the frame (lists +0x1c8 alpha blended / +0x1cc additive of 0x481560, recorded by hud.c): rnd_sorted
     * buckets them by the view z of their first clipped corner together with the fade list and the glow faces (0x428d00) */
    int  (*spr_count)(void);
    void (*spr_quad)(int i, float v[4][3], int *blended, int *early);
    void (*spr_draw)(int i);
    void (*spr_done)(void);
} Renderer;

int  rnd_init(Renderer *r, TexFile *tex, GelFile *gel, InsFile *ins, const LitFile *lit, const VisFile *vis);   /* lit / vis may be NULL */
void rnd_frame(Renderer *r, const Window *w, const FreeCamera *cam, float time_s);
/* 0x428d00, the end of the 3D frame: the fade list, the glow faces and the world sprites recorded since rnd_frame, in 256 depth
 * buckets from far to near. Call once after rnd_frame, when the frame's world sprites have been recorded (hud_world_*) */
void rnd_sorted(Renderer *r);
void rnd_fade(float brightness);             /* darken the finished frame: 1 = normal, 0 = black */
void rnd_free(Renderer *r);
/* message 34 [inst, other] (0x42dc21): while the camera is inside one of inst's volume nodes, `other` is not drawn (0x42aa0b) */
void rnd_link(Renderer *r, Instance *inst, Instance *other);
/* the .col file (0x4271e0): per kd leaf cell a count and that many refs (mask << 16 | object index); 0 = loaded. It is
 * loaded into the level's GelFile (gel_col_load), where the collision queries find it too */
int rnd_load_col(Renderer *r, const char *path);
/* 0x42a980 -> 0x42a840: rebuild this frame's instance list (Renderer.list, Instance.listed) from the camera's .vis list. race =
 * the race region list world+0xc0 (Renderer.race, rnd_set_race) while the Perso is a rider (subtypes 4/5, 0x401c36), NULL
 * otherwise. Call once per frame before the Thinks. */
void rnd_instance_list(Renderer *r, const Window *w, const FreeCamera *cam, const int32_t *race);
/* message 1120 SetRaceInfo (0x455f10..0x455fed): the region list from the race polyline; NULL = none (the full .vis path) */
void rnd_set_race(Renderer *r, const Trajectory *path);
/* message 1200 SetTypeInstance (0x403502..0x403e7a): the old object is unlinked (0x407850) and the new class object linked IN FRONT
 * of its sector's chain (0x407790), in the order of the messages; the chains take that over on the next list */
void rnd_note_link(Instance *in);
void rnd_set_sky(Renderer *r, const uint32_t tex[5]);   /* level bank images 3,0,1,2,4 replace the group's own frames when the bank has >= 5 images (0x5e8670) */
/* texture sharpness (PORT EXTRA, docs/DISPLAY.md 5): anisotropic filtering of the level textures, 1 = off (the original);
 * rnd_aniso_max = the most this GL can do (0 = none, needs the GL context) */
void rnd_set_aniso(int n);
int  rnd_aniso_max(void);
int  rnd_screenshot(const Window *w, const char *path);   /* binary PPM of the current back buffer */
/* an HNM film frame (RGB565, docs/HNM.md) over the whole window, 4:3 kept with black bars; px NULL frees the texture */
void rnd_film_frame(const Window *w, const uint16_t *px, int width, int height);
void rnd_uv_report(const Renderer *r, const Instance *inst);   /* WOODY_UVLOG: texture group + generated UV range per mesh node and material */
Vec3 cam_forward(const FreeCamera *c);
Vec3 cam_right(const FreeCamera *c);
/* the projection inlined in 0x47b230: a world point into the 640x480 HUD space. 0 = outside the four side planes
 * (the original then skips the animation that wanted it), 1 = sx/sy filled in. */
/* 0x498790(lightsys, kind, &pos, &rgb 0..255, radius): register a dynamic point light for the next drawn frame.
 * 1 = stored, 0 = table full (16). Drawn only with WOODY_DYNLIGHT=1: the original never draws them (docs/LIGHTING.md 7). */
int  rnd_light_add(int kind, Vec3 pos, const float rgb[3], float radius);
int  rnd_project(const Window *w, const FreeCamera *cam, Vec3 p, float *sx, float *sy);

#endif
