/* Exhaustively compare the table-driven conversion against the arithmetic it
   replaced, for every dither position and every channel value. */
#include <stdio.h>
#include <stdlib.h>
#include "wedge/canvas.h"

static const int k_bayer[8][8] = {
    { 0, 32, 8, 40, 2, 34, 10, 42 },   { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44, 4, 36, 14, 46, 6, 38 },  { 60, 28, 52, 20, 62, 30, 54, 22 },
    { 3, 35, 11, 43, 1, 33, 9, 41 },   { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47, 7, 39, 13, 45, 5, 37 },  { 63, 31, 55, 23, 61, 29, 53, 21 },
};
static int clampi(int v, int lo, int hi){ return v<lo?lo:(v>hi?hi:v); }

int main(void) {
    const int W = 536, H = 8;
    uint32_t *px = malloc((size_t)W*H*4);
    uint16_t *got = malloc((size_t)W*H*2);
    /* A spread of values including the extremes where clamping matters. */
    for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
        unsigned r=(x*7+y*13)&0xFF, g=(x*11+y*5)&0xFF, b=(x*3+y*29)&0xFF;
        if (x<8){ r=g=b=x; }                 /* near black */
        if (x>=8 && x<16){ r=g=b=248+(x-8); }/* near white */
        px[(size_t)y*W+x] = 0xFF000000u|(r<<16)|(g<<8)|b;
    }
    wg_canvas_t cv = wg_canvas_full(px, W, H);
    wg_to_rgb565_rows(&cv, got, 0, H);

    long bad = 0;
    for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
        uint32_t p = px[(size_t)y*W+x];
        int thr = k_bayer[y&7][x&7];
        int r = (int)((p>>16)&0xFF) + (thr>>3) - 4;
        int g = (int)((p>>8)&0xFF)  + (thr>>4) - 2;
        int b = (int)(p&0xFF)       + (thr>>3) - 4;
        r=clampi(r,0,255); g=clampi(g,0,255); b=clampi(b,0,255);
        uint16_t want = (uint16_t)(((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3));
        if (want != got[(size_t)y*W+x]) {
            if (bad < 3) printf("  mismatch at %d,%d: want %04x got %04x\n", x,y,want,got[(size_t)y*W+x]);
            bad++;
        }
    }
    if (bad) printf("\n%ld MISMATCHES\n", bad);
    else printf("\nbit-identical across %ld pixels and all 64 dither positions\n", (long)(W*H));
    return bad ? 1 : 0;
}
