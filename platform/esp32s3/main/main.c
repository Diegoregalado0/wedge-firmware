/* Device entry point.

   Three tasks, per section 34. They share nothing except two queues, so the
   only place app state is read or written is the UI task. That is what lets a
   blocking TLS handshake and a 60 Hz spring coexist without either one
   noticing the other. */

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cst816.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "net.h"
#include "provision.h"
#include "relay.h"
#include "rm67162.h"
#include "store.h"
#include "wedge/app.h"

static const char *TAG = "wedge";

/* The clock on first power-up.

   There is no battery-backed clock on this board, so a cold boot starts at the
   1970 epoch and the panel shows a confidently wrong hour. The last known time
   is restored from flash when there is one; when there is not, the moment this
   firmware was compiled is used instead. That is not the right time, but it is
   a real date, it is always in the past, and it is within days of the truth on
   a device that was just flashed, which beats January 1970 by two generations.
   Either way the network corrects it within seconds of coming up. */
static int64_t build_time_unix(void)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = { 0 };
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3 ||
        sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss) != 3) {
        return 0;
    }
    const char *at = strstr(months, mon);
    if (!at) {
        return 0;
    }
    int month = (int)((at - months) / 3) + 1;

    /* Howard Hinnant's days-from-civil, the same one core/ uses. */
    int y = year - (month <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    /* The compiler's clock is local time, which here is Pacific. Close enough
       for a seed that exists only to stop the panel saying 1969. */
    return days * 86400 + hh * 3600 + mm * 60 + ss + 8 * 3600;
}

static void seed_clock(void)
{
    int64_t t = 0;
    bool restored = store_time(&t);
    if (!restored || t < build_time_unix()) {
        t = build_time_unix();
        restored = false;
    }
    if (t <= 0) {
        return;
    }
    struct timeval tv = { .tv_sec = (time_t)t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "clock seeded from %s", restored ? "flash" : "build time");
}

#define PIN_BUTTON 0
#define LONG_PRESS_MS 900
/* Long enough that nobody reaches it by accident, and the panel warns from four
   seconds in. GPIO 0 is the boot strap pin, so the usual hold-at-power-on
   gesture is unavailable: that combination puts the chip in download mode. */
#define FORGET_WARN_MS 4000
#define FORGET_MS 10000

static QueueHandle_t s_events;   /* wg_event_t, into the UI task */
static QueueHandle_t s_messages; /* wg_message_t, into the UI task */
static QueueHandle_t s_acks;     /* message ids, out to the network task */
static volatile bool s_poll_requested;
static volatile uint8_t s_brightness = 0;
/* Setup owns the whole device while it runs, including the panel, so it reports
   its stage straight into the app the UI task is rendering. */
static wg_app_t *s_app;

static uint32_t *s_canvas;
static uint16_t *s_band;

/* Host interface implementation. These run on the UI task. */

static int64_t host_now_unix(void *ctx)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec;
}

static uint64_t host_millis(void *ctx)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void host_brightness(void *ctx, uint8_t level)
{
    s_brightness = level;
    rm67162_set_brightness(level);
}

static void host_request_poll(void *ctx)
{
    s_poll_requested = true;
}

static void host_ack_read(void *ctx, const char *id)
{
    char buf[WG_MSG_ID_MAX];
    snprintf(buf, sizeof(buf), "%s", id);
    xQueueSend(s_acks, buf, 0);
}

static void host_persist(void *ctx, const void *blob, size_t n)
{
    store_save(blob, n);
}

static size_t host_restore(void *ctx, void *blob, size_t n)
{
    return store_load(blob, n);
}

/* Network callbacks, on the network task. Enqueue only. */

static void on_wifi(bool up)
{
    wg_event_t e = { up ? WG_EV_WIFI_UP : WG_EV_WIFI_DOWN, 0, 0 };
    xQueueSend(s_events, &e, 0);
}

static void on_time(void)
{
    wg_event_t e = { WG_EV_TIME_SYNCED, 0, 0 };
    xQueueSend(s_events, &e, 0);
}

