#include "wedge/font.h"

#include <string.h>

static const wg_glyph_t *glyph_of(const wg_font_t *f, unsigned char ch)
{
    if (ch < f->first || ch > f->last) {
        return NULL;
    }
    return &f->glyphs[ch - f->first];
}

int wg_text_width(const wg_font_t *f, const char *s)
{
    int pen16 = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        const wg_glyph_t *g = glyph_of(f, *p);
        if (!g) {
            continue;
        }
        pen16 += g->advance + f->tracking;
    }
    /* The trailing tracking is not part of the run's ink, so drop it before
       rounding. Leaving it in shifts every centered line half a space left. */
    if (pen16 > 0) {
        pen16 -= f->tracking;
    }
    return (pen16 + 8) >> 4;
}

static void draw_glyph(wg_canvas_t *c, const wg_font_t *f, const wg_glyph_t *g, int x, int y, wg_color color)
{
    if (g->w == 0 || g->h == 0) {
        return;
    }
    const uint8_t *src = &f->alpha[g->offset];
    unsigned base = WG_A(color);
    int ox = x + g->bx;
    int oy = y + g->by;
    for (int j = 0; j < (int)g->h; j++) {
        int py = oy + j;
        if (py < 0 || py >= c->h) {
            continue;
        }
        const uint8_t *row = &src[(size_t)j * g->w];
        for (int i = 0; i < (int)g->w; i++) {
            unsigned a = row[i];
            if (a == 0) {
                continue;
            }
            wg_blend(c, ox + i, py, WG_RGBA(WG_R(color), WG_G(color), WG_B(color), (a * base) / 255u));
        }
    }
}

int wg_text(wg_canvas_t *c, const wg_font_t *f, int x, int y, wg_align_t align, wg_color color, const char *s)
{
    if (align != WG_ALIGN_LEFT) {
        int w = wg_text_width(f, s);
        x -= (align == WG_ALIGN_CENTER) ? w / 2 : w;
    }
    int pen16 = x << 4;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        const wg_glyph_t *g = glyph_of(f, *p);
        if (!g) {
            continue;
        }
        draw_glyph(c, f, g, (pen16 + 8) >> 4, y, color);
        pen16 += g->advance + f->tracking;
    }
    return (pen16 + 8) >> 4;
}

/* Greedy wrap over spaces. Message bodies are short and author-written, so a
   full line-breaking algorithm would be weight without a visible payoff. */
static int wrap_scan(const wg_font_t *f, const char *s, int w, int max_lines,
                     const char **starts, int *lens)
{
    int lines = 0;
    const char *p = s;
    while (*p && lines < max_lines) {
        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *line = p;
        const char *last_fit = NULL;
        const char *q = p;
        int pen16 = 0;
        while (*q) {
            if (*q == '\n') {
                break;
            }
            const wg_glyph_t *g = glyph_of(f, (unsigned char)*q);
            if (g) {
                pen16 += g->advance + f->tracking;
            }
            if (((pen16 + 8) >> 4) > w) {
                break;
            }
            q++;
            if (*q == ' ' || *q == '\0' || *q == '\n') {
                last_fit = q;
            }
        }
        const char *end;
        if (!*q || *q == '\n') {
            end = q;
        } else if (last_fit) {
            end = last_fit;
        } else {
            /* A single word longer than the line: break it mid-word rather
               than overflow the panel. */
            end = q > line ? q : line + 1;
        }
        starts[lines] = line;
        lens[lines] = (int)(end - line);
        lines++;
        p = end;
        if (*p == '\n') {
            p++;
        }
    }
    return lines;
}

int wg_text_wrap_lines(const wg_font_t *f, int w, int max_lines, const char *s)
{
    const char *starts[16];
    int lens[16];
    if (max_lines > 16) {
        max_lines = 16;
    }
    return wrap_scan(f, s, w, max_lines, starts, lens);
}

int wg_text_wrap(wg_canvas_t *c, const wg_font_t *f, int cx, int y, int w, int line_gap, int max_lines,
                 wg_color color, const char *s)
{
    const char *starts[16];
    int lens[16];
    char buf[128];
    if (max_lines > 16) {
        max_lines = 16;
    }
    int n = wrap_scan(f, s, w, max_lines, starts, lens);
    for (int i = 0; i < n; i++) {
        int len = lens[i];
        if (len > (int)sizeof(buf) - 1) {
            len = (int)sizeof(buf) - 1;
        }
        memcpy(buf, starts[i], (size_t)len);
        buf[len] = '\0';
        wg_text(c, f, cx, y + i * line_gap, WG_ALIGN_CENTER, color, buf);
    }
    return n;
}
