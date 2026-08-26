/* WebAssembly port.
 *
 * A third backend for the same core/ sources, alongside the panel and SDL. The
 * browser owns the clock and the frame loop; this file owns nothing except the
 * translation between them.
 *
 * It exists so the product can be judged the way it will be used: a whole day
 * scrubbed in a minute, a message sent and opened, a drag interrupted halfway.
 * None of that is testable from a screenshot. */

#include <emscripten/emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wedge/app.h"

static wg_app_t s_app;
static uint32_t *s_canvas;
static uint16_t *s_panel;
/* What the page blits: RGBA8888, already through the panel's RGB565 and its
   dither, and already scaled by the emission level the firmware asked for. The
   emulator shows the picture the glass would show, not a cleaner one. */
static uint8_t *s_rgba;

static double s_unix = 0.0;
static uint64_t s_ms = 0;
static uint8_t s_brightness = 255;
static int s_polls = 0;

static int64_t host_now(void *ctx) { return (int64_t)s_unix; }
static uint64_t host_ms(void *ctx) { return s_ms; }
static void host_brightness(void *ctx, uint8_t level) { s_brightness = level; }
static void host_poll(void *ctx) { s_polls++; }
static void host_ack(void *ctx, const char *id) { (void)id; }

EMSCRIPTEN_KEEPALIVE
void wedge_init(double unix_seconds, int reduce_motion)
{
    if (!s_canvas) {
        s_canvas = malloc((size_t)WG_W * WG_H * sizeof(uint32_t));
        s_panel = malloc((size_t)WG_W * WG_H * sizeof(uint16_t));
        s_rgba = malloc((size_t)WG_W * WG_H * 4);
    }
    s_unix = unix_seconds;
    s_ms = 0;

    wg_host_t host;
    memset(&host, 0, sizeof(host));
    host.now_unix = host_now;
    host.millis = host_ms;
    host.set_brightness = host_brightness;
    host.request_poll = host_poll;
    host.ack_read = host_ack;

    wg_app_init(&s_app, &host);
    s_app.config.reduce_motion = reduce_motion != 0;
    /* Re-init so the reduced-motion spring parameters take, since they are
       chosen at construction from the config. */
    if (reduce_motion) {
        wg_app_init(&s_app, &host);
        s_app.config.reduce_motion = true;
        s_app.card.damping = 1.0f;
        s_app.card.response = 0.26f;
        s_app.indicator.response = 0.30f;
    }
}

EMSCRIPTEN_KEEPALIVE
uint8_t *wedge_framebuffer(void) { return s_rgba; }

EMSCRIPTEN_KEEPALIVE
void wedge_set_clock(double unix_seconds) { s_unix = unix_seconds; }

EMSCRIPTEN_KEEPALIVE
double wedge_clock(void) { return s_unix; }

/* dt in seconds, clock_step in seconds of simulated time. Separating them lets
   the page run the day forward at 300x while motion still steps at real time,
   which is the only way to watch a sunset without the springs going with it. */
EMSCRIPTEN_KEEPALIVE
void wedge_tick(float dt, double clock_step)
{
    s_unix += clock_step;
    s_ms += (uint64_t)(dt * 1000.0f);

    wg_canvas_t canvas = { s_canvas, WG_W, WG_H };
    wg_app_tick(&s_app, dt);
    wg_app_render(&s_app, &canvas);
    wg_to_rgb565(&canvas, s_panel);

    /* Emission is perceptual, not linear: a panel at level 36 is dim, not
       eighty-six percent black. */
    float k = (float)s_brightness / 255.0f;
    k = 0.22f + 0.78f * __builtin_powf(k, 1.0f / 2.2f);

    for (int i = 0; i < WG_W * WG_H; i++) {
        uint16_t p = s_panel[i];
        unsigned r = ((p >> 11) & 0x1F) * 255u / 31u;
        unsigned g = ((p >> 5) & 0x3F) * 255u / 63u;
        unsigned b = (p & 0x1F) * 255u / 31u;
        s_rgba[i * 4 + 0] = (uint8_t)(r * k);
        s_rgba[i * 4 + 1] = (uint8_t)(g * k);
        s_rgba[i * 4 + 2] = (uint8_t)(b * k);
        s_rgba[i * 4 + 3] = 255;
    }
}

