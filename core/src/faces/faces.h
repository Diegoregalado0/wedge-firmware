#ifndef WEDGE_FACES_H
#define WEDGE_FACES_H

#include "wedge/app.h"
#include "wedge/font.h"

void wg_face_boot(wg_app_t *a, wg_canvas_t *c);
void wg_face_home(wg_app_t *a, wg_canvas_t *c);
void wg_face_message(wg_app_t *a, wg_canvas_t *c);
void wg_face_message_open(wg_app_t *a);
void wg_face_diagnostic(wg_app_t *a, wg_canvas_t *c);
void wg_face_provision(wg_app_t *a, wg_canvas_t *c);

/* The offer is a capsule, and the message card is that same capsule after it
   has grown. Both shapes come from here so the morph is between one object's
   two states rather than between two objects that resemble each other. */
wg_rect_t wg_offer_rect(const wg_app_t *a);
wg_rect_t wg_card_rect(void);

/* Interpolates every dimension including the corner radius, which is what makes
   a capsule become a card instead of a capsule cross-fading into one. */
wg_rect_t wg_rect_mix(wg_rect_t a, wg_rect_t b, float t);

/* The line the device says when nobody has sent anything.

   The clock justifies the object being plugged in; this is what justifies
   looking at it. It is deliberately not a notification: no mark, no capsule, no
   affordance, nothing to open. It is just true and it is always there, and a
   real message from him takes precedence over it without erasing it. */
const char *wg_ambient_line(const wg_app_t *a);

/* The words inside the capsule. Shared so the pill's width and the label it
   contains can never disagree by a pixel. */
void wg_offer_label(const wg_app_t *a, char *buf, size_t n);

/* Shared between home and message so the two never disagree about the clock. */
void wg_app_clock_strings(const wg_app_t *a, char *time_out, size_t tn, char *date_out, size_t dn);
const char *wg_app_meridiem(const wg_app_t *a);

/* Paints the whole glass stack for a rounded rect and leaves the content to the
   caller: refraction at the rim, blurred backdrop, adaptive fill, and the two
   specular lips. One function so the capsule and the card are the same material
   rather than two things that happen to look similar. */
void wg_glass(wg_canvas_t *c, const wg_app_t *a, wg_rect_t r, float alpha, float lift);

/* Glass colours for the sky at this hour. Over a dark sky the material lifts
   toward white; over a bright one it darkens instead. A fixed translucency is
   legible on exactly one of those, and this panel lives through both every day:
   the same card that reads as glass at midnight washes out at noon. */
void wg_material(float hours, wg_color *fill_top, wg_color *fill_bottom,
                 wg_color *edge_top, wg_color *edge_bottom);

/* Type set straight onto the scene, with its own ground. One dark pass offset a
   pixel down is enough to hold light type over a bright sky without resorting
   to a scrim over the whole panel. */
int wg_text_over(wg_canvas_t *c, const wg_font_t *f, int x, int y, wg_align_t align,
                 wg_color color, const char *s);

/* The heart mark. Drawn rather than baked so it can be scaled and tinted by the
   breath animation without a sprite per size. */
void wg_draw_heart(wg_canvas_t *c, float cx, float cy, float size, wg_color color);

#endif
