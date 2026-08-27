#include "wedge/canvas.h"

#include <math.h>
#include <string.h>

wg_color wg_color_mix(wg_color a, wg_color b, float t)
{
    t = wg_clampf(t, 0.0f, 1.0f);
    int r = (int)(WG_R(a) + (WG_R(b) - WG_R(a)) * t);
    int g = (int)(WG_G(a) + (WG_G(b) - WG_G(a)) * t);
    int bl = (int)(WG_B(a) + (WG_B(b) - WG_B(a)) * t);
    int al = (int)(WG_A(a) + (WG_A(b) - WG_A(a)) * t);
    return WG_RGBA(r, g, bl, al);
}

void wg_clear(wg_canvas_t *c, wg_color color)
{
    uint32_t v = color | 0xFF000000u;
    size_t n = (size_t)c->w * (size_t)c->rows;
    for (size_t i = 0; i < n; i++) {
        c->px[i] = v;
    }
}

/* Divide by 255 without dividing. The compositor does this three times for
   every blended pixel and a frame blends well over a hundred thousand of them,
   so an integer division here is worth several milliseconds a frame on its
   own. Exact for every value this is ever handed. */
static inline unsigned wg_mul255(unsigned x)
{
    x += 128;
    return (x + (x >> 8)) >> 8;
}

/* The blend itself, with the bounds check already done by the caller. Hot
   loops that walk a row in order should use this and keep their own pointer
   rather than paying for the clamp and the row multiply per pixel. */
static inline void wg_blend_at(uint32_t *p, unsigned cr, unsigned cg, unsigned cb, unsigned a)
{
    if (a == 0) {
        return;
    }
    if (a >= 255) {
        *p = 0xFF000000u | (cr << 16) | (cg << 8) | cb;
        return;
    }
    uint32_t d = *p;
    unsigned ia = 255u - a;
    unsigned r = wg_mul255(cr * a + ((d >> 16) & 0xFF) * ia);
    unsigned g = wg_mul255(cg * a + ((d >> 8) & 0xFF) * ia);
    unsigned b = wg_mul255(cb * a + (d & 0xFF) * ia);
    *p = 0xFF000000u | (r << 16) | (g << 8) | b;
}

void wg_blend(wg_canvas_t *c, int x, int y, wg_color color)
{
    if (x < 0 || x >= c->w || !wg_has_row(c, y)) {
        return;
    }
    wg_blend_at(wg_row_at(c, y) + x, WG_R(color), WG_G(color), WG_B(color), WG_A(color));
}

void wg_fill_rect(wg_canvas_t *c, int x, int y, int w, int h, wg_color color)
{
    int x1 = wg_clampi(x + w, 0, c->w);
    int y1 = wg_clampi(y + h, 0, c->h);
    x = wg_clampi(x, 0, c->w);
    y = wg_clampi(y, 0, c->h);
    for (int j = y; j < y1; j++) {
        for (int i = x; i < x1; i++) {
            wg_blend(c, i, j, color);
        }
    }
}

void wg_gradient_v(wg_canvas_t *c, int y0, int y1, const wg_stop_t *stops, int n)
{
    if (n <= 0 || y1 <= y0) {
        return;
    }
    int span = y1 - y0;
    int seg = 0;
    /* Clipped to the window before the loop rather than inside it. Drawn a
       band at a time this runs once per band, and skipping row by row would
       walk the whole ramp ten times over to fill a tenth of it. The segment
       cursor is seeded for the first row actually drawn, since it only ever
       moves forward. */
    int ys = y0 > c->y0 ? y0 : c->y0;
    int ye = y1 < c->y0 + c->rows ? y1 : c->y0 + c->rows;
    if (ys < 0) {
        ys = 0;
    }
    if (ye > c->h) {
        ye = c->h;
    }
    for (int y = ys; y < ye; y++) {
        float t = (float)(y - y0) / (float)span;
        while (seg < n - 2 && t > stops[seg + 1].pos) {
            seg++;
        }
        wg_color col;
        if (n == 1) {
            col = stops[0].color;
        } else {
            float a = stops[seg].pos;
            float b = stops[seg + 1].pos;
            float lt = (b - a) > 1e-6f ? (t - a) / (b - a) : 0.0f;
            col = wg_color_mix(stops[seg].color, stops[seg + 1].color, wg_clampf(lt, 0.0f, 1.0f));
        }
        uint32_t v = col | 0xFF000000u;
        uint32_t *row = wg_row_at(c, y);
        for (int x = 0; x < c->w; x++) {
            row[x] = v;
        }
    }
}

