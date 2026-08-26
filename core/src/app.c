#include "wedge/app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "faces.h"

void wg_config_defaults(wg_config_t *c)
{
    c->morning_start = 7;
    c->day_start = 10;
    c->evening_start = 17;
    c->night_start = 21;
    c->sleep_start = 0;
    /* The floor is not zero: the panel must still be readable if she wakes at
       03:00, just not bright enough to wake her further. With no light sensor
       on this board these two numbers are the entire brightness policy. */
    c->brightness_min = 6;
    c->brightness_max = 210;
    c->tz_auto_pacific = true;
    c->tz_offset_minutes = -8 * 60; /* the fallback if auto is ever switched off */
    c->poll_seconds = 300;
    c->glass_blur = true;
}

const char *wg_state_name(wg_state_t s)
{
    switch (s) {
    case WG_ST_BOOT: return "BOOT";
    case WG_ST_INITIALIZING: return "INITIALIZING";
    case WG_ST_CONNECTING: return "CONNECTING";
    case WG_ST_SYNCING_TIME: return "SYNCING_TIME";
    case WG_ST_HOME: return "HOME";
    case WG_ST_MESSAGE_AVAILABLE: return "MESSAGE_AVAILABLE";
    case WG_ST_MESSAGE_PRESENTATION: return "MESSAGE_PRESENTATION";
    case WG_ST_DIAGNOSTIC: return "DIAGNOSTIC";
    case WG_ST_PROVISIONING: return "PROVISIONING";
    default: return "?";
    }
}

const char *wg_mode_name(wg_mode_t m)
{
    switch (m) {
    case WG_MODE_SLEEP: return "SLEEP";
    case WG_MODE_MORNING: return "MORNING";
    case WG_MODE_DAY: return "DAY";
    case WG_MODE_EVENING: return "EVENING";
    case WG_MODE_NIGHT: return "NIGHT";
    default: return "?";
    }
}

wg_mode_t wg_mode_for_hour(const wg_config_t *c, float hours)
{
    int h = (int)hours;
    if (h >= c->night_start) {
        return WG_MODE_NIGHT;
    }
    if (h >= c->evening_start) {
        return WG_MODE_EVENING;
    }
    if (h >= c->day_start) {
        return WG_MODE_DAY;
    }
    if (h >= c->morning_start) {
        return WG_MODE_MORNING;
    }
    return WG_MODE_SLEEP;
}

uint8_t wg_brightness_for(const wg_config_t *c, float hours)
{
    /* Auto-dimming is switched off for now: full brightness at every hour,
       clock unconsulted. The curve is kept below under #if 0 rather than
       deleted, since turning it back on is meant to be flipping this to 1,
       not a rewrite. */
    (void)hours;
    return c->brightness_max;

#if 0
    /* With no light sensor on this board the clock is the only input, so the
       curve is defined by hand as control points rather than derived from the
       sun. Deriving it from altitude collapsed evening and deep night onto the
       same value, which is wrong: 21:00 in a lit bedroom and 03:00 in a dark
       one are different rooms.

       Levels are interpolated with smoothstep so no boundary is a step. */
    const float lo = (float)c->brightness_min;
    const float hi = (float)c->brightness_max;

    const float pts[][2] = {
        { 0.0f, lo },
        { (float)c->morning_start - 0.75f, lo },
        { (float)c->morning_start + 1.0f, hi * 0.62f },
        { (float)c->day_start, hi },
        { (float)c->evening_start, hi * 0.80f },
        { (float)c->evening_start + 2.5f, hi * 0.34f },
        { (float)c->night_start, hi * 0.20f },
        { (float)c->night_start + 2.0f, hi * 0.10f },
        { 24.0f, lo },
    };
    const int n = (int)(sizeof(pts) / sizeof(pts[0]));

    float h = wg_clampf(hours, 0.0f, 24.0f);
    float v = pts[n - 1][1];
    for (int i = 0; i < n - 1; i++) {
        if (h >= pts[i][0] && h <= pts[i + 1][0]) {
            float t = wg_smooth(pts[i][0], pts[i + 1][0], h);
            v = wg_lerpf(pts[i][1], pts[i + 1][1], t);
            break;
        }
    }
    return (uint8_t)wg_clampf(v, 0.0f, 255.0f);
#endif
}

