/* gtao.h - ground-truth ambient occlusion (PORT EXTRA, docs/DISPLAY.md 5): a screen-space pass over the opaque 3D image.
 * The original has nothing like it; off by default (woodyre.cfg ao=, the Graphics page, WOODY_AO). */
#ifndef WOODY_GTAO_H
#define WOODY_GTAO_H

void gtao_enable(int on);                     /* the setting; the pass runs only when it is on and the GL can do it */
int  gtao_supported(void);                    /* 1 = GLSL + framebuffer objects found (after the GL context exists), 0 = never runs */
/* darken the current viewport's colour by the occlusion of its depth buffer. zn / zf = the projection's near / far planes,
 * sx / sy = its x / y scale (f / aspect, f). Call after the opaque geometry, before anything blended or additive. */
void gtao_frame(float zn, float zf, float sx, float sy);

#endif
