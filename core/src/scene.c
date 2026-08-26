#include "wedge/scene.h"

#include <math.h>
#include <string.h>

/* xorshift, because the scene needs reproducible placement and nothing here
   deserves a dependency on the C library's global generator. */
static uint32_t rnd(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static float rndf(uint32_t *s)
{
    return (float)(rnd(s) >> 8) / 16777216.0f;
}

/* Sun altitude as a smooth function of the hour, peaking at 13:00 and crossing
   zero near 06:00 and 20:00. Everything visual keys off this one curve, which
   is why dusk reads as a single coherent event rather than several independent
   fades that happen to overlap. */
float wg_scene_altitude(float hours)
{
    float a = (hours - 13.0f) * (3.14159265f / 12.0f);
    return cosf(a);
}

/* Palette keyframes, night through day and back. Each row is a four-stop sky
   read top to bottom. Warm at the horizon, dark overhead, always: that is what
   makes it look like air rather than a gradient. */
typedef struct {
    float alt;
    wg_color top;
    wg_color upper;
    wg_color lower;
    wg_color horizon;
} sky_key_t;

static const sky_key_t k_sky[] = {
    /* deep night */
    { -1.00f, WG_RGB(0, 0, 0), WG_RGB(4, 5, 12), WG_RGB(10, 10, 26), WG_RGB(24, 18, 38) },
    /* night */
    { -0.45f, WG_RGB(2, 3, 8), WG_RGB(9, 11, 26), WG_RGB(22, 21, 48), WG_RGB(46, 32, 60) },
    /* civil twilight, the warm band appears low */
    { -0.12f, WG_RGB(8, 10, 26), WG_RGB(30, 26, 58), WG_RGB(76, 44, 74), WG_RGB(148, 74, 66) },
    /* sunrise or sunset */
    { 0.02f, WG_RGB(24, 28, 58), WG_RGB(72, 52, 92), WG_RGB(158, 82, 78), WG_RGB(226, 128, 68) },
    /* golden */
    { 0.22f, WG_RGB(46, 74, 120), WG_RGB(108, 108, 140), WG_RGB(196, 132, 96), WG_RGB(238, 170, 100) },
    /* morning */
    { 0.55f, WG_RGB(52, 96, 148), WG_RGB(96, 138, 176), WG_RGB(156, 174, 186), WG_RGB(198, 190, 176) },
    /* full day */
    { 1.00f, WG_RGB(44, 96, 156), WG_RGB(86, 140, 188), WG_RGB(148, 184, 206), WG_RGB(196, 212, 214) },
};

static void sky_at(float alt, sky_key_t *out)
{
    const int n = (int)(sizeof(k_sky) / sizeof(k_sky[0]));
    int i = 0;
    while (i < n - 2 && alt > k_sky[i + 1].alt) {
        i++;
    }
    float a = k_sky[i].alt;
    float b = k_sky[i + 1].alt;
    float t = (b - a) > 1e-6f ? (alt - a) / (b - a) : 0.0f;
    t = wg_clampf(t, 0.0f, 1.0f);
    /* Smoothstep between keys so the sky has no kink as it passes a keyframe. */
    t = t * t * (3.0f - 2.0f * t);
    out->top = wg_color_mix(k_sky[i].top, k_sky[i + 1].top, t);
    out->upper = wg_color_mix(k_sky[i].upper, k_sky[i + 1].upper, t);
    out->lower = wg_color_mix(k_sky[i].lower, k_sky[i + 1].lower, t);
    out->horizon = wg_color_mix(k_sky[i].horizon, k_sky[i + 1].horizon, t);
}

wg_color wg_scene_ink(float hours)
{
    float alt = wg_scene_altitude(hours);
    /* By day the sky is bright, so type goes near-white with enough weight to
       hold; at night it drops well below white so it does not glare in a dark
       bedroom. Pure white at 3am is the whole reason people unplug these. */
    float night = wg_smooth(0.15f, -0.25f, alt);
    int v = (int)wg_lerpf(248.0f, 132.0f, night);
    int warm = (int)wg_lerpf(248.0f, 156.0f, night);
    return WG_RGB(warm, (int)wg_lerpf(248.0f, 140.0f, night), v > 255 ? 255 : v);
}

/* Position in the synodic cycle: 0 new, 0.25 first quarter, 0.5 full, 0.75
   last quarter. Measured from the new moon of 2000-01-06 18:14 UTC, which is
   good to a few hours over a human lifetime and considerably better than the
   fixed crescent it replaces. */
float wg_moon_phase(int64_t unix_time)
{
    const double synodic = 29.530588853 * 86400.0;
    double turns = ((double)unix_time - 947182440.0) / synodic;
    turns -= floor(turns);
    if (turns < 0.0) {
        turns += 1.0;
    }
    return (float)turns;
}

/* The moon, drawn as coverage rather than as a disc with a second disc painted
   over it. The old approach guessed at the color behind the moon and painted a
   gradient stop over the top, which is wrong everywhere the real background is
   not that exact color: over the glow, over a star, over any other row of the
   gradient. Masking per pixel has nothing to guess at.

   The terminator is the projection of the day/night line onto the disc, which
   is an ellipse whose half-width is cos(2 pi phase) of the disc's half-width at
   that height. */
static void draw_moon(wg_canvas_t *c, float cx, float cy, float r, float phase, float alpha, wg_color lit)
{
    if (alpha <= 0.004f || r <= 0.0f) {
        return;
    }
    const float ct = cosf(6.2831853f * phase);
    const bool waxing = phase < 0.5f;

    int x0 = (int)floorf(cx - r - 1.0f), x1 = (int)ceilf(cx + r + 1.0f);
    int y0 = (int)floorf(cy - r - 1.0f), y1 = (int)ceilf(cy + r + 1.0f);

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = ((float)x + 0.5f - cx) / r;
            float dy = ((float)y + 0.5f - cy) / r;
            float d2 = dx * dx + dy * dy;
            if (d2 > 1.6f) {
                continue;
            }
            /* Limb, antialiased over one pixel. */
            float disc = wg_clampf((1.0f - sqrtf(d2)) * r + 0.5f, 0.0f, 1.0f);
            if (disc <= 0.0f) {
                continue;
            }
            float w = 1.0f - dy * dy;
            w = w > 0.0f ? sqrtf(w) : 0.0f;
            float xt = ct * w;
            float side = waxing ? (dx - xt) : (-xt - dx);
            /* Softened over a pixel and a half: the terminator on the real
               thing is a gradient, and a hard edge here reads as a bite taken
               out of a sticker. */
            float term = wg_clampf(side * r / 1.5f + 0.5f, 0.0f, 1.0f);

            /* Earthshine keeps the unlit limb faintly present, so a thin
               crescent still reads as a sphere rather than a nail clipping. */
            float k = term + (1.0f - term) * 0.09f;
            unsigned a = (unsigned)(255.0f * alpha * disc * k);
            if (a < 3) {
                continue;
            }
            wg_blend(c, x, y, WG_RGBA(WG_R(lit), WG_G(lit), WG_B(lit), a));
        }
    }
}