/* Civil date from a Unix timestamp, Howard Hinnant's algorithm. Kept in core
   rather than calling localtime so the simulator and the device agree exactly. */
static void civil_from_unix(int64_t t, int *y, int *m, int *d, int *wd)
{
    int64_t days = t / 86400;
    if (t < 0 && t % 86400 != 0) {
        days--;
    }
    *wd = (int)((days % 7 + 11) % 7); /* 0 = Sunday */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t dd = doy - (153 * mp + 2) / 5 + 1;
    int64_t mm = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (mm <= 2 ? 1 : 0));
    *m = (int)mm;
    *d = (int)dd;
}

/* The inverse, so the daylight-saving transitions can be located by rule
   rather than by a table that would need extending every few years. */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* Day number of the nth given weekday of a month, 0 = Sunday. */
static int64_t nth_weekday_day(int y, int m, int nth, int weekday)
{
    int64_t first = days_from_civil(y, m, 1);
    int first_wd = (int)((first % 7 + 11) % 7);
    int delta = (weekday - first_wd + 7) % 7;
    return first + delta + (int64_t)(nth - 1) * 7;
}

int wg_pacific_offset_minutes(int64_t utc)
{
    int y, m, d, wd;
    civil_from_unix(utc, &y, &m, &d, &wd);
    /* Daylight saving runs from 02:00 local standard on the second Sunday in
       March, which is 10:00 UTC, to 02:00 local daylight on the first Sunday
       in November, which is 09:00 UTC. */
    int64_t start = nth_weekday_day(y, 3, 2, 0) * 86400 + 10 * 3600;
    int64_t end = nth_weekday_day(y, 11, 1, 0) * 86400 + 9 * 3600;
    return (utc >= start && utc < end) ? -7 * 60 : -8 * 60;
}

int wg_tz_offset_minutes(const wg_config_t *c, int64_t utc)
{
    return c->tz_auto_pacific ? wg_pacific_offset_minutes(utc) : c->tz_offset_minutes;
}

static const char *k_weekday[7] = { "Sunday",    "Monday", "Tuesday", "Wednesday",
                                    "Thursday",  "Friday", "Saturday" };
static const char *k_month[12] = { "January", "February", "March",     "April",   "May",      "June",
                                   "July",    "August",   "September", "October", "November", "December" };

void wg_app_clock_strings(const wg_app_t *a, char *time_out, size_t tn, char *date_out, size_t dn)
{
    /* Always a real time, never a placeholder. The port seeds the clock from
       flash, or from the moment the firmware was built when there is nothing
       in flash yet, so there is a plausible hour on the panel from the first
       frame and the network only ever refines it. */
    int h24 = (int)a->hours;
    int minute = (int)((a->hours - (float)h24) * 60.0f);
    int h12 = h24 % 12;
    if (h12 == 0) {
        h12 = 12;
    }
    snprintf(time_out, tn, "%d:%02d", h12, minute);

    int64_t local = a->now_unix + (int64_t)wg_tz_offset_minutes(&a->config, a->now_unix) * 60;
    int y, m, d, wd;
    civil_from_unix(local, &y, &m, &d, &wd);
    snprintf(date_out, dn, "%s, %s %d", k_weekday[wd % 7], k_month[(m - 1) % 12], d);
}

const char *wg_app_meridiem(const wg_app_t *a)
{
    return ((int)a->hours) < 12 ? "AM" : "PM";
}

static void say(wg_app_t *a, const char *text)
{
    if (strncmp(a->status, text, sizeof(a->status)) == 0) {
        return;
    }
    snprintf(a->status_prev, sizeof(a->status_prev), "%s", a->status);
    snprintf(a->status, sizeof(a->status), "%s", text);
    a->status_since = a->ms;
}

static void go(wg_app_t *a, wg_state_t s)
{
    if (a->state == s) {
        return;
    }
    a->state = s;
    a->state_since = a->ms;
}

