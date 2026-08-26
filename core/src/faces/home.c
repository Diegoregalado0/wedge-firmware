#include "faces.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../fonts/fonts.h"

/* The implicit heart (x^2 + y^2 - 1)^3 = x^2 y^3.

   Two things about the previous version were wrong and both showed at the sizes
   this is actually drawn at. It shifted the curve's origin by 0.35 instead of
   centring it, so the shape sat off its own bounding box and lost the bottom of
   the cusp. And it took coverage from the raw field value scaled by a constant,
   but that field is a cubic whose gradient is large near the lobes and nearly
   flat at the cusp, so the same constant produced a razor edge in one place and
   several pixels of mush in another. Scaled by the breath animation, the shape
   appeared to change proportions as it pulsed.

   Coverage comes from supersampling now, which is indifferent to how the field
   is shaped. At the sizes used here that is a few thousand evaluations. */
#define HEART_HALF 1.1380f /* the curve's true half-extent */
#define HEART_YOFF 0.1200f /* its bounding box is not centred on the origin */
#define HEART_SS 3         /* samples per axis */

void wg_draw_heart(wg_canvas_t *c, float cx, float cy, float size, wg_color color)
{
    if (size <= 0.0f) {
        return;
    }
    /* size is the full width of the mark, so the scale maps the curve's own
       half-extent onto half of it. */
    const float s = (size * 0.5f) / HEART_HALF;
    const float inv = 1.0f / s;
    const float step = 1.0f / (float)HEART_SS;
    const float weight = 1.0f / (float)(HEART_SS * HEART_SS);

    int x0 = (int)floorf(cx - size * 0.62f) - 1;
    int x1 = (int)ceilf(cx + size * 0.62f) + 1;
    int y0 = (int)floorf(cy - size * 0.62f) - 1;
    int y1 = (int)ceilf(cy + size * 0.62f) + 1;
    unsigned base = WG_A(color);

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float cov = 0.0f;
            for (int sy = 0; sy < HEART_SS; sy++) {
                for (int sx = 0; sx < HEART_SS; sx++) {
                    float fx = (float)x + (0.5f + (float)sx) * step;
                    float fy = (float)y + (0.5f + (float)sy) * step;
                    float u = (fx - cx) * inv;
                    /* Screen y grows downward and the curve's point is at its
                       bottom, so the axis flips and the box offset comes back. */
                    float v = -(fy - cy) * inv + HEART_YOFF;
                    float a = u * u + v * v - 1.0f;
                    if (a * a * a - u * u * v * v * v <= 0.0f) {
                        cov += weight;
                    }
                }
            }
            if (cov <= 0.0f) {
                continue;
            }
            wg_blend(c, x, y, WG_RGBA(WG_R(color), WG_G(color), WG_B(color), (unsigned)(base * cov)));
        }
    }
}

void wg_material(float hours, wg_color *fill_top, wg_color *fill_bottom,
                 wg_color *edge_top, wg_color *edge_bottom)
{
    float day = wg_smooth(0.22f, 0.80f, wg_scene_glare(hours));
    /* The gradient reverses with the sky. At night the panel is dark at the
       foot, so the material can be top-heavy and catch the light. By day the
       brightest band sits low, so the material has to be densest there or the
       last line of a message lands on glare. */
    *fill_top = wg_color_mix(WG_RGBA(255, 255, 255, 44), WG_RGBA(10, 12, 20, 116), day);
    *fill_bottom = wg_color_mix(WG_RGBA(198, 204, 226, 22), WG_RGBA(12, 14, 22, 154), day);
    /* The lip stays bright in both: it is the light catching the edge of the
       material, and light does not stop landing on it at noon. */
    *edge_top = wg_color_mix(WG_RGBA(255, 255, 255, 108), WG_RGBA(255, 255, 255, 92), day);
    *edge_bottom = WG_RGBA(255, 255, 255, 20);
}

