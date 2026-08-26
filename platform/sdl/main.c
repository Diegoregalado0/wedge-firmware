/* Host simulator. It exists so the interaction can be judged before hardware,
   and it runs the same core/ sources the device runs: the only things that
   differ are where the pixels go and where the clock comes from.

   The window shows the panel at an integer scale with the wedge's 54 degree
   recline suggested by the surround, because a design judged flat-on is a
   design tuned for a viewing angle the object never has. */

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "wedge/app.h"

#define SCALE 2
#define MARGIN 40

typedef struct {
    uint64_t start_us;
    int64_t clock_offset; /* lets the demo run the day forward */
    float time_scale;
    uint8_t brightness;
    int poll_requests;
} sim_t;

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

static int64_t sim_now_unix(void *ctx)
{
    sim_t *s = (sim_t *)ctx;
    uint64_t elapsed = now_us() - s->start_us;
    return (int64_t)(elapsed / 1000000ull) * (int64_t)s->time_scale + s->clock_offset;
}

static uint64_t sim_millis(void *ctx)
{
    sim_t *s = (sim_t *)ctx;
    return (now_us() - s->start_us) / 1000ull;
}

static void sim_brightness(void *ctx, uint8_t level)
{
    ((sim_t *)ctx)->brightness = level;
}

static void sim_poll(void *ctx)
{
    ((sim_t *)ctx)->poll_requests++;
}

static void sim_ack(void *ctx, const char *id)
{
    (void)ctx;
    printf("[sim] acknowledged read: %s\n", id);
    fflush(stdout);
}

static const char *k_samples[][2] = {
    { "You've got this. I know today is the big one.", "encouragement" },
    { "Good morning. The first thing I thought about was you.", "good_morning" },
    { "Sleep well. Everything you did today was enough.", "good_night" },
    { "I saw a dog on the way to work that walked exactly like you do when you're pretending not to be excited.", "normal" },
    { "You are the best decision I have ever made.", "affection" },
};

static wg_msg_type_t type_from_name(const char *n)
{
    if (!strcmp(n, "good_morning")) return WG_MSG_GOOD_MORNING;
    if (!strcmp(n, "good_night")) return WG_MSG_GOOD_NIGHT;
    if (!strcmp(n, "encouragement")) return WG_MSG_ENCOURAGEMENT;
    if (!strcmp(n, "affection")) return WG_MSG_AFFECTION;
    return WG_MSG_NORMAL;
}

static void inject(wg_app_t *app, int which)
{
    static int counter = 0;
    wg_message_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.id, sizeof(m.id), "sim-%d", ++counter);
    snprintf(m.text, sizeof(m.text), "%s", k_samples[which][0]);
    m.type = type_from_name(k_samples[which][1]);
    m.state = WG_MSG_STATE_AVAILABLE;
    m.available_at = 0;
    m.expires_at = 0;
    m.priority = 1;
    wg_app_ingest(app, &m);
    printf("[sim] message queued: %s\n", m.text);
    fflush(stdout);
}

/* Draw the surround: a suggestion of the printed shell so the panel is judged
   as an object on a table rather than as a rectangle on a monitor. */
static void draw_surround(SDL_Renderer *r, int w, int h)
{
    SDL_SetRenderDrawColor(r, 18, 18, 20, 255);
    SDL_RenderClear(r);
    SDL_Rect shell = { MARGIN / 2, MARGIN / 2, w - MARGIN, h - MARGIN };
    SDL_SetRenderDrawColor(r, 38, 38, 42, 255);
    SDL_RenderFillRect(r, &shell);
    SDL_SetRenderDrawColor(r, 54, 54, 60, 255);
    SDL_RenderDrawRect(r, &shell);
}