void wg_app_init(wg_app_t *a, const wg_host_t *host)
{
    memset(a, 0, sizeof(*a));
    a->host = *host;
    wg_config_defaults(&a->config);
    wg_msg_cache_init(&a->cache);
    wg_ambient_restore(a);
    wg_scene_init(&a->scene, 0xC0FFEEu);

    /* Damping 1.0 everywhere by default. The card gets a little bounce only
       because it is the one thing a finger throws. */
    wg_spring_init(&a->card, 0.0f, 0.82f, 0.42f);
    wg_spring_init(&a->indicator, 0.0f, 1.0f, 0.55f);
    wg_spring_init(&a->drag, 0.0f, 1.0f, 0.30f);
    wg_spring_init(&a->press, 0.0f, 1.0f, 0.16f);
    wg_spring_init(&a->reveal, 0.0f, 1.0f, 0.75f);

    if (a->config.reduce_motion) {
        /* Reduced motion is not stillness. The card still travels and still
           follows the finger; it simply stops overshooting and gets there
           sooner, which removes the vestibular part without removing the
           feedback that says the touch landed. */
        a->card.damping = 1.0f;
        a->card.response = 0.26f;
        a->indicator.response = 0.30f;
    }

    a->hours = 21.0f;
    /* The panel itself comes up dark (rm67162_init sets emission level 0
       before app code runs, so a power cut mid-night doesn't flash full
       brightness into a dark bedroom) and expects the app to ramp it. This has
       to start at 0 to match: starting the software mirror at brightness_max
       made the first tick believe the panel was already where it needed to
       be, so the one push that would have turned it on never happened and it
       stayed dark forever. */
    a->brightness = 0;
    a->state = WG_ST_BOOT;
    say(a, "Starting up");
}

void wg_app_provisioning(wg_app_t *a, wg_prov_stage_t stage, const char *ap, const char *detail)
{
    a->prov_stage = stage;
    snprintf(a->prov_ap, sizeof(a->prov_ap), "%s", ap ? ap : "");
    snprintf(a->prov_detail, sizeof(a->prov_detail), "%s", detail ? detail : "");
    if (stage == WG_PROV_OFF) {
        if (a->state == WG_ST_PROVISIONING) {
            go(a, WG_ST_BOOT);
            a->state_since = a->ms;
        }
        return;
    }
    go(a, WG_ST_PROVISIONING);
}

void wg_app_ingest(wg_app_t *a, const wg_message_t *m)
{
    wg_message_t stored = *m;
    if (stored.state == WG_MSG_STATE_EMPTY) {
        stored.state = WG_MSG_STATE_AVAILABLE;
    }
    wg_msg_cache_put(&a->cache, &stored);
}

static void open_message(wg_app_t *a)
{
    /* Only a message that has not been read yet. The button is the entire
       interface this device has, one click to open and one click to close,
       and it does not also mean "show me that again": once something is
       read there is nothing left for a click to reach. */
    wg_message_t *m = wg_msg_cache_next(&a->cache, a->now_unix);
    if (!m) {
        return;
    }
    /* Captured before the state changes: marking the message presented drops
       the pending count to zero, which retires the mark and rewords the label,
       so a snapshot taken afterwards would be the wrong capsule. */
    a->card_from = wg_offer_rect(a);

    a->open = m;
    m->state = WG_MSG_STATE_PRESENTED;
    wg_spring_to(&a->card, 1.0f);
    go(a, WG_ST_MESSAGE_PRESENTATION);
    wg_face_message_open(a);
}

static void close_message(wg_app_t *a, float release_velocity)
{
    /* Re-captured on the way out: the capsule for whatever is next, if
       anything, has different words and a different width than the one that
       was there when it opened. The card should collapse into the box that
       will actually be there when it lands. */
    a->card_from = wg_offer_rect(a);

    if (a->open) {
        a->open->state = WG_MSG_STATE_READ;
        a->open->read_at = a->now_unix;
        /* The flag says the server has not been told yet, and it is cleared the
           moment the port takes the id. It was being set and never cleared
           anywhere, which meant no read message could ever be swept and the
           cache filled with entries it would hold until the next power cut.
           Delivery is the port's problem from here; it queues acks and keeps
           them across a disconnection. */
        a->open->read_ack_pending = true;
        if (a->host.ack_read) {
            a->host.ack_read(a->host.ctx, a->open->id);
            a->open->read_ack_pending = false;
        }
    }
    wg_spring_to(&a->card, 0.0f);
    /* Hand the finger's velocity straight to the spring so there is no seam
       between the drag and the animation that finishes it. */
    if (release_velocity != 0.0f) {
        wg_spring_kick(&a->card, release_velocity);
    }
}