static float luminance(wg_color c)
{
    return 0.2126f * (float)WG_R(c) + 0.7152f * (float)WG_G(c) + 0.0722f * (float)WG_B(c);
}

float wg_scene_glare(float hours)
{
    sky_key_t k;
    sky_at(wg_scene_altitude(hours), &k);
    /* The brightest of the three bands a card actually covers, not the average:
       legibility is set by the worst row, not the typical one. */
    float m = luminance(k.upper);
    float l = luminance(k.lower);
    float h = luminance(k.horizon);
    if (l > m) {
        m = l;
    }
    if (h > m) {
        m = h;
    }
    return wg_clampf(m / 200.0f, 0.0f, 1.0f);
}

wg_color wg_scene_accent(float hours)
{
    float alt = wg_scene_altitude(hours);
    float night = wg_smooth(0.15f, -0.25f, alt);
    return wg_color_mix(WG_RGB(255, 158, 92), WG_RGB(236, 122, 118), night);
}

void wg_scene_init(wg_scene_t *s, uint32_t seed)
{
    memset(s, 0, sizeof(*s));
    s->rng = seed ? seed : 0x9E3779B9u;
    s->hours = 21.0f;
    s->moon_phase = 0.5f;
    wg_spring_init(&s->recede, 0.0f, 1.0f, 0.5f);

    for (int i = 0; i < WG_PARTICLES; i++) {
        wg_mote_t *m = &s->motes[i];
        m->x = rndf(&s->rng) * WG_W;
        m->y = rndf(&s->rng) * (WG_HORIZON - 10);
        m->vx = 2.0f + rndf(&s->rng) * 6.0f;
        m->vy = -1.5f + rndf(&s->rng) * 3.0f;
        m->r = 0.6f + rndf(&s->rng) * 1.5f;
        m->phase = rndf(&s->rng) * 6.2831853f;
        m->life = rndf(&s->rng);
    }
    for (int i = 0; i < WG_STARS; i++) {
        wg_star_t *st = &s->stars[i];
        st->x = rndf(&s->rng) * WG_W;
        /* Stars thin out toward the horizon, as they do through real air. */
        float u = rndf(&s->rng);
        st->y = u * u * (WG_HORIZON - 18);
        st->mag = 0.25f + rndf(&s->rng) * 0.75f;
        st->twinkle = rndf(&s->rng) * 6.2831853f;
    }

    /* The land is a fixed sum of sines: same silhouette every boot, so the
       object has one face rather than a new one each power cycle. */
    for (int x = 0; x < WG_W; x++) {
        float fx = (float)x;
        float h = (float)WG_HORIZON;
        h += 9.0f * sinf(fx * 0.0062f + 0.7f);
        h += 4.5f * sinf(fx * 0.0143f + 2.1f);
        h += 2.0f * sinf(fx * 0.0311f + 4.3f);
        s->land[x] = h;
    }
}

