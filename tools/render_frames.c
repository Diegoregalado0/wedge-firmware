/* Renders the real firmware faces to image files without a window, so the
   composition can be reviewed at specific hours and specific states rather
   than only whenever the simulator happens to be in one. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wedge/app.h"

typedef struct {
    int64_t unix_time;
    uint64_t ms;
    uint8_t brightness;
} fake_t;

static int64_t f_now(void *c) { return ((fake_t *)c)->unix_time; }
static uint64_t f_ms(void *c) { return ((fake_t *)c)->ms; }
static void f_bright(void *c, uint8_t b) { ((fake_t *)c)->brightness = b; }

/* The panel's response to a backlight level is not linear, and simulating it as
   though it were made every night frame look uniformly crushed. Perceived
   luminance goes roughly as the 1/2.2 power, and the floor is lifted because
   even a dim AMOLED still shows its lit pixels clearly in a dark room. */
static float perceived(uint8_t brightness)
{
    float k = powf((float)brightness / 255.0f, 1.0f / 2.2f);
    return 0.22f + 0.78f * k;
}

static void write_ppm(const char *path, const wg_canvas_t *cv, uint8_t brightness)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", cv->w, cv->h);
    float k = perceived(brightness);
    for (int i = 0; i < cv->w * cv->h; i++) {
        uint32_t p = cv->px[i];
        unsigned char rgb[3] = {
            (unsigned char)(((p >> 16) & 0xFF) * k),
            (unsigned char)(((p >> 8) & 0xFF) * k),
            (unsigned char)((p & 0xFF) * k),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* Advance the app far enough that every spring has settled, so a still frame
   shows the resting composition rather than a moment mid-transition. */
static void settle(wg_app_t *a, wg_canvas_t *cv, fake_t *fk, float seconds)
{
    const float dt = 1.0f / 60.0f;
    int steps = (int)(seconds / dt);
    for (int i = 0; i < steps; i++) {
        fk->ms += 16;
        wg_app_tick(a, dt);
    }
    wg_app_render(a, cv);
}

/* Captures the open-and-dismiss transition frame by frame. Stills cannot show
   whether a spring settles well, and reviewing motion at one thirtieth speed is
   the only way to see what is actually in the in-between frames. */
static void capture_motion(const char *outdir)
{
    uint32_t *px = malloc((size_t)WG_W * WG_H * 4);
    wg_canvas_t cv = { px, WG_W, WG_H };

    fake_t fk;
    memset(&fk, 0, sizeof(fk));
    fk.unix_time = 1786147200 + 21 * 3600 + 42 * 60 + 7 * 3600;
    fk.ms = 100000;
    fk.brightness = 255;

    wg_host_t host;
    memset(&host, 0, sizeof(host));
    host.ctx = &fk;
    host.now_unix = f_now;
    host.millis = f_ms;
    host.set_brightness = f_bright;

    wg_app_t app;
    wg_app_init(&app, &host);
    app.state = WG_ST_HOME;
    wg_event_t up = { WG_EV_WIFI_UP, 0, 0 };
    wg_app_event(&app, &up);

    wg_message_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.id, sizeof(m.id), "motion");
    snprintf(m.text, sizeof(m.text), "You've got this. I know today is the big one.");
    m.type = WG_MSG_ENCOURAGEMENT;
    m.state = WG_MSG_STATE_AVAILABLE;
    m.priority = 1;
    wg_app_ingest(&app, &m);

    const float dt = 1.0f / 30.0f;
    const int total = 105;
    for (int i = 0; i < total; i++) {
        /* Tap at frame 12, then flick it away at frame 62, so the capture
           covers the arrival, the rest, and the dismissal in one pass. */
        if (i == 12) {
            wg_event_t tap = { WG_EV_TOUCH_UP, 268, 120 };
            wg_app_event(&app, &tap);
        }
        if (i == 62) {
            wg_event_t down = { WG_EV_TOUCH_DOWN, 268, 90 };
            wg_app_event(&app, &down);
        }
        if (i > 62 && i <= 70) {
            wg_event_t mv = { WG_EV_TOUCH_MOVE, 268, (int16_t)(90 + (i - 62) * 9) };
            wg_app_event(&app, &mv);
        }
        if (i == 71) {
            wg_event_t rel = { WG_EV_TOUCH_UP, 268, 162 };
            wg_app_event(&app, &rel);
        }

        fk.ms += 33;
        wg_app_tick(&app, dt);
        wg_app_render(&app, &cv);

        char path[256];
        snprintf(path, sizeof(path), "%s/motion-%03d.ppm", outdir, i);
        write_ppm(path, &cv, app.brightness);
    }
    printf("motion            %d frames at 30fps\n", total);
    free(px);
}

/* The boot face is the one screen defined by how it changes over its first few
   seconds, so it is captured as a sequence rather than as a settled still. */
static void capture_boot(const char *outdir)
{
    uint32_t *px = malloc((size_t)WG_W * WG_H * 4);
    wg_canvas_t cv = { px, WG_W, WG_H };

    fake_t fk;
    memset(&fk, 0, sizeof(fk));
    fk.unix_time = 1786147200 + 21 * 3600 + 42 * 60 + 7 * 3600;
    fk.ms = 0;
    fk.brightness = 255;

    wg_host_t host;
    memset(&host, 0, sizeof(host));
    host.ctx = &fk;
    host.now_unix = f_now;
    host.millis = f_ms;
    host.set_brightness = f_bright;

    wg_app_t app;
    wg_app_init(&app, &host);

    const float dt = 1.0f / 30.0f;
    const float marks[] = { 0.2f, 0.7f, 1.6f, 2.9f, 3.25f, 3.45f, 3.7f, 4.2f };
    int next = 0;
    float t = 0.0f;
    for (int i = 0; i < 160; i++) {
        /* Deliberately a slow start, so the caption actually appears and the
           crossfade between captions can be seen. */
        if (i == 40) {
            wg_event_t e = { WG_EV_WIFI_UP, 0, 0 };
            wg_app_event(&app, &e);
        }
        /* The clock lands late on purpose, so the capture covers the caption
           and then the dissolve out of black into the real scene. */
        if (i == 93) {
            wg_event_t e = { WG_EV_TIME_SYNCED, 0, 0 };
            wg_app_event(&app, &e);
        }
        fk.ms += 33;
        t += dt;
        wg_app_tick(&app, dt);
        wg_app_render(&app, &cv);
        if (next < (int)(sizeof(marks) / sizeof(marks[0])) && t >= marks[next]) {
            char path[256];
            snprintf(path, sizeof(path), "%s/boot-%d.ppm", outdir, next);
            write_ppm(path, &cv, 255);
            next++;
        }
    }
    printf("boot              %d frames\n", next);
    free(px);
}

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : ".";

    uint32_t *px = malloc((size_t)WG_W * WG_H * 4);
    wg_canvas_t cv = { px, WG_W, WG_H };

    struct {
        const char *name;
        int hour;
        int minute;
        int with_message;
        int open;
    } shots[] = {
        { "01-night", 21, 42, 0, 0 },     { "02-night-pending", 21, 42, 1, 0 },
        { "03-message-open", 21, 42, 1, 1 }, { "04-dawn", 6, 15, 0, 0 },
        { "05-morning", 7, 30, 1, 0 },    { "06-day", 13, 5, 0, 0 },
        { "07-dusk", 19, 20, 0, 0 },      { "08-sleep", 3, 12, 0, 0 },
        { "09-morning-open", 7, 30, 1, 1 },
        { "10-day-open", 13, 5, 1, 1 },   { "11-day-pending", 13, 5, 1, 0 },
    };

    for (size_t i = 0; i < sizeof(shots) / sizeof(shots[0]); i++) {
        fake_t fk;
        memset(&fk, 0, sizeof(fk));
        /* A real date, so the weekday and month on screen are the ones the
           device would actually print. Local time is what the app renders, so
           the configured offset is added back to land on the requested hour. */
        const int64_t day_base = 1786147200; /* 2026-08-08 00:00 UTC, a Saturday */
        fk.unix_time = day_base + (int64_t)shots[i].hour * 3600 + (int64_t)shots[i].minute * 60 + 7 * 3600;
        fk.ms = 100000;
        fk.brightness = 255;

        wg_host_t host;
        memset(&host, 0, sizeof(host));
        host.ctx = &fk;
        host.now_unix = f_now;
        host.millis = f_ms;
        host.set_brightness = f_bright;

        wg_app_t app;
        wg_app_init(&app, &host);
        app.state = WG_ST_HOME;
        wg_event_t up = { WG_EV_WIFI_UP, 0, 0 };
        wg_app_event(&app, &up);
        /* The clock only shows a time once it has been set, so a still frame
           of the resting composition has to say the time is known. */
        wg_event_t synced = { WG_EV_TIME_SYNCED, 0, 0 };
        wg_app_event(&app, &synced);

        if (shots[i].with_message) {
            wg_message_t m;
            memset(&m, 0, sizeof(m));
            snprintf(m.id, sizeof(m.id), "shot-%zu", i);
            if (shots[i].hour < 12) {
                snprintf(m.text, sizeof(m.text), "Good morning. The first thing I thought about was you.");
                m.type = WG_MSG_GOOD_MORNING;
            } else {
                snprintf(m.text, sizeof(m.text), "You've got this. I know today is the big one.");
                m.type = WG_MSG_ENCOURAGEMENT;
            }
            m.state = WG_MSG_STATE_AVAILABLE;
            m.priority = 1;
            wg_app_ingest(&app, &m);
        }

        settle(&app, &cv, &fk, 3.0f);

        if (shots[i].open) {
            wg_event_t tap = { WG_EV_TOUCH_UP, 268, 120 };
            wg_app_event(&app, &tap);
            settle(&app, &cv, &fk, 1.6f);
        }

        char path[256];
        snprintf(path, sizeof(path), "%s/%s.ppm", outdir, shots[i].name);
        write_ppm(path, &cv, app.brightness);
        printf("%-18s %s  mode=%-8s brightness=%3u\n", shots[i].name, path, wg_mode_name(app.mode),
               (unsigned)app.brightness);
    }

    free(px);

    capture_motion(outdir);
    capture_boot(outdir);
    return 0;
}