EMSCRIPTEN_KEEPALIVE
void wedge_event(int kind, int x, int y)
{
    wg_event_t e = { (wg_event_kind_t)kind, (int16_t)x, (int16_t)y };
    wg_app_event(&s_app, &e);
}

EMSCRIPTEN_KEEPALIVE
void wedge_send(const char *id, const char *text, int type, int priority, double available_at)
{
    wg_message_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.id, sizeof(m.id), "%s", id);
    snprintf(m.text, sizeof(m.text), "%s", text);
    m.type = (wg_msg_type_t)type;
    m.state = WG_MSG_STATE_AVAILABLE;
    m.priority = (uint8_t)priority;
    m.available_at = (int64_t)available_at;
    wg_app_ingest(&s_app, &m);
}

/* Readouts for the page's instrument panel. Each is a plain scalar so the page
   needs no struct layout knowledge and cannot desync from the C side. */
EMSCRIPTEN_KEEPALIVE int wedge_state(void) { return (int)s_app.state; }
EMSCRIPTEN_KEEPALIVE int wedge_mode(void) { return (int)s_app.mode; }
EMSCRIPTEN_KEEPALIVE int wedge_brightness(void) { return (int)s_app.brightness; }
EMSCRIPTEN_KEEPALIVE int wedge_pending(void) { return wg_msg_cache_pending(&s_app.cache, (int64_t)s_unix); }
EMSCRIPTEN_KEEPALIVE int wedge_cached(void) { return s_app.cache.count; }
EMSCRIPTEN_KEEPALIVE int wedge_polls(void) { return s_polls; }
EMSCRIPTEN_KEEPALIVE int wedge_has_kept(void) { return s_app.kept != NULL; }
EMSCRIPTEN_KEEPALIVE float wedge_card(void) { return s_app.card.value; }
EMSCRIPTEN_KEEPALIVE float wedge_card_velocity(void) { return s_app.card.velocity; }
EMSCRIPTEN_KEEPALIVE float wedge_hours(void) { return s_app.hours; }
EMSCRIPTEN_KEEPALIVE float wedge_moon_phase(void) { return s_app.scene.moon_phase; }
EMSCRIPTEN_KEEPALIVE int wedge_ambient_count(void) { return wg_ambient_count(&s_app); }
EMSCRIPTEN_KEEPALIVE const char *wedge_ambient_at(int i) { return wg_ambient_at(&s_app, i); }
EMSCRIPTEN_KEEPALIVE int wedge_ambient_set(int i, const char *t) { return wg_ambient_set(&s_app, i, t) ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE int wedge_ambient_add(const char *t) { return wg_ambient_add(&s_app, t) ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE int wedge_ambient_remove(int i) { return wg_ambient_remove(&s_app, i) ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE void wedge_ambient_reset(void) { wg_ambient_reset(&s_app); }
EMSCRIPTEN_KEEPALIVE int wedge_ambient_width(const char *t) { return wg_ambient_width(t); }
EMSCRIPTEN_KEEPALIVE int wedge_ambient_limit_px(void) { return WG_AMBIENT_WIDTH_PX; }
EMSCRIPTEN_KEEPALIVE int wedge_ambient_max(void) { return WG_AMBIENT_MAX; }
EMSCRIPTEN_KEEPALIVE int wedge_ambient_text_max(void) { return WG_AMBIENT_TEXT - 1; }

EMSCRIPTEN_KEEPALIVE
void wedge_provision(int stage, const char *ap, const char *detail)
{
    wg_app_provisioning(&s_app, (wg_prov_stage_t)stage, ap, detail);
}

EMSCRIPTEN_KEEPALIVE
void wedge_set_wifi(int up)
{
    wg_event_t e = { up ? WG_EV_WIFI_UP : WG_EV_WIFI_DOWN, 0, 0 };
    wg_app_event(&s_app, &e);
}

int main(void)
{
    return 0;
}