void wg_glass(wg_canvas_t *c, const wg_app_t *a, wg_rect_t r, float alpha, float lift)
{
    if (alpha <= 0.004f || r.w <= 0.0f || r.h <= 0.0f) {
        return;
    }

    /* Built in the order the physics happens: the surface casts before it
       refracts, the rim bends what is behind it, the thickness of the material
       diffuses all of it, the material's own colour sits on that, and only then
       does light catch the edges. */
    bool big = r.h > 80.0f;

    /* A small surface sits closer to the ground it is on, so it casts a tighter
       and lighter shadow. The card's shadow on a capsule read as a smudge. */
    wg_round_rect_shadow(c, r.x, r.y, r.w, r.h, r.r, big ? 16.0f : 7.0f,
                         (big ? 0.30f : 0.20f) * alpha);

    if (a->config.glass_blur) {
        wg_refract(c, r.x, r.y, r.w, r.h, r.r, (big ? 5.0f : 2.0f) * alpha);
        /* Masked to the shape. Blurring a rectangle under a rounded surface
           leaves the rectangle's own corners showing as a hard-edged square
           around the card, which is exactly what it looked like. Three passes,
           because a single box leaves a square kernel signature on a smooth
           sky as well.

           One pass while the card is actually moving. The blur is the most
           expensive thing in any frame that has a card in it, and the frames
           it is most expensive in are exactly the ones where the picture is
           sliding and nobody can see a kernel signature anyway. The radius is
           unchanged, so the amount of blur is too: a sliding-window box costs
           the same whatever its radius, and only the pass count is worth
           spending here. */
        bool moving = a->dragging || !wg_spring_settled(&a->card);
        wg_blur_rrect(c, r.x, r.y, r.w, r.h, r.r, 3, big ? 13 : 5, moving ? 1 : 3);
    }

    wg_color ft, fb, et, eb;
    wg_material(a->hours, &ft, &fb, &et, &eb);
    /* A bigger surface reads as a thicker one. The card covers most of the
       panel, so anything still legible through it competes with the words; the
       capsule is small enough to stay light. */
    float k = alpha * (1.0f + lift * 0.28f) * (big ? 1.5f : 1.0f);
    wg_round_rect_grad(c, r.x, r.y, r.w, r.h, r.r,
                       WG_RGBA(WG_R(ft), WG_G(ft), WG_B(ft), (unsigned)(WG_A(ft) * k)),
                       WG_RGBA(WG_R(fb), WG_G(fb), WG_B(fb), (unsigned)(WG_A(fb) * k)));

    /* A single lit edge rather than two strokes. The highlight is computed from
       the outline's own normal against a light above and slightly left, so it
       runs bright across the top, wraps the corners, thins at the sides and
       returns weakly along the bottom where the light leaves. A stroke of
       constant brightness is a border; this is a surface. */
    float e = alpha * (1.0f + lift * 0.45f);
    wg_round_rect_specular(c, r.x, r.y, r.w, r.h, r.r, -0.42f, -1.0f,
                           (float)WG_A(et) / 255.0f * e * 1.25f);

    /* A sheen set in from the lip, the far face of the glass seen through the
       near one. Only on the card: on a capsule the inset outline lands within a
       couple of pixels of the real one and reads as a doubled border rather
       than as thickness. */
    if (big) {
        const float inset = 3.5f;
        wg_round_rect_specular(c, r.x + inset, r.y + inset, r.w - inset * 2.0f,
                               r.h - inset * 2.0f, r.r - inset, -0.42f, -1.0f, 0.16f * e);
    }
    (void)eb;
}

int wg_ambient_width(const char *text)
{
    return wg_text_width(&wg_font_quote, text);
}

int wg_text_over(wg_canvas_t *c, const wg_font_t *f, int x, int y, wg_align_t align,
                 wg_color color, const char *s)
{
    unsigned a = WG_A(color);
    wg_text(c, f, x + 1, y + 1, align, WG_RGBA(0, 0, 0, (unsigned)(a * 0.34f)), s);
    return wg_text(c, f, x, y, align, color, s);
}

const char *wg_ambient_line(const wg_app_t *a)
{
    int n = wg_ambient_count(a);
    if (n <= 0) {
        return "";
    }
    /* One line per day, turning over at local midnight. It used to change with
       the part of the day as well, which meant the sentence moved under her
       while she was in the room; a standing line should be the same thought all
       day and a different one tomorrow. */
    int64_t local = a->now_unix + (int64_t)wg_tz_offset_minutes(&a->config, a->now_unix) * 60;
    int64_t days = local / 86400;
    int64_t idx = days % n;
    if (idx < 0) {
        idx += n;
    }
    return wg_ambient_at(a, (int)idx);
}

void wg_offer_label(const wg_app_t *a, char *buf, size_t n)
{
    int pending = wg_msg_cache_pending(&a->cache, a->now_unix);
    /* Short, because the pill shares its row with the clock and the meridiem
       now sits between them. The heart carries the warmth; the words only
       have to say that there is something and it is new. */
    if (pending > 1) {
        snprintf(buf, n, "%d messages", pending);
    } else if (pending == 1) {
        snprintf(buf, n, "New message");
    } else {
        /* Kept. Named for what it is rather than advertised again. */
        snprintf(buf, n, "Still here");
    }
}

