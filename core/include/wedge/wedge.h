#ifndef WEDGE_WEDGE_H
#define WEDGE_WEDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The panel is a 1.91" RM67162 AMOLED driven in landscape. Every layout constant
   in the firmware is expressed against these two numbers. */
#define WG_W 536
#define WG_H 240

/* Composition anchors. The horizon divides sky from land; type rests just above
   it so the clock never floats in the middle of an empty field. */
#define WG_HORIZON 188

/* Colors are 0xAARRGGBB while compositing and are converted to the panel's
   RGB565 only at flush, with an ordered dither. Blending in 565 bands visibly
   on the wide warm gradients this device spends most of its life displaying. */
typedef uint32_t wg_color;

#define WG_RGB(r, g, b) ((wg_color)(0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define WG_RGBA(r, g, b, a) ((wg_color)(((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))

#define WG_A(c) ((uint8_t)(((c) >> 24) & 0xFF))
#define WG_R(c) ((uint8_t)(((c) >> 16) & 0xFF))
#define WG_G(c) ((uint8_t)(((c) >> 8) & 0xFF))
#define WG_B(c) ((uint8_t)((c) & 0xFF))

#define WG_BLACK WG_RGB(0, 0, 0)

static inline float wg_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int wg_clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float wg_lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

/* Smoothstep, used wherever a value crosses a threshold and a hard edge would
   read as a jump: gradient stops, star fade, brightness ramps. */
static inline float wg_smooth(float edge0, float edge1, float x)
{
    float t = wg_clampf((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

wg_color wg_color_mix(wg_color a, wg_color b, float t);

#endif