static void on_message(const wg_message_t *m)
{
    xQueueSend(s_messages, m, pdMS_TO_TICKS(100));
}

/* Written by the network task and consumed by the UI task. Single producer,
   single consumer, and the flag is set only after the buffer is complete. */
static wg_ambient_bank_t s_ambient_in;
static volatile bool s_ambient_ready;

static void on_ambient(const char *const *lines, int count)
{
    wg_ambient_bank_t next;
    memset(&next, 0, sizeof(next));
    for (int i = 0; i < count && next.count < WG_AMBIENT_MAX; i++) {
        snprintf(next.lines[next.count], WG_AMBIENT_TEXT, "%s", lines[i]);
        next.count++;
    }
    if (next.count == 0) {
        return;
    }
    s_ambient_in = next;
    s_ambient_ready = true;
}

/* Runs on the network task. Counts only consecutive failures, because a
   network that works and blips is not the same thing as one that hands out an
   address and then carries nothing. */
static int s_poll_fail_streak;

static void on_poll_failed(void)
{
    wg_event_t e = { WG_EV_POLL_FAILED, 0, 0 };
    xQueueSend(s_events, &e, 0);

    if (++s_poll_fail_streak < 2 || !net_connected() || relay_active()) {
        return;
    }
    /* Ask plainly what is wrong before doing anything about it, and say so in
       the log either way. Lending the connection is still worth trying when
       the answer is that nothing replied at all, because a portal that drops
       packets rather than answering them looks identical from here. */
    net_reach_t reach = net_probe_reachability();
    if (reach == NET_REACH_OK) {
        ESP_LOGW(TAG, "internet is reachable; the backend itself is the problem");
        return;
    }
    ESP_LOGW(TAG, "%s after %d failed polls, lending the connection",
             reach == NET_REACH_PORTAL ? "a sign-in page is in the way" : "nothing answers",
             s_poll_fail_streak);
    if (relay_start() == ESP_OK && s_app) {
        wg_app_provisioning(s_app, WG_PROV_RELAY, relay_ap_name(), "");
    }
}

static void on_poll_ok(void)
{
    s_poll_fail_streak = 0;
    if (relay_active()) {
        relay_stop();
        if (s_app) {
            wg_app_provisioning(s_app, WG_PROV_OFF, "", "");
        }
    }
}

