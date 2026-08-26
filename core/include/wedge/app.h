#ifndef WEDGE_APP_H
#define WEDGE_APP_H

#include "wedge/message.h"
#include "wedge/scene.h"

/* The explicit state machine from section 7. Scheduled day modes are tracked
   separately because they run whether or not a message is being presented. */
typedef enum {
    WG_ST_BOOT = 0,
    WG_ST_INITIALIZING,
    WG_ST_CONNECTING,
    WG_ST_SYNCING_TIME,
    WG_ST_HOME,
    WG_ST_MESSAGE_AVAILABLE,
    WG_ST_MESSAGE_PRESENTATION,
    WG_ST_DIAGNOSTIC,
    WG_ST_PROVISIONING,
    WG_ST_COUNT,
} wg_state_t;

/* Setup runs on the panel as well as on the phone. Whoever is holding this has
   no serial console and no idea what an SSID is; the glass has to say what to
   do and what is happening. */
typedef enum {
    WG_PROV_OFF = -1,
    WG_PROV_WAIT = 0,   /* raising an access point, waiting for a phone */
    WG_PROV_CLIENT,     /* a phone has joined; choose a network on it */
    WG_PROV_TRYING,     /* testing the credentials that came back */
    WG_PROV_FAILED,     /* they did not work; the portal is up again */
    WG_PROV_FORGET,     /* the button is being held down to erase the network */
    /* Joined a network that wants a sign-in before it will carry traffic. The
       device cannot fill in a web form, so it lends its own connection to a
       phone instead and asks the person to sign in on its behalf. */
    WG_PROV_RELAY,
} wg_prov_stage_t;

typedef enum {
    WG_MODE_SLEEP = 0,
    WG_MODE_MORNING,
    WG_MODE_DAY,
    WG_MODE_EVENING,
    WG_MODE_NIGHT,
    WG_MODE_COUNT,
} wg_mode_t;

typedef enum {
    WG_EV_NONE = 0,
    WG_EV_TOUCH_DOWN,
    WG_EV_TOUCH_MOVE,
    WG_EV_TOUCH_UP,
    WG_EV_BUTTON_DOWN,
    WG_EV_BUTTON_UP,
    WG_EV_BUTTON_LONG,
    WG_EV_WIFI_UP,
    WG_EV_WIFI_DOWN,
    WG_EV_TIME_SYNCED,
    WG_EV_MESSAGE_ARRIVED,
    WG_EV_POLL_FAILED,
} wg_event_kind_t;

typedef struct {
    wg_event_kind_t kind;
    int16_t x, y;
} wg_event_t;

/* Everything the port must supply. Network calls are fire-and-forget: the port
   does the work on its own task and pushes results back through wg_app_event,
   so nothing on the render path can ever block on a socket. */
typedef struct {
    void *ctx;
    int64_t (*now_unix)(void *ctx);
    uint64_t (*millis)(void *ctx);
    void (*set_brightness)(void *ctx, uint8_t level);
    void (*request_poll)(void *ctx);
    void (*ack_read)(void *ctx, const char *id);
    void (*persist)(void *ctx, const void *blob, size_t n);
    size_t (*restore)(void *ctx, void *blob, size_t n);
} wg_host_t;

typedef struct {
    /* Schedule boundaries in whole hours, configurable per section 15. */
    uint8_t morning_start;
    uint8_t day_start;
    uint8_t evening_start;
    uint8_t night_start;
    uint8_t sleep_start;
    uint8_t brightness_min;
    uint8_t brightness_max;
    /* Used only when tz_auto_pacific is off. */
    int16_t tz_offset_minutes;
    /* US Pacific with its daylight-saving rule applied, rather than a fixed
       offset. A bedside clock that needs reflashing twice a year to stay
       correct is a broken bedside clock, and a fixed -8 is wrong for eight
       months of the year while a fixed -7 is wrong for the other four. */
    bool tz_auto_pacific;
    uint16_t poll_seconds;
    /* Motion is the whole character of this device, so reducing it is a setting
       rather than an absence. Springs go critically damped and shorter, and the
       environment stops drifting; nothing stops responding. */
    bool reduce_motion;
    /* Backdrop blur behind the glass. It is the largest single cost in a frame
       that has a card on screen, and the cost is dominated by PSRAM bandwidth
       rather than arithmetic, which has not been measured on the real panel.
       This is the first thing to turn off if the device cannot hold its frame
       rate during a drag; the material still reads without it. */
    bool glass_blur;
} wg_config_t;

void wg_config_defaults(wg_config_t *c);

/* The standing lines, editable at runtime and persisted, so changing what the
   device says does not mean recompiling and reflashing it. */
#define WG_AMBIENT_MAX 12
#define WG_AMBIENT_TEXT 96

typedef struct {
    char lines[WG_AMBIENT_MAX][WG_AMBIENT_TEXT];
    int count;
} wg_ambient_bank_t;

void wg_ambient_defaults(wg_ambient_bank_t *b);

typedef struct {
    float x, y, w, h, r;
} wg_rect_t;

