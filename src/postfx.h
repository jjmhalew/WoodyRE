/* postfx.h - edge smoothing of the 3D picture (PORT EXTRA, docs/DISPLAY.md 5): multisampling (MSAA) and SMAA. The original
 * draws without either; both are off by default (woodyre.cfg msaa= / smaa=, the Graphics page, WOODY_MSAA / WOODY_SMAA). */
#ifndef WOODY_POSTFX_H
#define WOODY_POSTFX_H

void postfx_set(int msaa, int smaa);          /* msaa = samples (0, 2, 4, 8), smaa = 0 off, 1..4 low / medium / high / ultra */
int  postfx_msaa_max(void);                   /* the most samples this GL can draw with, 0 = no MSAA (needs the GL context) */
int  postfx_smaa_supported(void);             /* 1 = framebuffer objects + GLSL 1.30 (needs the GL context) */
/* postfx_begin before the 3D picture: with an effect on, the frame is drawn into an own target of w x h (the window) instead
 * of the window. postfx_end after the 3D picture and its sprites, before the 2D layer (HUD, menus, text stay sharp): the
 * target is resolved, smoothed by SMAA when on, and copied into the window. Both do nothing while every effect is off. */
void postfx_begin(int w, int h);
void postfx_end(void);
/* the samples of the target the 3D picture is drawn into right now (0 = single sampled): gtao.c reads its depth */
int  postfx_samples(void);

#endif
