/* Tracks the moon alone, restricted to the hours where the sun's alpha is
   identically zero (altitude below -0.14). With only one body on screen the
   brightest pixel cannot change subject, so any jump in it is a jump in the
   moon's own path.
   Expected travel is about 0.4 px per minute, so anything above 5 is a seam. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "wedge/scene.h"

static int scan(const char *label, float h0, float h1, float phase, uint32_t *px)
{
    wg_scene_t s;
    wg_scene_init(&s, 0xC0FFEEu);
    s.moon_phase = phase;
    wg_canvas_t cv = { px, WG_W, WG_H };
    int lx = -1, ly = -1;
    float worst = 0, worst_at = 0;

    for (float h = h0; h <= h1 + 1e-4f; h += 1.0f / 60.0f) {
        float hh = h >= 24.0f ? h - 24.0f : h;
        wg_scene_step(&s, hh, 1.0f / 60.0f);
        wg_scene_draw(&cv, &s);
        int bx = -1, by = -1, bv = -1;
        for (int y = 0; y < WG_HORIZON - 6; y++) {
            for (int x = 0; x < WG_W; x++) {
                uint32_t p = px[(size_t)y * WG_W + x];
                int v = ((p >> 16) & 0xFF) + ((p >> 8) & 0xFF) + (p & 0xFF);
                if (v > bv) { bv = v; bx = x; by = y; }
            }
        }
        if (lx >= 0) {
            float d = sqrtf((float)((bx-lx)*(bx-lx) + (by-ly)*(by-ly)));
            if (d > worst) { worst = d; worst_at = hh; }
        }
        lx = bx; ly = by;
    }
    int ok = worst < 5.0f;
    printf("  %-26s worst move %6.1f px at %02d:%02d   %s\n", label, worst,
           (int)worst_at, (int)((worst_at-(int)worst_at)*60), ok ? "continuous" : "JUMP");
    return ok ? 0 : 1;
}

int main(void)
{
    uint32_t *px = malloc((size_t)WG_W * WG_H * 4);
    int fails = 0;
    fails += scan("evening into midnight",  19.7f, 24.0f, 0.50f, px);
    fails += scan("midnight into dawn",      0.0f,  6.4f, 0.50f, px);
    fails += scan("across the 06:00 seam",   5.0f,  6.4f, 0.50f, px);
    fails += scan("06:00 seam, crescent",    5.0f,  6.4f, 0.84f, px);
    free(px);
    printf(fails ? "\n  %d JUMP(S)\n" : "\n  moon path continuous throughout\n", fails);
    return fails ? 1 : 0;
}