/* Capsule geometry.

   It rides the clock's own baseline band, low and right, so the panel reads as
   two objects resting on a shelf rather than a field with things scattered in
   it. Sitting it higher put it exactly where the sun and moon cross and it
   swallowed the moon whole; the land's ridge varies by fifteen pixels, so the
   band between the arc and the silhouette is narrow and this is it. */
wg_rect_t wg_offer_rect(const wg_app_t *a)
{
    char label[40];
    wg_offer_label(a, label, sizeof(label));
    /* The mark belongs to something new. A kept message keeps the capsule but
       loses the heart, because a heart that stays lit after she has read the
       message teaches her that it does not mean anything is waiting. */
    bool fresh = wg_msg_cache_pending(&a->cache, a->now_unix) > 0;
    const float h = 44.0f;
    const float pad_l = fresh ? 14.0f : 16.0f;
    const float gap = 9.0f;    /* mark to words */
    const float pad_r = fresh ? 16.0f : 16.0f;
    const float mark = fresh ? 20.0f : 0.0f;
    float text_w = (float)wg_text_width(&wg_font_offer, label);
    float w = pad_l + mark + (fresh ? gap : 0.0f) + text_w + pad_r;
    wg_rect_t r;
    r.w = w;
    r.h = h;
    r.x = (float)WG_W - 30.0f - w;
    r.y = 148.0f - h * 0.5f;
    r.r = h * 0.5f;
    return r;
}

wg_rect_t wg_card_rect(void)
{
    wg_rect_t r = { 24.0f, 20.0f, (float)WG_W - 48.0f, (float)WG_H - 40.0f, 26.0f };
    return r;
}

wg_rect_t wg_rect_mix(wg_rect_t a, wg_rect_t b, float t)
{
    wg_rect_t o;
    o.x = wg_lerpf(a.x, b.x, t);
    o.y = wg_lerpf(a.y, b.y, t);
    o.w = wg_lerpf(a.w, b.w, t);
    o.h = wg_lerpf(a.h, b.h, t);
    o.r = wg_lerpf(a.r, b.r, t);
    return o;
}

