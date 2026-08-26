#ifndef WEDGE_FONT_H
#define WEDGE_FONT_H

#include "wedge/canvas.h"

typedef struct {
    int16_t advance; /* 1/16 px, so tracking accumulates without drift */
    int16_t bx;      /* inked box left, relative to the pen */
    int16_t by;      /* inked box top, relative to the baseline (negative is up) */
    uint16_t w;
    uint16_t h;
    uint32_t offset; /* into the face's alpha blob */
} wg_glyph_t;

typedef struct {
    const char *name;
    int16_t ascent;
    int16_t descent;
    int16_t line_height;
    uint8_t first;
    uint8_t last;
    int16_t tracking; /* 1/16 px, baked per optical size */
    const wg_glyph_t *glyphs;
    const uint8_t *alpha;
} wg_font_t;

typedef enum {
    WG_ALIGN_LEFT,
    WG_ALIGN_CENTER,
    WG_ALIGN_RIGHT,
} wg_align_t;

/* Width of a run in whole pixels, including baked tracking. */
int wg_text_width(const wg_font_t *f, const char *s);

/* Draw a single line with its baseline at y. Returns the advanced pen x. */
int wg_text(wg_canvas_t *c, const wg_font_t *f, int x, int y, wg_align_t align, wg_color color, const char *s);

/* Word-wrap into at most max_lines of width w, drawing centered on cx with the
   first baseline at y. Returns the number of lines drawn. Message bodies are
   author-supplied and arbitrary, so wrapping happens on the device. */
int wg_text_wrap(wg_canvas_t *c, const wg_font_t *f, int cx, int y, int w, int line_gap, int max_lines,
                 wg_color color, const char *s);

/* Lines a run would occupy at the given width, without drawing. Lets a face
   center a block vertically before it commits to painting it. */
int wg_text_wrap_lines(const wg_font_t *f, int w, int max_lines, const char *s);

#endif