void wg_disc(wg_canvas_t *c, float cx, float cy, float r, wg_color color)
{
    if (r <= 0.0f) {
        return;
    }
    int x0 = (int)floorf(cx - r - 1.0f);
    int x1 = (int)ceilf(cx + r + 1.0f);
    int y0 = (int)floorf(cy - r - 1.0f);
    int y1 = (int)ceilf(cy + r + 1.0f);
    unsigned base = WG_A(color);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float dy = (float)y + 0.5f - cy;
            float d = sqrtf(dx * dx + dy * dy);
            /* One pixel of coverage ramp at the edge is enough antialiasing at
               this pixel density and costs nothing. */
            float cov = wg_clampf(r + 0.5f - d, 0.0f, 1.0f);
            if (cov <= 0.0f) {
                continue;
            }
            wg_blend(c, x, y, WG_RGBA(WG_R(color), WG_G(color), WG_B(color), (unsigned)(base * cov)));
        }
    }
}

void wg_glow(wg_canvas_t *c, float cx, float cy, float r, wg_color color, float falloff)
{
    if (r <= 0.0f) {
        return;
    }
    unsigned base = WG_A(color);
    if (base == 0) {
        return;
    }
    int x0 = wg_clampi((int)(cx - r), 0, c->w);
    int x1 = wg_clampi((int)(cx + r) + 1, 0, c->w);
    int y0 = wg_clampi((int)(cy - r), c->y0, c->y0 + c->rows);
    int y1 = wg_clampi((int)(cy + r) + 1, c->y0, c->y0 + c->rows);

    /* The falloff depends on nothing but distance, so it is tabulated once per
       call instead of evaluated per pixel. This was the single most expensive
       thing in a frame by a wide margin: the sun's halo is ninety-six pixels
       across, which is close to thirty thousand pixels, and each one was
       paying for a square root and a pow that this part has no hardware for.
       The table is indexed by squared distance, which removes the root as
       well, and stores the finished alpha so the inner loop has no arithmetic
       left in it at all. */
    enum { WG_GLOW_LUT = 512 };
    uint8_t lut[WG_GLOW_LUT + 1];
    for (int i = 0; i <= WG_GLOW_LUT; i++) {
        float q = (float)i / (float)WG_GLOW_LUT;
        float k = powf(1.0f - sqrtf(q), falloff);
        lut[i] = (uint8_t)((float)base * wg_clampf(k, 0.0f, 1.0f) + 0.5f);
    }

    const unsigned cr = WG_R(color), cg = WG_G(color), cb = WG_B(color);
    const float inv2 = 1.0f / (r * r);
    for (int y = y0; y < y1; y++) {
        if (!wg_has_row(c, y)) {
            continue;
        }
        float dy = (float)y + 0.5f - cy;
        float dy2 = dy * dy;
        uint32_t *row = wg_row_at(c, y);
        for (int x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float q = (dx * dx + dy2) * inv2;
            if (q >= 1.0f) {
                continue;
            }
            wg_blend_at(row + x, cr, cg, cb, lut[(int)(q * (float)WG_GLOW_LUT)]);
        }
    }
}


void wg_line(wg_canvas_t *c, float x0, float y0, float x1, float y1, float width, wg_color color)
{
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) {
        return;
    }
    int steps = (int)(len * 2.0f) + 1;
    float half = width * 0.5f;
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / (float)steps;
        wg_disc(c, x0 + dx * t, y0 + dy * t, half, color);
    }
}

void wg_fill_under(wg_canvas_t *c, const float *height, int n, wg_color color)
{
    int cols = n < c->w ? n : c->w;
    for (int x = 0; x < cols; x++) {
        float h = height[x];
        int y0 = (int)floorf(h);
        /* Antialias the ridge itself; a jagged silhouette against a gradient is
           the single most visible artifact on this panel. */
        float frac = 1.0f - (h - (float)y0);
        if (y0 >= 0 && y0 < c->h && frac > 0.0f) {
            wg_blend(c, x, y0, WG_RGBA(WG_R(color), WG_G(color), WG_B(color), (unsigned)(WG_A(color) * frac)));
        }
        /* The body of the land is tens of thousands of pixels every frame, and
           they are all the same colour: hoisting the clamp and the row
           multiply out of the loop leaves only the blend itself. */
        int ys = y0 + 1 < 0 ? 0 : y0 + 1;
        if (ys < c->y0) {
            ys = c->y0;
        }
        int ye = c->y0 + c->rows;
        if (ye > c->h) {
            ye = c->h;
        }
        const unsigned cr = WG_R(color), cg = WG_G(color), cb = WG_B(color), ca = WG_A(color);
        if (ys < ye) {
            uint32_t *p = wg_row_at(c, ys) + x;
            for (int y = ys; y < ye; y++, p += c->w) {
                wg_blend_at(p, cr, cg, cb, ca);
            }
        }
    }
}