void wg_face_home(wg_app_t *a, wg_canvas_t *c)
{
    char t[16], d[48];
    wg_app_clock_strings(a, t, sizeof(t), d, sizeof(d));

    wg_color ink = wg_scene_ink(a->hours);
    /* Type recedes as the card comes forward rather than being covered by it.
       The home layer clears well before the card is fully up: a linear
       crossfade leaves both legible at once around the halfway point, and two
       sets of words at half strength reads as a smear. */
    float open = wg_clampf(a->card.value, 0.0f, 1.0f);
    float fade = 1.0f - wg_smooth(0.0f, 0.42f, open);
    unsigned ia = (unsigned)(255.0f * fade);

    if (fade > 0.01f) {
        /* Date above time, the way a lock screen reads: the small line orients
           you, the large one answers the question.

           The block sits in the upper left and stops well short of the land.
           It used to be pushed down against the horizon, where the ridge
           varies by fifteen pixels and cut straight through the date. Type
           belongs in the sky; the silhouette is a floor, not a background. */
        const int x = 24;
        const int date_y = 34;
        const int clock_y = 158;

        wg_text_over(c, &wg_font_date, x + 2, date_y, WG_ALIGN_LEFT,
                     WG_RGBA(WG_R(ink), WG_G(ink), WG_B(ink), (unsigned)(ia * 0.82f)), d);

        wg_text_over(c, &wg_font_clock, x, clock_y, WG_ALIGN_LEFT,
                     WG_RGBA(WG_R(ink), WG_G(ink), WG_B(ink), ia), t);

        /* Sat on the numerals' own baseline rather than raised: a superscript
           meridiem at this size reads as a footnote to the hour instead of
           part of it. Quieter than the digits, because it is the least
           surprising thing on the panel. */
        /* On the numerals' own baseline, not raised: a superscript meridiem at
           this size reads as a footnote to the hour rather than part of it.
           Quieter than the digits, because it is the least surprising thing
           on the panel and never the reason anyone looked. */
        const char *mer = wg_app_meridiem(a);
        if (mer[0]) {
            int tw = wg_text_width(&wg_font_clock, t);
            wg_text_over(c, &wg_font_offer, x + tw + 9, clock_y, WG_ALIGN_LEFT,
                         WG_RGBA(WG_R(ink), WG_G(ink), WG_B(ink), (unsigned)(ia * 0.62f)), mer);
        }
    }

    /* The standing line, set low over the land where the silhouette is near
       black and gives it a ground for free. It is quieter than the clock on
       purpose: it should be found, not announced. */
    if (fade > 0.01f) {
        const int ax = 26;
        const int avail = WG_AMBIENT_WIDTH_PX;
        const char *line = wg_ambient_line(a);

        /* Three steps, and the last one cannot fail: drop to the smaller face,
           then shorten until it fits. Whatever an editor allowed, a line never
           runs off the glass and never collides with the bezel. */
        const wg_font_t *face = &wg_font_quote;
        char buf[WG_AMBIENT_TEXT + 4];
        if (wg_text_width(face, line) > avail) {
            face = &wg_font_label;
        }
        if (wg_text_width(face, line) > avail) {
            snprintf(buf, sizeof(buf), "%s", line);
            int len = (int)strlen(buf);
            while (len > 1) {
                buf[--len] = '\0';
                char probe[WG_AMBIENT_TEXT + 4];
                snprintf(probe, sizeof(probe), "%s...", buf);
                if (wg_text_width(face, probe) <= avail) {
                    break;
                }
            }
            snprintf(buf + len, sizeof(buf) - (size_t)len, "...");
            line = buf;
        }

        wg_text_over(c, face, ax, 222, WG_ALIGN_LEFT,
                     WG_RGBA(WG_R(ink), WG_G(ink), WG_B(ink), (unsigned)(ia * 0.72f)), line);
    }

    /* The offer. It leaves faster than the rest of home, because it is not
       being dismissed: it is becoming the card. */
    float ind = wg_clampf(a->indicator.value, 0.0f, 1.0f);
    float offer_a = ind * (1.0f - wg_smooth(0.0f, 0.22f, open));
    if (offer_a <= 0.01f) {
        return;
    }

    char label[40];
    wg_offer_label(a, label, sizeof(label));
    int pending = wg_msg_cache_pending(&a->cache, a->now_unix);
    bool fresh = pending > 0;

    wg_rect_t p = wg_offer_rect(a);
    wg_color accent = wg_scene_accent(a->hours);

    /* Something new breathes; something already read sits still. The difference
       between an offer and a nag is entirely whether it stops moving. */
    float breath = fresh ? 0.5f + 0.5f * sinf(a->scene.t * 1.15f) : 0.0f;
    float press = wg_clampf(a->press.value, 0.0f, 1.0f);

    if (fresh) {
        wg_glow(c, p.x + p.w * 0.5f, p.y + p.h * 0.5f, p.w * 0.62f,
                WG_RGBA(WG_R(accent), WG_G(accent), WG_B(accent),
                        (unsigned)(44.0f * offer_a * (0.45f + 0.55f * breath))),
                2.2f);
    }

    /* Pressing lifts the material rather than scaling it. The capsule holds
       baked glyphs that cannot be resampled, and text that scales badly is
       worse than text that does not scale at all. */
    wg_glass(c, a, p, offer_a * (fresh ? 1.0f : 0.86f), press);

    float cy = p.y + p.h * 0.5f;
    float text_x = p.x + 16.0f;
    if (fresh) {
        float mark_scale = wg_lerpf(0.94f, 1.04f, breath);
        wg_draw_heart(c, p.x + 14.0f + 10.0f, cy, 20.0f * mark_scale,
                      WG_RGBA(WG_R(accent), WG_G(accent), WG_B(accent), (unsigned)(offer_a * 244.0f)));
        text_x = p.x + 14.0f + 20.0f + 9.0f;
    }

    wg_text(c, &wg_font_offer, (int)text_x, (int)(cy + 6.0f), WG_ALIGN_LEFT,
            WG_RGBA(246, 246, 250, (unsigned)(offer_a * (fresh ? 236.0f : 168.0f))), label);

    /* Offline is stated once, quietly, and only where it changes what she can
       expect. It is not an error and is never allowed to look like one. */
    if (!a->wifi_up && fade > 0.01f) {
        wg_text(c, &wg_font_label, WG_W - 30, 30, WG_ALIGN_RIGHT,
                WG_RGBA(WG_R(ink), WG_G(ink), WG_B(ink), (unsigned)(ia * 0.28f)), "Offline");
    }
}