void wg_scene_step(wg_scene_t *s, float hours, float dt)
{
    s->hours = hours;
    s->t += dt;
    wg_spring_step(&s->recede, dt);

    float alt = wg_scene_altitude(hours);
    /* Air moves less at night, and a bedside object that is busy at 2am is a
       bedside object that gets turned around to face the wall. */
    float activity = wg_lerpf(0.35f, 1.0f, wg_smooth(-0.3f, 0.4f, alt));
    s->wind = 1.0f + 0.4f * sinf(s->t * 0.11f);

    for (int i = 0; i < WG_PARTICLES; i++) {
        wg_mote_t *m = &s->motes[i];
        m->x += m->vx * s->wind * activity * dt;
        m->y += m->vy * activity * dt;
        m->phase += dt * 0.6f;
        if (m->x > WG_W + 4.0f) {
            m->x = -4.0f;
            m->y = rndf(&s->rng) * (WG_HORIZON - 10);
        }
        if (m->y < -4.0f) {
            m->y = (float)WG_HORIZON - 12.0f;
        }
        if (m->y > (float)WG_HORIZON - 4.0f) {
            m->y = 4.0f;
        }
    }
}

/* Soft horizontal cloud banding. Not sprites: three offset sine bands whose
   opacity follows the sky, which costs a few hundred blends and never needs an
   asset pipeline. */