static void net_task(void *arg)
{
    const net_config_t cfg = {
        .base_url = CONFIG_WEDGE_BACKEND_URL,
        .device_id = CONFIG_WEDGE_DEVICE_ID,
        .token = CONFIG_WEDGE_DEVICE_TOKEN,
    };
    const net_callbacks_t cb = {
        .on_wifi = on_wifi,
        .on_time = on_time,
        .on_message = on_message,
        .on_poll_failed = on_poll_failed,
        .on_poll_ok = on_poll_ok,
        .on_ambient = on_ambient,
    };
    net_init(&cfg, &cb);

    for (;;) {
        char id[WG_MSG_ID_MAX];
        /* Acknowledgements go out before the next poll, otherwise the server
           hands back the message she just read. One that cannot be delivered
           goes to the back of the queue rather than being dropped. */
        int waiting = uxQueueMessagesWaiting(s_acks);
        while (waiting-- > 0 && xQueueReceive(s_acks, id, 0) == pdTRUE) {
            if (!net_ack_read(id)) {
                xQueueSend(s_acks, id, 0);
                break;
            }
        }
        /* Half-hourly, so a device that has been up for days does not fall
           back to a stale timestamp after a power cut. Rare enough that flash
           wear over the life of the device is not a consideration. */
        static int64_t last_clock_save;
        static int64_t last_retry;
        int64_t now_s = host_now_unix(NULL);
        if (now_s - last_clock_save > 1800) {
            last_clock_save = now_s;
            store_set_time(now_s);
        }
        /* The app asks for a poll every few minutes, which is the right rhythm
           for a device that is working. It is far too slow for one that is
           not: a network that needs signing in to would take three quarters
           of an hour to say so. While a streak is running, retry on its own
           every twenty seconds so the panel can explain itself. */
        bool retry_due = s_poll_fail_streak > 0 && s_poll_fail_streak < 6 &&
                         now_s - last_retry > 20;
        if (s_poll_requested || retry_due) {
            s_poll_requested = false;
            last_retry = now_s;
            net_poll_messages();
            net_poll_ambient();
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void input_task(void *arg)
{
    const gpio_config_t btn = {
        .pin_bit_mask = (1ULL << PIN_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    bool was_down = false;
    bool long_sent = false;
    bool warned = false;
    uint64_t down_at = 0;
    bool touch_down = false;

    for (;;) {
        /* The button is active low and shares GPIO0 with the boot strap, so it
           is only read after startup has finished sampling it. */
        bool down = gpio_get_level(PIN_BUTTON) == 0;
        uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);

        if (down && !was_down) {
            down_at = now;
            long_sent = false;
            wg_event_t e = { WG_EV_BUTTON_DOWN, 0, 0 };
            xQueueSend(s_events, &e, 0);
        } else if (down && !long_sent && now - down_at > LONG_PRESS_MS) {
            long_sent = true;
            wg_event_t e = { WG_EV_BUTTON_LONG, 0, 0 };
            xQueueSend(s_events, &e, 0);
        } else if (down && now - down_at > FORGET_MS) {
            store_clear_wifi();
            esp_restart();
        } else if (down && now - down_at > FORGET_WARN_MS && !warned) {
            warned = true;
            if (s_app) {
                char left[64];
                snprintf(left, sizeof(left), "Hold to set up a different Wi-Fi");
                wg_app_provisioning(s_app, WG_PROV_FORGET, "", left);
            }
        } else if (!down && was_down) {
            if (warned && s_app) {
                /* Released in time. Back to whatever it was doing. */
                wg_app_provisioning(s_app, WG_PROV_OFF, "", "");
            }
            warned = false;
            if (!long_sent) {
                wg_event_t e = { WG_EV_BUTTON_UP, 0, 0 };
                xQueueSend(s_events, &e, 0);
            }
        }
        was_down = down;

        cst816_point_t p;
        if (cst816_read(&p)) {
            if (p.pressed && !touch_down) {
                touch_down = true;
                wg_event_t e = { WG_EV_TOUCH_DOWN, p.x, p.y };
                xQueueSend(s_events, &e, 0);
            } else if (p.pressed) {
                wg_event_t e = { WG_EV_TOUCH_MOVE, p.x, p.y };
                xQueueSend(s_events, &e, 0);
            } else if (touch_down) {
                touch_down = false;
                wg_event_t e = { WG_EV_TOUCH_UP, p.x, p.y };
                xQueueSend(s_events, &e, 0);
            }
        }

        /* 100 Hz. Touch has to be sampled faster than the frame rate or a fast
           flick loses the samples the release velocity is computed from. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* Setup gets its own task; the UI task simply renders whatever stage it
   reports. */
static void on_provision(void *ctx, wg_prov_stage_t stage, const char *ap, const char *detail)
{
    if (s_app) {
        wg_app_provisioning(s_app, stage, ap, detail);
    }
}

static void provision_task(void *arg)
{
    provision_run(on_provision, NULL);
    vTaskDelete(NULL);
}

static void ui_task(void *arg)
{
    wg_host_t host = {
        .ctx = NULL,
        .now_unix = host_now_unix,
        .millis = host_millis,
        .set_brightness = host_brightness,
        .request_poll = host_request_poll,
        .ack_read = host_ack_read,
        .persist = host_persist,
        .restore = host_restore,
    };

    static wg_app_t app;
    wg_app_init(&app, &host);
    s_app = &app;

    /* Setup runs instead of the network, not alongside it: the radio cannot be
       an access point and a station on someone else's channel at once. */
    if (provision_needed()) {
        wg_app_provisioning(&app, WG_PROV_WAIT, "", "");
        xTaskCreatePinnedToCore(provision_task, "prov", 6144, NULL, 4, NULL, 0);
    } else {
        xTaskCreatePinnedToCore(net_task, "net", 8192, NULL, 4, NULL, 0);
    }

    wg_canvas_t canvas = wg_canvas_full(s_canvas, WG_W, WG_H);
    uint64_t last = (uint64_t)(esp_timer_get_time() / 1000);

    for (;;) {
        wg_event_t e;
        while (xQueueReceive(s_events, &e, 0) == pdTRUE) {
            wg_app_event(&app, &e);
        }
        wg_message_t m;
        while (xQueueReceive(s_messages, &m, 0) == pdTRUE) {
            wg_app_ingest(&app, &m);
        }
        if (s_ambient_ready) {
            s_ambient_ready = false;
            const char *lines[WG_AMBIENT_MAX];
            for (int i = 0; i < s_ambient_in.count; i++) {
                lines[i] = s_ambient_in.lines[i];
            }
            wg_ambient_replace(&app, lines, s_ambient_in.count);
        }

        uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
        float dt = (float)(now - last) / 1000.0f;
        last = now;

        wg_app_tick(&app, dt);

        /* Two ways to produce a frame, and the difference is where the pixels
           live while they are being worked on.

           A frame with glass in it is composed whole, in PSRAM, because the
           blur behind the glass reads rows either side of the one it writes.
           Everything else is composed a band at a time into internal memory,
           which has no miss penalty: the same arithmetic against SRAM instead
           of PSRAM, and the conversion that follows reads pixels that are
           already in cache because they were written moments ago. */
        /* Composed whole, in PSRAM.
    
           Drawing this a band at a time into internal memory was tried and
           measured: 224 ms a frame against 104 ms for this, and it exhausted
           the internal heap so thoroughly that TLS could no longer allocate a
           session. Both problems have the same cause. The compositor is built
           out of whole-frame primitives, so producing ten slices means ten
           passes over every glyph, every column of land and every cloud in
           order to keep a tenth of each, and the repeated setup costs far more
           than the cache misses it avoids. Making it pay would need the
           primitives themselves rewritten to be slice-native, which is a
           different project from moving the buffer. */
        wg_app_render(&app, &canvas);
        /* Converted and pushed a band at a time. The dither reads absolute y,
           so the bands are seamless. */
        for (int y0 = 0; y0 < WG_H; y0 += RM67162_BAND) {
            int rows = WG_H - y0 < RM67162_BAND ? WG_H - y0 : RM67162_BAND;
            wg_to_rgb565_rows(&canvas, s_band, y0, rows);
            rm67162_blit_rows(s_band, y0, rows);
        }

        /* Nothing held back while something is moving: a frame already costs
           most of a tick, so an added delay there is pure lost frame rate.
           The idle pause stays, because a clock that is not being touched has
           no reason to redraw sixty times a second and every one of those
           costs power in a sealed shell. */
        bool busy = app.dragging || !wg_spring_settled(&app.card) || app.state == WG_ST_MESSAGE_PRESENTATION;
        vTaskDelay(busy ? 1 : pdMS_TO_TICKS(25));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(store_init());
    seed_clock();
    ESP_ERROR_CHECK(rm67162_init());
    cst816_init();

    /* The canvas lives in PSRAM. Section 5.2 asks for exactly this: at 514 kB it
       would take all of internal SRAM and leave nothing for the TLS stack. The
       band the panel is fed from has to be internal, so it is kept small. */
    s_canvas = heap_caps_malloc((size_t)WG_W * WG_H * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_band = heap_caps_malloc((size_t)WG_W * RM67162_BAND * sizeof(uint16_t),
                              MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_canvas || !s_band) {
        /* Without buffers there is no display, which section 41 calls a
           display initialization failure. Restarting is the honest response;
           there is no degraded mode that still shows her the time. */
        ESP_LOGE(TAG, "no memory for frame buffers");
        esp_restart();
    }

    s_events = xQueueCreate(24, sizeof(wg_event_t));
    s_messages = xQueueCreate(8, sizeof(wg_message_t));
    s_acks = xQueueCreate(8, WG_MSG_ID_MAX);

    /* The UI task decides whether the network task or setup runs, because that
       depends on whether a working network is stored. */
    xTaskCreatePinnedToCore(ui_task, "ui", 8192, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(input_task, "input", 4096, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "up, free heap %u, psram %u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