/* Signed distance to a rounded rectangle, negative inside.

   The corner uses a p-norm rather than the usual 2-norm. At p = 2 the corner is
   a circular arc whose curvature jumps from zero to 1/r the instant it leaves
   the straight edge; the eye reads that discontinuity as a kink. Raising p
   spreads the curvature into the flat, which is the squircle every current
   Apple surface is built from. The powf only runs on true corner pixels, a few
   hundred per card, so it costs nothing measurable. */
/* The same field, and the direction it points, in one evaluation.
 *
 * The specular highlight needs the outward normal, and took it by sampling the
 * field four more times around each pixel. That is a million evaluations a
 * frame on the message card, and it is unnecessary: the gradient of this
 * particular field is available in closed form. For the corner it is the
 * gradient of the fourth-power norm, and along the flat edges it is simply the
 * axis the nearest edge lies on.
 *
 * The result is not normalised here, only pointed the right way; the caller
 * normalises what it needs. */
static float rrect_sd_grad(float px, float py, float cx, float cy, float hw, float hh, float r,
                           float *nx, float *ny)
{
    float dx = px - cx, dy = py - cy;
    float sx = dx < 0.0f ? -1.0f : 1.0f;
    float sy = dy < 0.0f ? -1.0f : 1.0f;
    float qx = (dx < 0.0f ? -dx : dx) - (hw - r);
    float qy = (dy < 0.0f ? -dy : dy) - (hh - r);

    if (qx > 0.0f && qy > 0.0f) {
        float a2 = qx * qx, b2 = qy * qy;
        float sum = a2 * a2 + b2 * b2;
        float root2 = sqrtf(sum);       /* sum^(1/2) */
        float root4 = sqrtf(root2);     /* sum^(1/4) */
        /* d(sum^(1/4))/dqx = qx^3 * sum^(-3/4) */
        float inv = (root2 * root4) > 1e-12f ? 1.0f / (root2 * root4) : 0.0f;
        *nx = sx * qx * a2 * inv;
        *ny = sy * qy * b2 * inv;
        return root4 - r;
    }
    if (qx > qy) {
        *nx = sx;
        *ny = 0.0f;
        return qx - r;
    }
    *nx = 0.0f;
    *ny = sy;
    return qy - r;
}

static float rrect_sd(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    float qx = fabsf(px - cx) - (hw - r);
    float qy = fabsf(py - cy) - (hh - r);
    if (qx > 0.0f && qy > 0.0f) {
        /* A fourth-power norm, not 4.2. The exponent is what gives the corner
           its continuous curvature, and moving it by two tenths shifts the
           outline by well under a pixel at these radii, but it is the
           difference between three library pow calls and four multiplies with
           two hardware square roots. This runs five times per pixel under the
           specular highlight and was, by a wide margin, the most expensive
           thing in any frame containing glass. */
        float a2 = qx * qx, b2 = qy * qy;
        return sqrtf(sqrtf(a2 * a2 + b2 * b2)) - r;
    }
    float m = qx > qy ? qx : qy;
    return m - r;
}

/* Half-width of the shape at a given row, using the same corner exponent as
   rrect_sd so the mask and the fill agree exactly. */
static float rrect_half_at(float py, float cy, float hw, float hh, float r)
{
    float dy = fabsf(py - cy);
    if (dy >= hh) {
        return -1.0f;
    }
    float t = dy - (hh - r);
    if (t <= 0.0f) {
        return hw;
    }
    /* The same fourth-power norm as rrect_sd, for the same reason. */
    float r2 = r * r, t2 = t * t;
    float inner = r2 * r2 - t2 * t2;
    if (inner <= 0.0f) {
        return -1.0f;
    }
    return (hw - r) + sqrtf(sqrtf(inner));
}

static void rrect_shade(wg_canvas_t *c, float x, float y, float w, float h, float r,
                        wg_color top, wg_color bottom, bool gradient)
{
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }
    float hw = w * 0.5f, hh = h * 0.5f;
    float cx = x + hw, cy = y + hh;
    if (r > hw) {
        r = hw;
    }
    if (r > hh) {
        r = hh;
    }
    int x0 = wg_clampi((int)floorf(x) - 1, 0, c->w);
    int x1 = wg_clampi((int)ceilf(x + w) + 1, 0, c->w);
    int y0 = wg_clampi((int)floorf(y) - 1, 0, c->h);
    int y1 = wg_clampi((int)ceilf(y + h) + 1, 0, c->h);

    for (int py = y0; py < y1; py++) {
        float v = h > 1.0f ? ((float)py + 0.5f - y) / h : 0.0f;
        wg_color col = gradient ? wg_color_mix(top, bottom, wg_clampf(v, 0.0f, 1.0f)) : top;
        unsigned base = WG_A(col);
        for (int pxi = x0; pxi < x1; pxi++) {
            float d = rrect_sd((float)pxi + 0.5f, (float)py + 0.5f, cx, cy, hw, hh, r);
            float cov = wg_clampf(0.5f - d, 0.0f, 1.0f);
            if (cov <= 0.0f) {
                continue;
            }
            wg_blend(c, pxi, py, WG_RGBA(WG_R(col), WG_G(col), WG_B(col), (unsigned)(base * cov)));
        }
    }
}

