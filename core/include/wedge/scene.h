#ifndef WEDGE_SCENE_H
#define WEDGE_SCENE_H

#include "wedge/canvas.h"
#include "wedge/spring.h"

#define WG_PARTICLES 44
#define WG_STARS 70

/* The scene is the full-bleed environment behind everything else: sky, stars,
   sun or moon on its real arc, cloud banding, land silhouette, drifting motes.

   It is driven by one continuous input, the hour of the day as a float. Nothing
   in here switches on a mode boundary, so dusk arrives by the sky actually
   changing color over an hour rather than by a scene being swapped at 17:00. */

typedef struct {
    float x, y;
    float vx, vy;
    float r;
    float phase;
    float life;
} wg_mote_t;

typedef struct {
    float x, y;
    float mag;
    float twinkle;
} wg_star_t;

typedef struct {
    float hours;      /* 0..24, continuous */
    float moon_phase; /* 0 new, 0.5 full; set from the date each tick */
    float wind;
    float t;     /* seconds since init, for procedural phase */
    uint32_t rng;
    wg_mote_t motes[WG_PARTICLES];
    wg_star_t stars[WG_STARS];
    float land[WG_W];
    /* Converges to 1 while a message is open so the environment can recede
       without the sky snapping to a different picture. */
    wg_spring_t recede;
} wg_scene_t;

void wg_scene_init(wg_scene_t *s, uint32_t seed);

void wg_scene_step(wg_scene_t *s, float hours, float dt);

void wg_scene_draw(wg_canvas_t *c, const wg_scene_t *s);

/* Sun altitude at the given hour, -1 (deep night) to 1 (noon). Exposed because
   the brightness schedule and the type color both follow the same curve the
   sky does, which is what keeps the composition coherent as the day turns. */
float wg_scene_altitude(float hours);

/* Position in the synodic cycle for a given instant: 0 new, 0.25 first quarter,
   0.5 full, 0.75 last quarter. */
float wg_moon_phase(int64_t unix_time);

/* How bright the sky actually is behind a full-width surface, 0 to 1.

   Altitude is the wrong input for this: at dawn the sun is barely up but the
   horizon band is blazing orange, so a material sized by altitude thins out
   exactly where the backdrop is loudest. This measures the painted stops. */
float wg_scene_glare(float hours);

/* The color type should be to sit legibly on the sky at this hour. */
wg_color wg_scene_ink(float hours);

/* The accent, warm at night and cooler by day. Used by the indicator. */
wg_color wg_scene_accent(float hours);

#endif