static void draw_clouds(wg_canvas_t *c, const wg_scene_t *s, float alt, float fade)
{
    float day = wg_smooth(-0.1f, 0.45f, alt);
    float a = day * fade;
    if (a <= 0.01f) {
        return;
    }
    const wg_color warm = WG_RGB(255, 236, 214);
    for (int band = 0; band < 3; band++) {
        float base_y = 44.0f + (float)band * 34.0f;
        float speed = 3.0f + (float)band * 2.2f;
        float amp = 5.0f + (float)band * 2.0f;
        float thick = 7.0f + (float)band * 3.0f;
        float alpha = a * (0.16f - 0.03f * (float)band);
        /* The band's whole vertical reach, checked once. Drawn a slice at a
           time this loop runs per slice, and its per-column trigonometry is
           the most expensive thing in the scene to repeat needlessly. */
        float lo = base_y - amp * 1.5f;
        float hi = base_y + amp * 1.5f + thick;
        if (hi < (float)c->y0 || lo >= (float)(c->y0 + c->rows)) {
            continue;
        }
        for (int x = 0; x < WG_W; x++) {
            float fx = (float)x + s->t * speed;
            float y = base_y + amp * sinf(fx * 0.0091f + (float)band) + amp * 0.5f * sinf(fx * 0.0217f);
            float cover = 0.5f + 0.5f * sinf(fx * 0.0043f + (float)band * 2.0f);
            cover = cover * cover;
            float ta = alpha * cover;
            if (ta <= 0.004f) {
                continue;
            }
            for (int j = 0; j < (int)thick; j++) {
                float ay = y + (float)j;
                float edge = 1.0f - fabsf(((float)j / thick) * 2.0f - 1.0f);
                wg_blend(c, x, (int)ay, WG_RGBA(WG_R(warm), WG_G(warm), WG_B(warm), (unsigned)(255.0f * ta * edge)));
            }
        }
    }
}