void wg_app_event(wg_app_t *a, const wg_event_t *e)
{
    switch (e->kind) {
    case WG_EV_WIFI_UP:
        a->wifi_up = true;
        say(a, "Connected");
        if (a->state == WG_ST_CONNECTING) {
            go(a, WG_ST_SYNCING_TIME);
        }
        break;

    case WG_EV_WIFI_DOWN:
        a->wifi_up = false;
        /* Never a fatal screen for a missing network: the clock is still true
           and the cached messages are still hers. */
        say(a, "Offline");
        break;

    case WG_EV_TIME_SYNCED:
        a->time_synced = true;
        if (a->state == WG_ST_SYNCING_TIME || a->state == WG_ST_CONNECTING) {
            go(a, WG_ST_HOME);
        }
        break;

    case WG_EV_MESSAGE_ARRIVED:
        break;

    case WG_EV_POLL_FAILED:
        say(a, "Reconnecting");
        break;

    case WG_EV_BUTTON_UP:
        if (a->state == WG_ST_MESSAGE_PRESENTATION) {
            close_message(a, 0.0f);
        } else if (a->state == WG_ST_MESSAGE_AVAILABLE) {
            open_message(a);
        }
        break;

    case WG_EV_BUTTON_LONG:
        go(a, a->state == WG_ST_DIAGNOSTIC ? WG_ST_HOME : WG_ST_DIAGNOSTIC);
        break;

    case WG_EV_TOUCH_DOWN:
        /* Feedback belongs on the press, not the release. The offer dips the
           instant the glass is touched; waiting for touch-up to acknowledge a
           contact is what makes a screen feel dead. */
        if (a->state == WG_ST_MESSAGE_AVAILABLE) {
            wg_spring_to(&a->press, 1.0f);
        }
        if (a->state == WG_ST_MESSAGE_PRESENTATION) {
            /* Grab the card mid-flight. Because the spring keeps its live value
               rather than its target, a card still rising can be caught and
               thrown back down without a jump. */
            a->dragging = true;
            a->drag_start_y = (float)e->y;
            a->drag_last_y = (float)e->y;
            a->drag_last_ms = a->ms;
            a->drag_velocity = 0.0f;
        }
        break;

    case WG_EV_TOUCH_MOVE:
        if (a->dragging) {
            float dy = (float)e->y - a->drag_last_y;
            uint64_t dtms = a->ms - a->drag_last_ms;
            if (dtms > 0) {
                float inst = dy * 1000.0f / (float)dtms;
                /* A short running average, not the last sample: a single noisy
                   report at release should not decide where the card lands. */
                a->drag_velocity = a->drag_velocity * 0.6f + inst * 0.4f;
            }
            a->drag_last_y = (float)e->y;
            a->drag_last_ms = a->ms;

            float travel = (float)e->y - a->drag_start_y;
            float v = 1.0f - travel / (float)WG_H;
            if (v > 1.0f) {
                /* Past fully open there is nothing more to reach, so resist
                   progressively instead of stopping dead. */
                v = 1.0f + wg_rubberband(v - 1.0f, 1.0f, 0.55f);
            }
            wg_spring_set(&a->card, wg_clampf(v, 0.0f, 1.6f));
        }
        break;

    case WG_EV_TOUCH_UP:
        wg_spring_to(&a->press, 0.0f);
        if (a->dragging) {
            a->dragging = false;
            /* A touch that never went anywhere is a tap, and a tap on an open
               card closes it. Without this the card could only be dismissed by
               a deliberate swipe: a tap projected to exactly where it started,
               which is fully open, so the card sprang back and the screen
               ignored every press until the timeout retired it. */
            float travel = (float)e->y - a->drag_start_y;
            if (travel < 0.0f) {
                travel = -travel;
            }
            float speed = a->drag_velocity < 0.0f ? -a->drag_velocity : a->drag_velocity;
            if (travel < 12.0f && speed < 60.0f) {
                close_message(a, 0.0f);
                break;
            }
            /* Decide on the projected endpoint, not the release point, so a
               small fast flick dismisses and a large slow drag does not. */
            float pos = a->card.value;
            float projected = pos - wg_project(a->drag_velocity / (float)WG_H, 0.994f);
            if (projected < 0.55f) {
                close_message(a, -a->drag_velocity / (float)WG_H);
            } else {
                wg_spring_to(&a->card, 1.0f);
                wg_spring_kick(&a->card, -a->drag_velocity / (float)WG_H);
            }
        } else if (a->state == WG_ST_MESSAGE_AVAILABLE) {
            open_message(a);
        } else if (a->state == WG_ST_DIAGNOSTIC) {
            go(a, WG_ST_HOME);
        }
        break;

    default:
        break;
    }
}