void wg_round_rect(wg_canvas_t *c, float x, float y, float w, float h, float r, wg_color color)
{
    rrect_shade(c, x, y, w, h, r, color, color, false);
}

void wg_round_rect_grad(wg_canvas_t *c, float x, float y, float w, float h, float r,
                        wg_color top, wg_color bottom)
{
    rrect_shade(c, x, y, w, h, r, top, bottom, true);
}

void wg_round_rect_stroke(wg_canvas_t *c, float x, float y, float w, float h, float r,
                          wg_color top, wg_color bottom)
{
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }
    float hw = w * 0.5f, hh = h * 0.5f;
    float cx = x + hw, cy = y + hh;
    if (r > hw) {
        r = hw;
    }
    if (r > hh) {
        r = hh;
    }
    int x0 = wg_clampi((int)floorf(x) - 2, 0, c->w);
    int x1 = wg_clampi((int)ceilf(x + w) + 2, 0, c->w);
    int y0 = wg_clampi((int)floorf(y) - 2, 0, c->h);
    int y1 = wg_clampi((int)ceilf(y + h) + 2, 0, c->h);

    for (int py = y0; py < y1; py++) {
        float v = h > 1.0f ? ((float)py + 0.5f - y) / h : 0.0f;
        wg_color col = wg_color_mix(top, bottom, wg_clampf(v, 0.0f, 1.0f));
        unsigned base = WG_A(col);
        for (int pxi = x0; pxi < x1; pxi++) {
            float d = rrect_sd((float)pxi + 0.5f, (float)py + 0.5f, cx, cy, hw, hh, r);
            /* A one pixel band centred on the outline. */
            float cov = wg_clampf(1.0f - fabsf(d + 0.5f), 0.0f, 1.0f);
            if (cov <= 0.0f) {
                continue;
            }
            wg_blend(c, pxi, py, WG_RGBA(WG_R(col), WG_G(col), WG_B(col), (unsigned)(base * cov)));
        }
    }
}

/* Working set for the blur. Static rather than heap because this runs every
   frame a card is on screen, and an appliance that mallocs 60 times a second
   for a year is an appliance that fragments.

   The ring holds untouched source rows for the vertical pass; the accumulators
   hold one running column sum per channel. */
/* The sliding window makes the blur O(1) in radius, so a wide kernel costs the
   same as a narrow one and only the ring grows. That matters here: the land is
   a hard near-black silhouette, and a small radius left its ridge legible
   through the glass as a dirty band rather than diffusing it. */
#define WG_BLUR_MAX_R 14
static uint32_t s_blur_line[WG_W];

/* Both passes slide a running sum rather than re-adding the window at every
   pixel. Re-summing is O(radius) per pixel, which measured out around eighty
   milliseconds a frame for a full-width card on this chip and would have made
   the dismissal drag stutter. Sliding is O(1). */
