#ifndef WEDGE_CANVAS_H
#define WEDGE_CANVAS_H

#include "wedge/wedge.h"

/* A canvas is a linear XRGB8888 buffer. On the device it lives in PSRAM; in the
   simulator it is malloc'd. Nothing in core/ allocates it, so the port decides
   where the memory comes from. */
/* A canvas is a window onto the frame, not necessarily the whole of it.
 *
 * w and h are always the full frame, because every piece of layout is written
 * against the real panel and must not know whether it is being drawn in one
 * pass or ten. What px actually covers is the rows [y0, y0 + rows), and every
 * primitive translates into that window and clips to it.
 *
 * A whole-frame canvas is simply the case where y0 is 0 and rows is h. Use
 * wg_canvas_full to build one rather than an initialiser list, so a canvas
 * that covers nothing is impossible to construct by forgetting a field. */
typedef struct {
    uint32_t *px;
    int w;
    int h;
    int y0;
    int rows;
} wg_canvas_t;

static inline wg_canvas_t wg_canvas_full(uint32_t *px, int w, int h)
{
    wg_canvas_t c = { px, w, h, 0, h };
    return c;
}

static inline wg_canvas_t wg_canvas_band(uint32_t *px, int w, int h, int y0, int rows)
{
    wg_canvas_t c = { px, w, h, y0, rows };
    return c;
}

/* True when this row is inside the window px covers. */
static inline int wg_has_row(const wg_canvas_t *c, int y)
{
    return y >= c->y0 && y < c->y0 + c->rows;
}

/* The start of a row, which must be one this canvas actually covers. */
static inline uint32_t *wg_row_at(const wg_canvas_t *c, int y)
{
    return &c->px[(size_t)(y - c->y0) * (size_t)c->w];
}

/* A gradient stop is a color at a normalized vertical position. */
typedef struct {
    float pos;
    wg_color color;
} wg_stop_t;

void wg_clear(wg_canvas_t *c, wg_color color);

/* Source-over blend of a single pixel. Out of bounds is a no-op, which lets the
   particle and glow loops run without clipping every coordinate first. */
void wg_blend(wg_canvas_t *c, int x, int y, wg_color color);

void wg_fill_rect(wg_canvas_t *c, int x, int y, int w, int h, wg_color color);

/* Vertical multi-stop gradient across [y0, y1). Stops must be sorted by pos.
   This is the single most-used call in the firmware: it paints the sky. */
void wg_gradient_v(wg_canvas_t *c, int y0, int y1, const wg_stop_t *stops, int n);

/* Antialiased filled disc. Used for the sun, the moon, and every particle. */
void wg_disc(wg_canvas_t *c, float cx, float cy, float r, wg_color color);

/* Radial falloff centered on (cx, cy) reaching zero at r, additive against what
   is already there. This is how the sun lights the sky around it rather than
   sitting on top of it as a flat sticker. */
void wg_glow(wg_canvas_t *c, float cx, float cy, float r, wg_color color, float falloff);

/* Antialiased line, used for the horizon rule and the message card edge. */
void wg_line(wg_canvas_t *c, float x0, float y0, float x1, float y1, float width, wg_color color);

/* Fill everything below a height function, sampled per column. The land
   silhouette is a sum of sines evaluated through this. */
void wg_fill_under(wg_canvas_t *c, const float *height, int n, wg_color color);

/* Continuous-curvature rounded rectangle, antialiased.

   The corners are a superellipse rather than a circular arc: a circular corner
   meets the straight edge with a sudden jump in curvature, which the eye reads
   as a seam even when it cannot name it. This is the shape every modern Apple
   surface uses, and it is most of why a card looks current or dated. */
void wg_round_rect(wg_canvas_t *c, float x, float y, float w, float h, float r, wg_color color);

/* Same shape filled with a vertical gradient, so a material can be denser at
   the top edge where the light lands. */
void wg_round_rect_grad(wg_canvas_t *c, float x, float y, float w, float h, float r,
                        wg_color top, wg_color bottom);

/* A hairline following the shape's outline, brightest at the top and fading
   toward the bottom. This is the light catching the lip of the material. */
void wg_round_rect_stroke(wg_canvas_t *c, float x, float y, float w, float h, float r,
                          wg_color top, wg_color bottom);

/* Box-blur a region in place, twice, which approximates a gaussian closely
   enough at this scale. Used behind glass so the moon and the land ridge
   diffuse instead of showing through as recognisable shapes. */
/* Box-blur behind a rounded rect, in place, written back only where the shape
   actually covers.

   Blurring a rectangular region under a rounded surface leaves the corners of
   that rectangle showing as a hard-edged blurred square around the card. Reads
   still come from the whole region, which is correct: glass shows what is
   behind its edge. Only the writes are masked. Pass r = 0 for a plain rect. */
void wg_blur_rrect(wg_canvas_t *c, float x, float y, float w, float h, float r, int pad,
                   int radius, int passes);

/* Specular highlight following the outline, lit from a direction.

   A stroke of uniform brightness reads as a border. Real glass catches light
   where its surface turns toward the source and again, weaker, on the edge the
   light leaves through, so the highlight travels around the corners and dies at
   the sides. That travel is most of what makes a surface look like a material
   rather than a shape with an outline. */
void wg_round_rect_specular(wg_canvas_t *c, float x, float y, float w, float h, float r,
                            float lx, float ly, float intensity);

/* Soft shadow outside the shape, so the surface sits above the scene rather
   than being painted onto it. */
void wg_round_rect_shadow(wg_canvas_t *c, float x, float y, float w, float h, float r,
                          float spread, float alpha);

/* Refract the backdrop inside a rounded rect, the way a thick piece of glass
   bends what is behind its edges.

   Coverage alone makes a tinted rectangle. What separates glass from tint is
   that the background is displaced near the rim, pulled outward as the surface
   curves away. The displacement runs along the row, which is where it reads on
   a panel three times wider than it is tall, and it costs one line copy. */
void wg_refract(wg_canvas_t *c, float x, float y, float w, float h, float r, float amount);

/* Multiply the whole canvas toward black. Used to push a parent layer back when
   a message takes over, per the material rules: dim to focus. */
void wg_dim(wg_canvas_t *c, float amount);

/* Convert to the panel's RGB565 with an 8x8 ordered dither. The dither is what
   keeps a 240-row warm gradient from banding into visible steps. */
void wg_to_rgb565(const wg_canvas_t *c, uint16_t *out);

/* The same conversion for one horizontal band, written to the start of out.
   The device needs this because the panel cannot be fed from PSRAM: the frame
   goes across in slices through a small buffer in internal memory. */
void wg_to_rgb565_rows(const wg_canvas_t *c, uint16_t *out, int y0, int rows);

#endif