/* Steps the panel toward the level the clock calls for. Pulled out of
   wg_app_tick because provisioning returns early and skips the rest of that
   function, and the panel needs this exactly as much while it is a black
   rectangle asking to be joined as it does once it is a clock: it comes up at
   emission level 0 by design and nothing else ever turns it on. */
static void wg_app_step_brightness(wg_app_t *a)
{
    uint8_t want = wg_brightness_for(&a->config, a->hours);
    /* Rate-limit brightness so a mode boundary is a slow ramp rather than a
       step she would notice from across the room. */
    if (want != a->brightness) {
        int delta = (int)want - (int)a->brightness;
        int step = delta > 0 ? 1 : -1;
        if (delta > 24 || delta < -24) {
            step = delta / 24;
        }
        a->brightness = (uint8_t)((int)a->brightness + step);
        if (a->host.set_brightness) {
            a->host.set_brightness(a->host.ctx, a->brightness);
        }
    }
}

void wg_app_tick(wg_app_t *a, float dt)
{
    a->ms = a->host.millis ? a->host.millis(a->host.ctx) : a->ms + (uint64_t)(dt * 1000.0f);
    a->frames++;

    if (a->host.now_unix) {
        a->now_unix = a->host.now_unix(a->host.ctx);
    }
    int64_t local = a->now_unix + (int64_t)wg_tz_offset_minutes(&a->config, a->now_unix) * 60;
    int64_t sod = local % 86400;
    if (sod < 0) {
        sod += 86400;
    }
    a->hours = (float)sod / 3600.0f;
    a->mode = wg_mode_for_hour(&a->config, a->hours);

    if (a->state == WG_ST_PROVISIONING) {
        /* Nothing else runs during setup. There is no clock to keep and no
           network to poll; the port is busy being an access point. */
        wg_spring_step(&a->reveal, dt);
        wg_scene_step(&a->scene, a->hours, dt);
        wg_app_step_brightness(a);
        return;
    }

    switch (a->state) {
    case WG_ST_BOOT:
        go(a, WG_ST_INITIALIZING);
        break;
    case WG_ST_INITIALIZING:
        if (a->ms - a->state_since > 400) {
            go(a, WG_ST_CONNECTING);
            say(a, "Looking for Wi-Fi");
        }
        break;
    case WG_ST_CONNECTING:
        /* The clock is the reason it earns its place on the table, so it stops
           waiting on the network after a few seconds and shows what it knows. */
        if (a->ms - a->state_since > 6000) {
            go(a, WG_ST_HOME);
        }
        break;
    case WG_ST_SYNCING_TIME:
        if (a->ms - a->state_since > 4000) {
            go(a, WG_ST_HOME);
        }
        break;
    default:
        break;
    }

    /* Boot advances on the flags rather than on catching each event in the one
       state that was listening for it. A router with cached credentials can
       associate before the 400 ms init window is over, and an event that
       arrives early must not be lost: dropping it stranded the device on the
       boot face for ten seconds with everything it needed already in hand. */
    switch (a->state) {
    case WG_ST_INITIALIZING:
    case WG_ST_CONNECTING:
    case WG_ST_SYNCING_TIME:
        if (a->wifi_up && a->time_synced) {
            go(a, WG_ST_HOME);
        } else if (a->wifi_up && a->state == WG_ST_CONNECTING) {
            go(a, WG_ST_SYNCING_TIME);
        }
        break;
    default:
        break;
    }

    a->kept = wg_msg_cache_kept(&a->cache, a->now_unix);
    if (a->state == WG_ST_HOME || a->state == WG_ST_MESSAGE_AVAILABLE) {
        bool fresh = wg_msg_cache_next(&a->cache, a->now_unix) != NULL;
        /* The indicator is lit only for something unread, so the gesture
           always has a genuinely new subject: the button opens and closes,
           and once a message is read a click has nothing left to reach. */
        go(a, fresh ? WG_ST_MESSAGE_AVAILABLE : WG_ST_HOME);
        wg_spring_to(&a->indicator, fresh ? 1.0f : 0.0f);
    }

    if (a->state == WG_ST_MESSAGE_PRESENTATION) {
        wg_spring_to(&a->indicator, 0.0f);
        /* An unattended presentation returns home on its own. The device is
           expected to be looked at, not operated, and a minute is long enough
           to read a long note twice without it becoming the resting state. */
        if (!a->dragging && a->ms - a->state_since > 60000) {
            close_message(a, 0.0f);
        }
        if (a->card.value < 0.02f && wg_spring_settled(&a->card)) {
            a->open = NULL;
            go(a, WG_ST_HOME);
        }
    }

    if (a->host.request_poll && a->wifi_up) {
        uint32_t interval = (uint32_t)a->config.poll_seconds * 1000u;
        /* Polling stops overnight: nothing is going to be read at 04:00, and a
           radio that stays quiet is a device that stays cool in a sealed shell. */
        if (a->mode == WG_MODE_SLEEP) {
            interval *= 4;
        }
        /* last_poll starts at zero, meaning "never polled," and zero is
           itself a plausible uptime early in boot: without the explicit
           check below the first poll waited out a full interval before ever
           firing; up to twenty minutes overnight, on every single boot. */
        if (a->last_poll == 0 || a->ms - a->last_poll > interval) {
            a->last_poll = a->ms;
            a->host.request_poll(a->host.ctx);
        }
    }

    /* Never while something is on screen: the sweep compacts the array that
       a->open points into, so retiring an entry mid-presentation would leave
       the card rendering whatever slid into that slot. */
    if (a->state != WG_ST_MESSAGE_PRESENTATION) {
        a->open = NULL;
        wg_msg_cache_sweep(&a->cache, a->now_unix);
    }

    if (!a->dragging) {
        wg_spring_step(&a->card, dt);
    }
    wg_spring_step(&a->indicator, dt);
    wg_spring_step(&a->press, dt);
    /* The scene is revealed only once boot is over, so the first thing she sees
       is not a sky drawn from a clock that has not been set. */
    wg_spring_to(&a->reveal, a->state > WG_ST_SYNCING_TIME ? 1.0f : 0.0f);
    wg_spring_step(&a->reveal, dt);
    /* The environment tracks the card rather than the state, so it comes back
       under the finger during a drag instead of waiting for the gesture to be
       classified as a dismissal. Motion should point where the gesture is
       going, not report where it ended up. */
    wg_spring_to(&a->scene.recede, wg_clampf(a->card.value, 0.0f, 1.0f));
    a->scene.moon_phase = wg_moon_phase(a->now_unix);
    wg_scene_step(&a->scene, a->hours, dt);

    wg_app_step_brightness(a);
}