void wg_refract(wg_canvas_t *c, float x, float y, float w, float h, float r, float amount)
{
    if (w <= 0.0f || h <= 0.0f || amount <= 0.0f) {
        return;
    }
    float hw = w * 0.5f, hh = h * 0.5f;
    float cx = x + hw, cy = y + hh;
    if (r > hw) {
        r = hw;
    }
    if (r > hh) {
        r = hh;
    }
    /* The band is where the surface is curving; inside it the glass is flat and
       shows the backdrop straight through.

       It has to be a fraction of the smaller half-dimension, not simply the
       corner radius. On a capsule the radius is the half-height, so a band of r
       reached the centre line and displaced the whole body outward instead of
       just the rim: the backdrop was pulled apart through the middle and read
       as a smear rather than an edge. */
    float small = hw < hh ? hw : hh;
    float band = small * 0.45f;
    if (band > r) {
        band = r;
    }
    if (band < 3.0f) {
        band = 3.0f;
    }

    int y0 = wg_clampi((int)floorf(y), 0, c->h);
    int y1 = wg_clampi((int)ceilf(y + h), 0, c->h);
    int x0 = wg_clampi((int)floorf(x) - 2, 0, c->w);
    int x1 = wg_clampi((int)ceilf(x + w) + 2, 0, c->w);
    int span = x1 - x0;
    if (span <= 0) {
        return;
    }

    for (int py = y0; py < y1; py++) {
        if (!wg_has_row(c, py)) {
            continue;
        }
        uint32_t *row = wg_row_at(c, py);
        memcpy(s_blur_line, &row[x0], (size_t)span * 4);
        for (int px = x0; px < x1; px++) {
            float d = rrect_sd((float)px + 0.5f, (float)py + 0.5f, cx, cy, hw, hh, r);
            if (d > 0.0f || d < -band) {
                continue;
            }
            /* Zero at the middle of the glass, strongest at the rim, and the
               sign follows which side of the shape the pixel is on so the
               backdrop is pulled outward rather than sheared one way. */
            float k = 1.0f + d / band;
            k = k * k;
            float dir = ((float)px + 0.5f) < cx ? -1.0f : 1.0f;
            int src = px - x0 + (int)(dir * k * amount);
            src = wg_clampi(src, 0, span - 1);
            row[px] = s_blur_line[src];
        }
    }
}

void wg_round_rect_specular(wg_canvas_t *c, float x, float y, float w, float h, float r,
                            float lx, float ly, float intensity)
{
    if (w <= 0.0f || h <= 0.0f || intensity <= 0.0f) {
        return;
    }
    float hw = w * 0.5f, hh = h * 0.5f;
    float cx = x + hw, cy = y + hh;
    if (r > hw) {
        r = hw;
    }
    if (r > hh) {
        r = hh;
    }
    float ll = sqrtf(lx * lx + ly * ly);
    if (ll < 1e-5f) {
        return;
    }
    lx /= ll;
    ly /= ll;

    int x0 = wg_clampi((int)floorf(x) - 3, 0, c->w);
    int x1 = wg_clampi((int)ceilf(x + w) + 3, 0, c->w);
    int y0 = wg_clampi((int)floorf(y) - 3, 0, c->h);
    int y1 = wg_clampi((int)ceilf(y + h) + 3, 0, c->h);

    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            float fx = (float)px + 0.5f, fy = (float)py + 0.5f;
            float nx, ny;
            float d = rrect_sd_grad(fx, fy, cx, cy, hw, hh, r, &nx, &ny);
            /* A band straddling the outline, biased inward: the lip belongs to
               the surface, not to the air beside it. */
            if (d > 0.9f || d < -2.6f) {
                continue;
            }
            float nl = sqrtf(nx * nx + ny * ny);
            if (nl < 1e-5f) {
                continue;
            }
            nx /= nl;
            ny /= nl;

            float facing = nx * lx + ny * ly;
            /* The near lip, tight and bright, and the far lip the light leaves
               through, broader and dimmer. */
            float key = facing > 0.0f ? facing * facing * facing : 0.0f;
            float back = facing < 0.0f ? (-facing) * (-facing) * 0.30f : 0.0f;
            float lit = key + back;
            if (lit <= 0.004f) {
                continue;
            }
            /* Across the band, brightest right on the outline. */
            float prof = 1.0f - wg_clampf(fabsf(d + 0.7f) / 1.9f, 0.0f, 1.0f);
            prof = prof * prof;
            unsigned a = (unsigned)(255.0f * intensity * lit * prof);
            if (a < 2) {
                continue;
            }
            wg_blend(c, px, py, WG_RGBA(255, 255, 255, a));
        }
    }
}

void wg_round_rect_shadow(wg_canvas_t *c, float x, float y, float w, float h, float r,
                          float spread, float alpha)
{
    if (w <= 0.0f || h <= 0.0f || alpha <= 0.0f || spread <= 0.0f) {
        return;
    }
    float hw = w * 0.5f, hh = h * 0.5f;
    float cx = x + hw, cy = y + hh;
    /* Offset downward: one light source, above. */
    cy += spread * 0.35f;
    if (r > hw) {
        r = hw;
    }
    if (r > hh) {
        r = hh;
    }
    int x0 = wg_clampi((int)floorf(x - spread), 0, c->w);
    int x1 = wg_clampi((int)ceilf(x + w + spread), 0, c->w);
    int y0 = wg_clampi((int)floorf(y - spread), 0, c->h);
    int y1 = wg_clampi((int)ceilf(y + h + spread * 2.0f), 0, c->h);

    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            float d = rrect_sd((float)px + 0.5f, (float)py + 0.5f, cx, cy, hw, hh, r);
            if (d <= 0.0f || d > spread) {
                continue;
            }
            float k = 1.0f - d / spread;
            k = k * k * k;
            unsigned a = (unsigned)(255.0f * alpha * k);
            if (a < 2) {
                continue;
            }
            wg_blend(c, px, py, WG_RGBA(0, 0, 0, a));
        }
    }
}