typedef struct {
    wg_state_t state;
    wg_mode_t mode;
    wg_config_t config;
    wg_host_t host;

    wg_scene_t scene;
    wg_msg_cache_t cache;
    wg_ambient_bank_t ambient;
    wg_message_t *open; /* the message being presented, if any */
    wg_message_t *kept; /* read, still within its keep window; the button no longer reopens it */
    /* The capsule's shape at the moment the gesture happened, so the card grows
       out of the box that was actually touched. Recomputing it per frame meant
       it changed shape under the animation: opening a message drops the pending
       count to zero, which retires the mark and rewords the label, so the
       origin narrowed on the very frame the finger landed. */
    wg_rect_t card_from;

    /* Setup. Owned by the port, which is the only thing that can raise an
       access point, and read by the face. */
    wg_prov_stage_t prov_stage;
    char prov_ap[33];
    char prov_detail[64];

    bool wifi_up;
    bool time_synced;
    int64_t now_unix;
    float hours; /* local time of day, continuous */

    /* Presentation motion. Each is an independent spring so an interrupted
       open does not desync the card from the scene behind it. */
    wg_spring_t card;      /* 0 home, 1 message fully presented */
    wg_spring_t indicator; /* 0 absent, 1 offering a message */
    wg_spring_t drag;      /* live finger offset while dismissing */
    wg_spring_t press;     /* 0 at rest, 1 while a finger is down on the offer */
    /* 0 while booting, 1 once the scene has been revealed. The sky is a
       function of the hour, and before NTP answers the hour is a guess, so
       painting a night sky at boot is a picture of a time that may not be. */
    wg_spring_t reveal;

    bool dragging;
    float drag_start_y;
    float drag_last_y;
    uint64_t drag_last_ms;
    float drag_velocity;

    uint64_t ms;
    uint64_t state_since;
    uint64_t last_poll;
    uint8_t brightness;
    uint32_t frames;
    /* The caption and the one it replaced, so boot can crossfade between them
       instead of cutting. */
    char status[48];
    char status_prev[48];
    uint64_t status_since;
} wg_app_t;

void wg_app_init(wg_app_t *a, const wg_host_t *host);

/* Feed one input or one asynchronous result. Safe to call between ticks. */
void wg_app_event(wg_app_t *a, const wg_event_t *e);

/* Store a message the port fetched. The app decides whether it becomes the
   pending offer. */
void wg_app_ingest(wg_app_t *a, const wg_message_t *m);

/* Drives the setup face. Passing WG_PROV_OFF returns the device to its normal
   boot sequence. `ap` is the network name to join; `detail` is whatever should
   be said underneath, and may be empty. */
void wg_app_provisioning(wg_app_t *a, wg_prov_stage_t stage, const char *ap, const char *detail);

void wg_app_tick(wg_app_t *a, float dt);

void wg_app_render(wg_app_t *a, wg_canvas_t *c);

/* Reading and editing the standing lines. Every mutation persists through the
   host, so the bank survives a power cut without the caller remembering to
   save. An empty bank falls back to the built-in set rather than showing
   nothing: there is always something on the screen. */
int wg_ambient_count(const wg_app_t *a);
const char *wg_ambient_at(const wg_app_t *a, int index);
bool wg_ambient_set(wg_app_t *a, int index, const char *text);
bool wg_ambient_add(wg_app_t *a, const char *text);
bool wg_ambient_remove(wg_app_t *a, int index);
void wg_ambient_reset(wg_app_t *a);

/* How much room a standing line has on the panel, and how wide a given line
   would be at the size it is drawn.

   A character count cannot express this: ordinary prose runs about nine pixels
   per character in this face, so fifty or so fit, but a line of capitals runs
   nineteen and only twenty-four would. Editors should measure. The renderer
   drops to the smaller face and then truncates regardless, so nothing can run
   off the glass whatever an editor allows. */
#define WG_AMBIENT_WIDTH_PX 460
int wg_ambient_width(const char *text);

/* Replaces the whole bank in one write, which is what the sender interface
   sends: editing lines one at a time over the network would leave the device in
   a half-updated state if the connection dropped between them. */
bool wg_ambient_replace(wg_app_t *a, const char *const *lines, int count);

/* Loads the bank from the host, falling back to the built-in set if nothing
   valid is stored. Called by wg_app_init. */
void wg_ambient_restore(wg_app_t *a);

const char *wg_state_name(wg_state_t s);
const char *wg_mode_name(wg_mode_t m);

/* The scheduled mode for an hour, and the brightness that mode wants. With no
   ambient light sensor on this board, the clock is the only input to
   brightness, so the curve has to be right on its own. */
wg_mode_t wg_mode_for_hour(const wg_config_t *c, float hours);
uint8_t wg_brightness_for(const wg_config_t *c, float hours);

/* Minutes to add to UTC for US Pacific at this instant: -480 on standard time,
   -420 while daylight saving is in effect. */
int wg_pacific_offset_minutes(int64_t utc);

/* The offset actually in force, honouring tz_auto_pacific. */
int wg_tz_offset_minutes(const wg_config_t *c, int64_t utc);

#endif