bool wg_app_needs_full_frame(const wg_app_t *a)
{
    if (!a->config.glass_blur) {
        return false;
    }
    /* Glass is on screen whenever a card is up, or an offer is being made, or
       either is on its way in or out. */
    return a->card.value > 0.001f || a->indicator.value > 0.001f;
}

void wg_app_render(wg_app_t *a, wg_canvas_t *c)
{
    if (a->state == WG_ST_PROVISIONING) {
        wg_clear(c, WG_BLACK);
        wg_face_provision(a, c);
        return;
    }

    switch (a->state) {
    case WG_ST_BOOT:
    case WG_ST_INITIALIZING:
    case WG_ST_CONNECTING:
    case WG_ST_SYNCING_TIME:
        /* Black, not the scene. On an AMOLED that is genuinely off, which is
           the right thing for a device plugged in at night, and it avoids
           showing a sky built from an hour the device has not learned yet. */
        wg_clear(c, WG_BLACK);
        wg_face_boot(a, c);
        return;
    default:
        break;
    }

    wg_scene_draw(c, &a->scene);

    switch (a->state) {
    case WG_ST_DIAGNOSTIC:
        wg_face_diagnostic(a, c);
        break;
    default:
        wg_face_home(a, c);
        if (a->card.value > 0.001f) {
            wg_face_message(a, c);
        }
        break;
    }

    /* One dissolve out of black covering the whole first frame of the real UI,
       so the device arrives rather than cutting. */
    float veil = 1.0f - wg_clampf(a->reveal.value, 0.0f, 1.0f);
    if (veil > 0.002f) {
        wg_dim(c, veil);
    }
}