/* Separable blur whose writes are confined to a rounded rect.

   Each pass copies the source span first, so the running sum always consumes
   originals, and reads deliberately extend past the shape: what sits behind the
   rim is exactly what a real edge would show. */
/* The backdrop blur, done at a quarter of the panel's resolution.
 *
 * A box blur costs the same per pixel whatever its radius, so the price is set
 * by how many pixels it touches and how many times. Blurring the card's
 * backdrop where it lies meant six full-resolution traversals of a hundred
 * thousand pixels, which measured at a quarter of a second a frame on the
 * device: by far the most expensive thing in any frame with glass in it, and
 * unavoidably so in exactly the frames where something is moving.
 *
 * Blur is low-frequency by definition, so it does not need to be computed at
 * full resolution to look right. The backdrop is reduced by four in each axis,
 * blurred there at a quarter of the radius, and interpolated back on the way
 * out. That is sixteen times fewer pixels through the expensive part, and the
 * result is indistinguishable because everything the downsample discarded was
 * about to be blurred away.
 *
 * It also costs less memory than it replaces: the old ring buffer held
 * twenty-nine full-width rows.
 */
#define WG_BLUR_SHIFT 2
#define WG_BLUR_SCALE (1 << WG_BLUR_SHIFT)
#define WG_SMALL_W (WG_W / WG_BLUR_SCALE + 2)
#define WG_SMALL_H (WG_H / WG_BLUR_SCALE + 2)
#define WG_SMALL_RING (2 * (WG_BLUR_MAX_R / WG_BLUR_SCALE + 1) + 1)

static uint32_t s_small[WG_SMALL_W * WG_SMALL_H];
static uint32_t s_small_line[WG_SMALL_W];
static uint32_t s_small_ring[WG_SMALL_RING * WG_SMALL_W];
static uint32_t s_sacc_r[WG_SMALL_W], s_sacc_g[WG_SMALL_W], s_sacc_b[WG_SMALL_W];
static uint32_t s_up_row[WG_SMALL_W];

