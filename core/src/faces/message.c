#include "faces.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../fonts/fonts.h"

/* Motes that converge as the card arrives. Seeded when the message opens rather
   than animated from a fixed script, so an interrupted open leaves the
   particles wherever they honestly are. */
#define BURST 26

static struct {
    float x, y, r, life;
} s_burst[BURST];

static uint32_t s_seed = 0x1234567u;
static bool s_active;

static float frnd(void)
{
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 17;
    s_seed ^= s_seed << 5;
    return (float)(s_seed >> 8) / 16777216.0f;
}

void wg_face_message_open(wg_app_t *a)
{
    (void)a;
    s_active = true;
    for (int i = 0; i < BURST; i++) {
        float ang = frnd() * 6.2831853f;
        float rad = 90.0f + frnd() * 150.0f;
        s_burst[i].x = WG_W * 0.5f + cosf(ang) * rad;
        s_burst[i].y = WG_H * 0.5f + sinf(ang) * rad * 0.55f;
        s_burst[i].r = 0.8f + frnd() * 1.8f;
        s_burst[i].life = 0.6f + frnd() * 0.4f;
    }
}

static void draw_burst(wg_canvas_t *c, float open, wg_color accent, float tx, float ty)
{
    if (!s_active) {
        return;
    }
    /* They fade out as the card settles: the burst announces the arrival and
       then gets out of the way of the words, which are the point. */
    float a = wg_smooth(0.15f, 0.55f, open) * (1.0f - wg_smooth(0.72f, 1.0f, open));
    if (a <= 0.01f) {
        if (open > 0.99f) {
            s_active = false;
        }
        return;
    }
    for (int i = 0; i < BURST; i++) {
        float t = wg_clampf(open, 0.0f, 1.0f);
        float px = wg_lerpf(s_burst[i].x, tx, t * t);
        float py = wg_lerpf(s_burst[i].y, ty, t * t);
        wg_disc(c, px, py, s_burst[i].r,
                WG_RGBA(WG_R(accent), WG_G(accent), WG_B(accent),
                        (unsigned)(255.0f * a * s_burst[i].life)));
    }
}

void wg_face_message(wg_app_t *a, wg_canvas_t *c)
{
    float open = wg_clampf(a->card.value, 0.0f, 1.2f);
    if (open <= 0.001f) {
        return;
    }
    wg_color accent = wg_scene_accent(a->hours);
    draw_burst(c, open, accent, WG_W * 0.5f, WG_H * 0.5f);

    if (!a->open) {
        return;
    }

    /* The capsule she touched becomes the card, every dimension including the
       corner radius travelling together. This is one object changing size, not
       a card fading in over a capsule fading out, and the difference is what
       makes the gesture and its result read as the same event. */
    float t = wg_clampf(open, 0.0f, 1.0f);
    float shape = wg_smooth(0.0f, 1.0f, t);
    wg_rect_t r = wg_rect_mix(a->card_from, wg_card_rect(), shape);
    unsigned ia = (unsigned)(255.0f * t);

    /* The same material the capsule is made of, at the size it has grown to. */
    wg_glass(c, a, r, t, 0.0f);
    wg_color accent2 = accent;
    (void)accent2;

    /* The material arrives first, then the words. Glyphs cannot be scaled at
       this size without resampling them, so type that appeared at t=0 would be
       full-size text overflowing a capsule. Bringing it in over the back half
       of the travel is both the only way it fits and the right reading: a
       surface settles, then it has something to say. */
    unsigned content_a = (unsigned)(255.0f * wg_smooth(0.75f, 0.98f, t));
    if (content_a == 0) {
        return;
    }

    /* Everything below is laid out against the settled card, never the animated
       one.

       Taking the wrap width from the shrinking rect meant that dismissing a
       message re-ran the line breaker every frame: the count crossed three
       partway down and the body swapped to the smaller optical size in the
       middle of the gesture, so the words appeared to shrink as they left. Text
       does not reflow because a container is moving.

       Holding it still also means the words arrive rather than slide, which is
       why the content only starts at three quarters of the travel: by then the
       card is wider than the block and nothing has to be clipped. */
    const wg_rect_t f = wg_card_rect();
    const int cx = (int)(f.x + f.w * 0.5f);

    const char *kicker = wg_msg_type_kicker(a->open->type);
    wg_text(c, &wg_font_kicker, cx, (int)f.y + 34, WG_ALIGN_CENTER,
            WG_RGBA(WG_R(accent), WG_G(accent), WG_B(accent), (unsigned)(content_a * 0.95f)),
            kicker);

    const int wrap_w = (int)f.w - 74;
    int lines = wg_text_wrap_lines(&wg_font_msg, wrap_w, 4, a->open->text);
    const wg_font_t *body = &wg_font_msg;
    int gap = 38;
    if (lines > 3) {
        /* Long notes drop to the smaller optical size rather than overflowing;
           the face was baked at two sizes for exactly this. The decision is
           made once, from the settled width. */
        body = &wg_font_msg_s;
        gap = 30;
        lines = wg_text_wrap_lines(body, wrap_w, 5, a->open->text);
    }
    const int head = 52;
    int block_h = lines * gap;
    int first = (int)f.y + head + ((int)f.h - head - block_h) / 2 + body->ascent / 2;

    wg_text_wrap(c, body, cx, first, wrap_w, gap, lines > 4 ? 5 : 4,
                 WG_RGBA(247, 246, 245, content_a), a->open->text);

    /* A grabber rather than a sentence, at the foot because that is the
       direction the card leaves in. Words here would be an instruction manual
       on an object trying not to feel like equipment. */
    float handle_a = wg_smooth(0.72f, 0.98f, t) * (a->dragging ? 0.95f : 0.5f);
    wg_round_rect(c, (float)cx - 18.0f, f.y + f.h - 14.0f, 36.0f, 4.0f, 2.0f,
                  WG_RGBA(255, 255, 255, (unsigned)(255.0f * handle_a * 0.5f)));
    (void)ia;
}