int main(int argc, char **argv)
{
    int start_hour = -1;
    float time_scale = 1.0f;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hour") && i + 1 < argc) {
            start_hour = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--speed") && i + 1 < argc) {
            time_scale = (float)atof(argv[++i]);
        } else if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            /* handled below */
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    const int win_w = WG_W * SCALE + MARGIN * 2;
    const int win_h = WG_H * SCALE + MARGIN * 2;
    SDL_Window *win = SDL_CreateWindow("Wedge  536x240  1.91\" AMOLED", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, win_w, win_h, SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, WG_W, WG_H);

    uint32_t *pixels = (uint32_t *)malloc((size_t)WG_W * WG_H * sizeof(uint32_t));
    uint16_t *panel = (uint16_t *)malloc((size_t)WG_W * WG_H * sizeof(uint16_t));
    wg_canvas_t canvas = wg_canvas_full(pixels, WG_W, WG_H);

    sim_t sim;
    memset(&sim, 0, sizeof(sim));
    sim.start_us = now_us();
    sim.time_scale = time_scale;
    sim.brightness = 255;
    if (start_hour >= 0) {
        sim.clock_offset = (int64_t)start_hour * 3600 + 7 * 3600; /* undo the default tz */
    } else {
        sim.clock_offset = (int64_t)time(NULL);
    }

    wg_host_t host = {
        .ctx = &sim,
        .now_unix = sim_now_unix,
        .millis = sim_millis,
        .set_brightness = sim_brightness,
        .request_poll = sim_poll,
        .ack_read = sim_ack,
        .persist = NULL,
        .restore = NULL,
    };

    wg_app_t app;
    wg_app_init(&app, &host);

    /* The simulator reports the network as present immediately; the device port
       is the one with a real radio to wait on. */
    wg_event_t up = { WG_EV_WIFI_UP, 0, 0 };
    wg_app_event(&app, &up);
    wg_event_t synced = { WG_EV_TIME_SYNCED, 0, 0 };
    wg_app_event(&app, &synced);

    printf("Wedge simulator\n");
    printf("  1-5    queue a sample message      space  press the button\n");
    printf("  L      long press (diagnostic)     [ ]    step the clock by an hour\n");
    printf("  S      save a screenshot           Q      quit\n");
    printf("  drag   with the mouse to dismiss an open message\n\n");
    fflush(stdout);

    bool running = true;
    uint64_t last = now_us();
    int shot_index = 0;
    bool mouse_down = false;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
            } else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode k = ev.key.keysym.sym;
                if (k == SDLK_q || k == SDLK_ESCAPE) {
                    running = false;
                } else if (k >= SDLK_1 && k <= SDLK_5) {
                    inject(&app, k - SDLK_1);
                } else if (k == SDLK_SPACE) {
                    wg_event_t d = { WG_EV_BUTTON_DOWN, 0, 0 };
                    wg_app_event(&app, &d);
                } else if (k == SDLK_l) {
                    wg_event_t d = { WG_EV_BUTTON_LONG, 0, 0 };
                    wg_app_event(&app, &d);
                } else if (k == SDLK_LEFTBRACKET) {
                    sim.clock_offset -= 3600;
                } else if (k == SDLK_RIGHTBRACKET) {
                    sim.clock_offset += 3600;
                } else if (k == SDLK_s) {
                    char path[128];
                    snprintf(path, sizeof(path), "shot_%02d.bmp", shot_index++);
                    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
                        pixels, WG_W, WG_H, 32, WG_W * 4, SDL_PIXELFORMAT_ARGB8888);
                    SDL_SaveBMP(surf, path);
                    SDL_FreeSurface(surf);
                    printf("[sim] wrote %s\n", path);
                    fflush(stdout);
                }
            } else if (ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_SPACE) {
                wg_event_t u = { WG_EV_BUTTON_UP, 0, 0 };
                wg_app_event(&app, &u);
            } else if (ev.type == SDL_MOUSEBUTTONDOWN) {
                mouse_down = true;
                wg_event_t e = { WG_EV_TOUCH_DOWN, (int16_t)((ev.button.x - MARGIN) / SCALE),
                                 (int16_t)((ev.button.y - MARGIN) / SCALE) };
                wg_app_event(&app, &e);
            } else if (ev.type == SDL_MOUSEMOTION && mouse_down) {
                wg_event_t e = { WG_EV_TOUCH_MOVE, (int16_t)((ev.motion.x - MARGIN) / SCALE),
                                 (int16_t)((ev.motion.y - MARGIN) / SCALE) };
                wg_app_event(&app, &e);
            } else if (ev.type == SDL_MOUSEBUTTONUP) {
                mouse_down = false;
                wg_event_t e = { WG_EV_TOUCH_UP, (int16_t)((ev.button.x - MARGIN) / SCALE),
                                 (int16_t)((ev.button.y - MARGIN) / SCALE) };
                wg_app_event(&app, &e);
            }
        }

        uint64_t t = now_us();
        float dt = (float)(t - last) / 1000000.0f;
        last = t;

        wg_app_tick(&app, dt);
        wg_app_render(&app, &canvas);

        /* Round-trip through the panel's real RGB565 with the dither applied,
           so the simulator shows the banding the device would actually show
           rather than a cleaner 24-bit fantasy of it. */
        wg_to_rgb565(&canvas, panel);
        for (int i = 0; i < WG_W * WG_H; i++) {
            uint16_t p = panel[i];
            unsigned r = ((p >> 11) & 0x1F) * 255 / 31;
            unsigned g = ((p >> 5) & 0x3F) * 255 / 63;
            unsigned b = (p & 0x1F) * 255 / 31;
            /* Apply the backlight level the app asked for, so the night modes
               look on screen the way they will look in the room. */
            float k = (float)sim.brightness / 255.0f;
            k = 0.15f + 0.85f * k;
            pixels[i] = 0xFF000000u | ((unsigned)(r * k) << 16) | ((unsigned)(g * k) << 8) | (unsigned)(b * k);
        }

        SDL_UpdateTexture(tex, NULL, pixels, WG_W * 4);
        draw_surround(ren, win_w, win_h);
        SDL_Rect dst = { MARGIN, MARGIN, WG_W * SCALE, WG_H * SCALE };
        SDL_RenderCopy(ren, tex, NULL, &dst);
        SDL_RenderPresent(ren);
    }

    free(pixels);
    free(panel);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