static inline uint32_t mix_px(uint32_t a, uint32_t b, unsigned t)
{
    unsigned it = 256u - t;
    unsigned r = (((a >> 16) & 0xFF) * it + ((b >> 16) & 0xFF) * t) >> 8;
    unsigned g = (((a >> 8) & 0xFF) * it + ((b >> 8) & 0xFF) * t) >> 8;
    unsigned bl = ((a & 0xFF) * it + (b & 0xFF) * t) >> 8;
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

/* Separable box blur over the reduced image, in place. */
static void blur_small(int sw, int sh, int radius)
{
    if (radius < 1) {
        radius = 1;
    }
    const unsigned n = (unsigned)(2 * radius + 1);

    for (int y = 0; y < sh; y++) {
        uint32_t *row = &s_small[(size_t)y * WG_SMALL_W];
        memcpy(s_small_line, row, (size_t)sw * 4);
        unsigned sr = 0, sg = 0, sb = 0;
        for (int k = -radius; k <= radius; k++) {
            uint32_t q = s_small_line[wg_clampi(k, 0, sw - 1)];
            sr += (q >> 16) & 0xFF;
            sg += (q >> 8) & 0xFF;
            sb += q & 0xFF;
        }
        for (int x = 0; x < sw; x++) {
            row[x] = 0xFF000000u | ((sr / n) << 16) | ((sg / n) << 8) | (sb / n);
            uint32_t out = s_small_line[wg_clampi(x - radius, 0, sw - 1)];
            uint32_t in = s_small_line[wg_clampi(x + radius + 1, 0, sw - 1)];
            sr += ((in >> 16) & 0xFF) - ((out >> 16) & 0xFF);
            sg += ((in >> 8) & 0xFF) - ((out >> 8) & 0xFF);
            sb += (in & 0xFF) - (out & 0xFF);
        }
    }

    const int span = 2 * radius + 1;
    memset(s_sacc_r, 0, (size_t)sw * sizeof(uint32_t));
    memset(s_sacc_g, 0, (size_t)sw * sizeof(uint32_t));
    memset(s_sacc_b, 0, (size_t)sw * sizeof(uint32_t));
    for (int k = 0; k < span; k++) {
        int sy = wg_clampi(k - radius, 0, sh - 1);
        uint32_t *slot = &s_small_ring[(size_t)k * WG_SMALL_W];
        memcpy(slot, &s_small[(size_t)sy * WG_SMALL_W], (size_t)sw * 4);
        for (int i = 0; i < sw; i++) {
            uint32_t q = slot[i];
            s_sacc_r[i] += (q >> 16) & 0xFF;
            s_sacc_g[i] += (q >> 8) & 0xFF;
            s_sacc_b[i] += q & 0xFF;
        }
    }
    for (int y = 0; y < sh; y++) {
        uint32_t *row = &s_small[(size_t)y * WG_SMALL_W];
        for (int i = 0; i < sw; i++) {
            row[i] = 0xFF000000u | ((s_sacc_r[i] / n) << 16) | ((s_sacc_g[i] / n) << 8) |
                     (s_sacc_b[i] / n);
        }
        uint32_t *slot = &s_small_ring[(size_t)(y % span) * WG_SMALL_W];
        int fetch = wg_clampi(y + radius + 1, 0, sh - 1);
        for (int i = 0; i < sw; i++) {
            uint32_t o = slot[i];
            s_sacc_r[i] -= (o >> 16) & 0xFF;
            s_sacc_g[i] -= (o >> 8) & 0xFF;
            s_sacc_b[i] -= o & 0xFF;
        }
        memcpy(slot, &s_small[(size_t)wg_clampi(fetch, 0, sh - 1) * WG_SMALL_W], (size_t)sw * 4);
        for (int i = 0; i < sw; i++) {
            uint32_t q = slot[i];
            s_sacc_r[i] += (q >> 16) & 0xFF;
            s_sacc_g[i] += (q >> 8) & 0xFF;
            s_sacc_b[i] += q & 0xFF;
        }
    }
}

void wg_blur_rrect(wg_canvas_t *c, float x, float y, float w, float h, float r, int pad,
                   int radius, int passes)
{
    if (w <= 0.0f || h <= 0.0f || radius < 1) {
        return;
    }
    if (radius > WG_BLUR_MAX_R) {
        radius = WG_BLUR_MAX_R;
    }
    if (passes < 1) {
        passes = 1;
    }

    float hw = w * 0.5f, hh = h * 0.5f;
    float cx = x + hw, cy = y + hh;
    if (r > hw) {
        r = hw;
    }
    if (r > hh) {
        r = hh;
    }

    int rx0 = wg_clampi((int)floorf(x) - pad, 0, c->w);
    int rx1 = wg_clampi((int)ceilf(x + w) + pad, 0, c->w);
    int ry0 = wg_clampi((int)floorf(y) - pad, 0, c->h);
    int ry1 = wg_clampi((int)ceilf(y + h) + pad, 0, c->h);
    int rw = rx1 - rx0, rh = ry1 - ry0;
    if (rw <= 0 || rh <= 0) {
        return;
    }

    /* Reduced, rounding up so the last source pixels are covered. */
    int sw = (rw + WG_BLUR_SCALE - 1) / WG_BLUR_SCALE;
    int sh = (rh + WG_BLUR_SCALE - 1) / WG_BLUR_SCALE;
    if (sw < 2) sw = 2;
    if (sh < 2) sh = 2;
    if (sw > WG_SMALL_W) sw = WG_SMALL_W;
    if (sh > WG_SMALL_H) sh = WG_SMALL_H;

    /* Down. Two samples per axis rather than the full sixteen: the blur that
       follows removes what the extra reads would have preserved. */
    for (int sy = 0; sy < sh; sy++) {
        int y0 = ry0 + sy * WG_BLUR_SCALE;
        int y1 = y0 + 2 < ry1 ? y0 + 2 : y0;
        if (y0 >= ry1) {
            y0 = ry1 - 1;
            y1 = y0;
        }
        if (!wg_has_row(c, y0) || !wg_has_row(c, y1)) {
            continue;
        }
        const uint32_t *r0 = wg_row_at(c, y0);
        const uint32_t *r1 = wg_row_at(c, y1);
        uint32_t *out = &s_small[(size_t)sy * WG_SMALL_W];
        for (int sx = 0; sx < sw; sx++) {
            int x0 = rx0 + sx * WG_BLUR_SCALE;
            int x1 = x0 + 2 < rx1 ? x0 + 2 : x0;
            if (x0 >= rx1) {
                x0 = rx1 - 1;
                x1 = x0;
            }
            uint32_t a = r0[x0], b = r0[x1], d = r1[x0], e = r1[x1];
            unsigned rr = (((a >> 16) & 0xFF) + ((b >> 16) & 0xFF) + ((d >> 16) & 0xFF) + ((e >> 16) & 0xFF)) >> 2;
            unsigned gg = (((a >> 8) & 0xFF) + ((b >> 8) & 0xFF) + ((d >> 8) & 0xFF) + ((e >> 8) & 0xFF)) >> 2;
            unsigned bb = ((a & 0xFF) + (b & 0xFF) + (d & 0xFF) + (e & 0xFF)) >> 2;
            out[sx] = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
        }
    }

    for (int p = 0; p < passes; p++) {
        int rs = radius >> WG_BLUR_SHIFT;
        if (p > 0) {
            rs = (rs * 3) / 4 + 1;
        }
        blur_small(sw, sh, rs);
    }

    /* Up, masked to the shape. The vertical blend is done once per output row
       into a reduced-width line, so the per-pixel cost along the row is a
       single interpolation. */
    for (int py = ry0; py < ry1; py++) {
        float half = rrect_half_at((float)py + 0.5f, cy, hw, hh, r);
        if (half < 0.0f || !wg_has_row(c, py)) {
            continue;
        }
        int ia = wg_clampi((int)floorf(cx - half), rx0, rx1);
        int ib = wg_clampi((int)ceilf(cx + half), rx0, rx1);
        if (ib <= ia) {
            continue;
        }

        float v = ((float)(py - ry0) + 0.5f) / (float)WG_BLUR_SCALE - 0.5f;
        int j0 = (int)floorf(v);
        unsigned tv = (unsigned)((v - (float)j0) * 256.0f);
        int j1 = j0 + 1;
        j0 = wg_clampi(j0, 0, sh - 1);
        j1 = wg_clampi(j1, 0, sh - 1);
        const uint32_t *ra = &s_small[(size_t)j0 * WG_SMALL_W];
        const uint32_t *rb = &s_small[(size_t)j1 * WG_SMALL_W];
        for (int i = 0; i < sw; i++) {
            s_up_row[i] = mix_px(ra[i], rb[i], tv);
        }

        uint32_t *row = wg_row_at(c, py);
        for (int px = ia; px < ib; px++) {
            float u = ((float)(px - rx0) + 0.5f) / (float)WG_BLUR_SCALE - 0.5f;
            int i0 = (int)floorf(u);
            unsigned tu = (unsigned)((u - (float)i0) * 256.0f);
            int i1 = i0 + 1;
            i0 = wg_clampi(i0, 0, sw - 1);
            i1 = wg_clampi(i1, 0, sw - 1);
            row[px] = mix_px(s_up_row[i0], s_up_row[i1], tu);
        }
    }
}

void wg_dim(wg_canvas_t *c, float amount)
{
    float k = wg_clampf(1.0f - amount, 0.0f, 1.0f);
    size_t n = (size_t)c->w * (size_t)c->rows;
    for (size_t i = 0; i < n; i++) {
        uint32_t d = c->px[i];
        unsigned r = (unsigned)(((d >> 16) & 0xFF) * k);
        unsigned g = (unsigned)(((d >> 8) & 0xFF) * k);
        unsigned b = (unsigned)((d & 0xFF) * k);
        c->px[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

/* Ordered dither. Without it the sky's slow vertical ramps quantize into bands
   that are obvious on an AMOLED in a dark room. */
static const uint8_t k_bayer[8][8] = {
    { 0, 32, 8, 40, 2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44, 4, 36, 14, 46, 6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    { 3, 35, 11, 43, 1, 33, 9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47, 7, 39, 13, 45, 5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 },
};

void wg_to_rgb565_rows(const wg_canvas_t *c, uint16_t *out, int y0, int rows)
{
    const int w = c->w;
    for (int y = y0; y < y0 + rows; y++) {
        const uint32_t *row = wg_row_at(c, y);
        /* Indexed from the band's own start, but dithered by absolute y: the
           Bayer threshold has to keep its phase across a band boundary or the
           seams become visible lines in a gradient. */
        uint16_t *orow = &out[(size_t)(y - y0) * w];
        /* The dither offsets repeat every eight columns, so they are worked out
           once per row rather than re-derived from the matrix per pixel. Red
           and blue quantize to 5 bits, green to 6, so the amplitude differs
           per channel. */
        int drb[8], dg[8];
        for (int i = 0; i < 8; i++) {
            int thr = k_bayer[y & 7][i];
            drb[i] = (thr >> 3) - 4;
            dg[i] = (thr >> 4) - 2;
        }
        for (int x = 0; x < w; x++) {
            uint32_t p = row[x];
            int k = x & 7;
            int r = (int)((p >> 16) & 0xFF) + drb[k];
            int g = (int)((p >> 8) & 0xFF) + dg[k];
            int b = (int)(p & 0xFF) + drb[k];
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);
            orow[x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    }
}

void wg_to_rgb565(const wg_canvas_t *c, uint16_t *out)
{
    wg_to_rgb565_rows(c, out, 0, c->h);
}