void wg_face_boot(wg_app_t *a, wg_canvas_t *c)
{
    /* Boot says as little as possible, and it says it once.

       The fade used to be measured from the current state, which resets on
       every transition. Boot walks four states in about a second, so the mark
       and its caption restarted a half-second ramp three times and never
       finished one: it read as a flicker with the words smearing over each
       other. Uptime is the honest clock here, and it only moves forward. */
    float t = (float)a->ms / 1000.0f;
    float in = wg_smooth(0.0f, 0.65f, t);
    wg_color ink = wg_scene_ink(a->hours);
    wg_color accent = wg_scene_accent(a->hours);

    float breath = 0.5f + 0.5f * sinf(a->scene.t * 1.5f);
    /* Sized to be a deliberate mark rather than a speck on a panel this wide,
       and still small enough that it is the only thing on screen. */
    wg_draw_heart(c, WG_W * 0.5f, WG_H * 0.5f - 6.0f, 26.0f + breath * 2.0f,
                  WG_RGBA(WG_R(accent), WG_G(accent), WG_B(accent), (unsigned)(225.0f * in)));

    /* A device that comes up in a second should say nothing at all. The caption
       exists for the case where something is actually wrong, so it waits until
       the wait is real, then crossfades instead of cutting between strings. */
    float say = wg_smooth(2.4f, 3.0f, t);
    if (say <= 0.01f) {
        return;
    }
    float age = (float)(a->ms - a->status_since) / 1000.0f;
    float swap = wg_smooth(0.0f, 0.28f, age);
    float base = 110.0f * say * in;

    if (swap < 0.999f && a->status_prev[0]) {
        wg_text(c, &wg_font_label, WG_W / 2, WG_H / 2 + 40, WG_ALIGN_CENTER,
                WG_RGBA(WG_R(ink), WG_G(ink), WG_B(ink), (unsigned)(base * (1.0f - swap))),
                a->status_prev);
    }
    wg_text(c, &wg_font_label, WG_W / 2, WG_H / 2 + 40, WG_ALIGN_CENTER,
            WG_RGBA(WG_R(ink), WG_G(ink), WG_B(ink), (unsigned)(base * swap)), a->status);
}

void wg_face_diagnostic(wg_app_t *a, wg_canvas_t *c)
{
    /* Deliberately plain and deliberately hidden. This is the only screen in
       the firmware allowed to look like a computer. */
    wg_dim(c, 0.86f);
    char line[80];
    const int x = 26;
    int y = 36;
    const wg_color ink = WG_RGB(196, 202, 212);
    const wg_color key = WG_RGB(118, 124, 134);

    wg_text(c, &wg_font_kicker, x, y, WG_ALIGN_LEFT, WG_RGB(236, 158, 92), "DIAGNOSTIC");
    y += 26;

    snprintf(line, sizeof(line), "state    %s", wg_state_name(a->state));
    wg_text(c, &wg_font_label, x, y, WG_ALIGN_LEFT, ink, line);
    y += 19;
    snprintf(line, sizeof(line), "mode     %s   %.2fh", wg_mode_name(a->mode), (double)a->hours);
    wg_text(c, &wg_font_label, x, y, WG_ALIGN_LEFT, ink, line);
    y += 19;
    snprintf(line, sizeof(line), "wifi     %s   time %s", a->wifi_up ? "up" : "down",
             a->time_synced ? "synced" : "unsynced");
    wg_text(c, &wg_font_label, x, y, WG_ALIGN_LEFT, ink, line);
    y += 19;
    snprintf(line, sizeof(line), "messages %d cached, %d pending", a->cache.count,
             wg_msg_cache_pending(&a->cache, a->now_unix));
    wg_text(c, &wg_font_label, x, y, WG_ALIGN_LEFT, ink, line);
    y += 19;
    snprintf(line, sizeof(line), "backlight %u/255   frames %u", (unsigned)a->brightness,
             (unsigned)a->frames);
    wg_text(c, &wg_font_label, x, y, WG_ALIGN_LEFT, ink, line);
    y += 19;
    snprintf(line, sizeof(line), "uptime   %llus", (unsigned long long)(a->ms / 1000));
    wg_text(c, &wg_font_label, x, y, WG_ALIGN_LEFT, ink, line);

    wg_text(c, &wg_font_label, WG_W - 26, WG_H - 18, WG_ALIGN_RIGHT, key, "touch to leave");
}