void wg_scene_draw(wg_canvas_t *c, const wg_scene_t *s)
{
    float alt = wg_scene_altitude(s->hours);
    /* While a message is open the environment steps back rather than being
       replaced, so the message reads as something arriving into the room the
       device was already showing. */
    float fade = 1.0f - 0.55f * wg_clampf(s->recede.value, 0.0f, 1.0f);

    sky_key_t sky;
    sky_at(alt, &sky);

    const wg_stop_t stops[4] = {
        { 0.00f, sky.top },
        { 0.42f, sky.upper },
        { 0.80f, sky.lower },
        { 1.00f, sky.horizon },
    };
    wg_gradient_v(c, 0, WG_HORIZON + 14, stops, 4);

    /* Stars, fading with the last of the blue. */
    float night = wg_smooth(0.05f, -0.30f, alt);
    if (night > 0.01f) {
        for (int i = 0; i < WG_STARS; i++) {
            const wg_star_t *st = &s->stars[i];
            if (st->y < (float)c->y0 - 2.0f || st->y >= (float)(c->y0 + c->rows) + 2.0f) {
                continue;
            }
            float tw = 0.72f + 0.28f * sinf(s->t * 1.6f + st->twinkle);
            float a = night * st->mag * tw * fade;
            if (a <= 0.02f) {
                continue;
            }
            wg_disc(c, st->x, st->y, st->mag < 0.6f ? 0.6f : 0.9f,
                    WG_RGBA(226, 232, 255, (unsigned)(a * 235.0f)));
        }
    }

    /* The sun or moon on an arc, rising left and setting right, at the same
       altitude the palette is keyed to.

       The travel is confined to the right of WG_ARC_X0 because the clock owns
       the lower left. Type in the calm region, motion in the open one: an arc
       that crosses the numerals makes both unreadable twice a day. */
#define WG_ARC_X0 0.55f
#define WG_ARC_X1 0.97f
    /* Both bodies are always positioned and always drawable; only their
       opacities differ. Selecting one with a boolean meant the loser was
       deleted mid-fade, which is what made the moon vanish and the sun appear
       out of nothing within the same minute, twice a day. Independent alphas
       that overlap hand one off to the other with no instant in between. */
    float sun_a = wg_smooth(-0.14f, 0.03f, alt) * fade;
    float moon_a = wg_smooth(0.06f, -0.10f, alt) * fade;

    /* The sun's parameter is already continuous: it saturates before dusk and
       stays there through the night, where it is invisible anyway. */
    float day_frac = wg_clampf((s->hours - 6.0f) / 14.0f, 0.0f, 1.0f);

    /* The moon's was not. It was two expressions meeting at 06:00, and they
       disagreed by a third of the screen, so the moon teleported left at the
       seam. Measuring from the middle of the night and wrapping once keeps it
       a single continuous function across midnight and across dawn. */
    float mh = s->hours - 1.0f;
    if (mh > 12.0f) {
        mh -= 24.0f;
    } else if (mh < -12.0f) {
        mh += 24.0f;
    }
    float night_frac = wg_clampf((mh + 5.0f) / 10.0f, 0.0f, 1.0f);

    if (sun_a > 0.004f) {
        float sx = wg_lerpf(WG_ARC_X0, WG_ARC_X1, day_frac) * WG_W;
        float sy = (float)WG_HORIZON - 18.0f - alt * 118.0f;
        float low = wg_smooth(0.35f, 0.0f, alt);
        wg_color body_col = wg_color_mix(WG_RGB(255, 250, 232), WG_RGB(255, 168, 88), low);
        wg_color halo_col = wg_color_mix(WG_RGB(255, 236, 190), WG_RGB(255, 132, 62), low);
        wg_glow(c, sx, sy, 96.0f,
                WG_RGBA(WG_R(halo_col), WG_G(halo_col), WG_B(halo_col), (unsigned)(sun_a * 118.0f)), 2.4f);
        wg_disc(c, sx, sy, 15.0f,
                WG_RGBA(WG_R(body_col), WG_G(body_col), WG_B(body_col), (unsigned)(sun_a * 255.0f)));
    }

    if (moon_a > 0.004f) {
        float mx = wg_lerpf(WG_ARC_X0, WG_ARC_X1, night_frac) * WG_W;
        float my = (float)WG_HORIZON - 30.0f + alt * 96.0f;
        /* The halo tracks how much of the disc is actually lit. A three-day
           crescent throwing a full moon's glow is the sort of thing nobody can
           name but everybody notices. */
        float illum = (1.0f - cosf(6.2831853f * s->moon_phase)) * 0.5f;
        wg_glow(c, mx, my, 30.0f + 22.0f * illum,
                WG_RGBA(150, 168, 220, (unsigned)(moon_a * 70.0f * (0.35f + 0.65f * illum))), 2.4f);
        draw_moon(c, mx, my, 11.0f, s->moon_phase, moon_a, WG_RGB(236, 238, 246));
    }

    draw_clouds(c, s, alt, fade);

    /* Land. Near black so the bottom of the panel is genuinely off on AMOLED,
       with a thin lit ridge where it meets the sky. */
    /* Opaque, not almost-opaque. At 252 every one of the thirty thousand
       pixels under the ridge was a read-modify-write against PSRAM to let
       three parts in two hundred and fifty five of the sky through, which
       neither survives the panel's own five-bit red nor was ever visible.
       Fully opaque makes it a plain store, and is closer to the intent, which
       was a bottom edge that is genuinely off. */
    wg_fill_under(c, s->land, WG_W, WG_RGBA(3, 4, 8, 255));
    for (int x = 0; x < WG_W; x++) {
        float h = s->land[x];
        wg_color rim = wg_color_mix(sky.horizon, WG_RGB(255, 255, 255), 0.12f);
        wg_blend(c, x, (int)h, WG_RGBA(WG_R(rim), WG_G(rim), WG_B(rim), (unsigned)(70.0f * fade)));
    }

    /* Motes last: they belong in front of the land as well as the sky. */
    wg_color mote = wg_scene_accent(s->hours);
    float mote_a = wg_lerpf(0.5f, 0.22f, wg_smooth(-0.2f, 0.5f, alt)) * fade;
    for (int i = 0; i < WG_PARTICLES; i++) {
        const wg_mote_t *m = &s->motes[i];
        if (m->y < (float)c->y0 - m->r - 2.0f || m->y >= (float)(c->y0 + c->rows) + m->r + 2.0f) {
            continue;
        }
        float pulse = 0.55f + 0.45f * sinf(m->phase);
        unsigned a = (unsigned)(255.0f * mote_a * pulse * m->life);
        if (a < 4) {
            continue;
        }
        wg_disc(c, m->x, m->y, m->r, WG_RGBA(WG_R(mote), WG_G(mote), WG_B(mote), a));
    }
}
