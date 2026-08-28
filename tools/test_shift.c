/* Burn-in pixel shifting.
 *
 * Two things are checked. That the walk behaves: covers the lattice evenly,
 * never jumps more than a pixel, stays in bounds and repeats. And that it
 * actually does something, by measuring how long the panel's worst pixel
 * spends lit under the clock with and without it, which is the closest thing
 * to evidence available for an effect that takes months to appear. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wedge/canvas.h"

static int fails;
static void ok(const char *what, int cond, const char *extra)
{
    printf("  %s %-52s %s\n", cond ? "ok  " : "FAIL", what, extra ? extra : "");
    if (!cond) fails++;
}

int main(void)
{
    const int span = 2 * WG_SHIFT_MAX + 1;
    const int cells = span * span;

    /* Every position of the lattice, and no others. */
    int seen[64] = { 0 };
    int in_bounds = 1;
    for (int m = 0; m < WG_SHIFT_PERIOD; m++) {
        int dx, dy;
        wg_pixel_shift(m, &dx, &dy);
        if (dx < -WG_SHIFT_MAX || dx > WG_SHIFT_MAX || dy < -WG_SHIFT_MAX || dy > WG_SHIFT_MAX) {
            in_bounds = 0;
        }
        seen[(dy + WG_SHIFT_MAX) * span + (dx + WG_SHIFT_MAX)]++;
    }
    ok("stays inside the lattice", in_bounds, NULL);

    int missing = 0, lo = 1 << 30, hi = 0;
    for (int i = 0; i < cells; i++) {
        if (!seen[i]) missing++;
        if (seen[i] < lo) lo = seen[i];
        if (seen[i] > hi) hi = seen[i];
    }
    char buf[64];
    ok("visits every position", missing == 0, NULL);
    snprintf(buf, sizeof buf, "each %d to %d times per cycle", lo, hi);
    ok("spends the same time at each", hi - lo <= 1, buf);

    /* No step larger than a pixel, including across the wrap. */
    int worst = 0;
    for (int m = 0; m < WG_SHIFT_PERIOD * 3; m++) {
        int ax, ay, bx, by;
        wg_pixel_shift(m, &ax, &ay);
        wg_pixel_shift(m + 1, &bx, &by);
        int d = abs(bx - ax) + abs(by - ay);
        if (d > worst) worst = d;
    }
    snprintf(buf, sizeof buf, "largest step %d px", worst);
    ok("never moves more than a pixel at a time", worst <= 1, buf);

    /* Deterministic, and the same on either side of the wrap. */
    int a1, b1, a2, b2;
    wg_pixel_shift(7, &a1, &b1);
    wg_pixel_shift(7 + WG_SHIFT_PERIOD, &a2, &b2);
    ok("repeats exactly", a1 == a2 && b1 == b2, NULL);
    wg_pixel_shift(-1, &a1, &b1);
    ok("handles a clock before the epoch", a1 >= -WG_SHIFT_MAX && a1 <= WG_SHIFT_MAX, NULL);

    /* The conversion moves the picture and holds the edges. */
    const int W = 64, H = 16;
    uint32_t *px = malloc((size_t)W * H * 4);
    uint16_t *out = malloc((size_t)W * H * 2);
    uint16_t *ref = malloc((size_t)W * H * 2);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            px[y * W + x] = 0xFF000000u | ((unsigned)(x * 4) << 16) | ((unsigned)(y * 16) << 8) | 0x40;
    wg_canvas_t cv = wg_canvas_full(px, W, H);

    wg_to_rgb565_rows(&cv, ref, 0, H);
    wg_to_rgb565_rows_shifted(&cv, out, 0, H, 0, 0);
    ok("no displacement matches the plain conversion", memcmp(ref, out, (size_t)W * H * 2) == 0, NULL);

    wg_to_rgb565_rows_shifted(&cv, out, 0, H, 2, 3);
    int moved = 1, edged = 1;
    for (int y = 3; y < H; y++)
        for (int x = 2; x < W; x++)
            if (out[y * W + x] != ref[(y - 3) * W + (x - 2)]) moved = 0;
    ok("the picture is displaced by exactly that much", moved, "dx 2, dy 3");
    /* The uncovered edge repeats the nearest real pixel. */
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 2; x++)
            if (out[y * W + x] != ref[0]) edged = 0;
    ok("the uncovered edge repeats, rather than sampling nothing", edged, NULL);

    /* What it buys. A stroke as wide as the clock's colon, held at one place
       for a day, against the same stroke walked around the lattice. */
    const int STROKE = 11, DAY = 60 * 24;
    int still[64] = { 0 }, walked[64] = { 0 };
    for (int m = 0; m < DAY; m++) {
        int dx, dy;
        wg_pixel_shift(m, &dx, &dy);
        for (int i = 0; i < STROKE; i++) {
            still[20 + i]++;
            walked[20 + i + dx]++;
        }
    }
    int peak_still = 0, peak_walk = 0, full_still = 0, full_walk = 0;
    for (int i = 0; i < 64; i++) {
        if (still[i] > peak_still) peak_still = still[i];
        if (walked[i] > peak_walk) peak_walk = walked[i];
        if (still[i] >= DAY) full_still++;
        if (walked[i] >= DAY) full_walk++;
    }
    printf("\n  a %d px stroke, the width of the clock's colon, over a day:\n", STROKE);
    printf("    unshifted   %d columns lit every minute of it\n", full_still);
    printf("    shifted     %d columns lit every minute of it\n", full_walk);
    printf("    peak stays %d of %d minutes either way\n", peak_walk, DAY);
    ok("the always-lit core narrows", full_walk < full_still, "11 columns down to 5");
    /* Stated as a check rather than left as a footnote, because it is the
       limit of what this can do and it should fail loudly if someone later
       believes otherwise: a stroke wider than twice the displacement keeps a
       core that is lit at every offset, and no amount of walking changes that.
       Reducing the peak is a matter of brightness and off-time. */
    ok("but the peak is unchanged, which is the limit of this",
       peak_walk == peak_still, "shifting spreads edges, it does not dim");

    /* A stroke no wider than the walk loses its core entirely, which is why
       this works better on fine detail than on heavy type. */
    const int THIN = 5;
    int thin_walk[64] = { 0 };
    for (int m = 0; m < DAY; m++) {
        int dx, dy;
        wg_pixel_shift(m, &dx, &dy);
        for (int i = 0; i < THIN; i++) thin_walk[20 + i + dx]++;
    }
    int thin_full = 0;
    for (int i = 0; i < 64; i++) {
        if (thin_walk[i] >= DAY) thin_full++;
    }
    printf("\n  a %d px stroke, no wider than the walk:\n", THIN);
    printf("    shifted     %d columns lit every minute\n", thin_full);
    ok("a stroke within the walk's reach is never continuously lit", thin_full == 0, NULL);

    printf(fails ? "\n%d FAILED\n" : "\nall pixel shift checks passed\n", fails);
    return fails ? 1 : 0;
}
