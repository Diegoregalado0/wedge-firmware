#include "faces.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../fonts/fonts.h"

/* Setup, on the panel.
 *
 * Whoever is holding this device the first time has no serial console, no app,
 * and no reason to know what an SSID is. Everything they need is one sentence,
 * and the glass says it: join this network, then the phone takes over.
 *
 * Drawn on black like boot, for the same reason: the clock is not known yet, so
 * a sky built from it would be a picture of a time that may not be.
 */
void wg_face_provision(wg_app_t *a, wg_canvas_t *c)
{
    const wg_color accent = WG_RGB(232, 147, 92);
    const wg_color ink = WG_RGB(238, 236, 232);
    const wg_color dim = WG_RGB(150, 154, 166);

    float t = (float)(a->ms - a->state_since) / 1000.0f;
    float in = wg_smooth(0.0f, 0.5f, t);
    unsigned ia = (unsigned)(255.0f * in);
    const int cx = WG_W / 2;

    if (a->prov_stage == WG_PROV_FORGET) {
        /* A held button, seconds from erasing the network. This is the one
           screen that is allowed to be alarming, because the action is not
           reversible from the couch. */
        wg_text(c, &wg_font_kicker, cx, 92, WG_ALIGN_CENTER,
                WG_RGBA(226, 112, 95, ia), "FORGETTING THIS NETWORK");
        wg_text(c, &wg_font_msg, cx, 136, WG_ALIGN_CENTER, WG_RGBA(WG_R(ink), WG_G(ink), WG_B(ink), ia),
                a->prov_detail);
        wg_text(c, &wg_font_label, cx, 168, WG_ALIGN_CENTER,
                WG_RGBA(WG_R(dim), WG_G(dim), WG_B(dim), (unsigned)(ia * 0.8f)),
                "Let go to keep it");
        return;
    }

    if (a->prov_stage == WG_PROV_RELAY) {
        /* Three lines, in the order they are acted on: what is happening, the
           name to look for, what to do. Nothing here explains why the network
           will not carry traffic or what the Wedge is doing about it, because
           knowing that changes nothing about what the person has to do next. */
        wg_text(c, &wg_font_kicker, cx, 66, WG_ALIGN_CENTER,
                WG_RGBA(WG_R(accent), WG_G(accent), WG_B(accent), ia), "FINISH ON YOUR PHONE");
        wg_text(c, &wg_font_msg, cx, 122, WG_ALIGN_CENTER,
                WG_RGBA(WG_R(ink), WG_G(ink), WG_B(ink), ia), a->prov_ap);
        wg_text(c, &wg_font_label, cx, 166, WG_ALIGN_CENTER,
                WG_RGBA(WG_R(dim), WG_G(dim), WG_B(dim), (unsigned)(ia * 0.9f)),
                "Join this network, then sign in when your phone asks.");
        return;
    }

    const char *kicker = "SET UP";
    const char *hint = "";
    switch (a->prov_stage) {
    case WG_PROV_WAIT:
        hint = "On your phone, join this Wi-Fi network";
        break;
    case WG_PROV_CLIENT:
        kicker = "ALMOST THERE";
        hint = "Your phone should open a page. If not, visit wedge.setup";
        break;
    case WG_PROV_TRYING:
        kicker = "CONNECTING";
        hint = "";
        break;
    case WG_PROV_FAILED:
        kicker = "THAT DID NOT WORK";
        hint = "Join the network again and check the password";
        break;
    default:
        break;
    }

    wg_text(c, &wg_font_kicker, cx, 80, WG_ALIGN_CENTER,
            WG_RGBA(WG_R(accent), WG_G(accent), WG_B(accent), ia), kicker);

    /* The line that matters: the network name while waiting, the network being
       joined while connecting. Set at the message size, because it is the one
       thing on screen anyone needs to read from across a room. */
    const char *headline = a->prov_stage == WG_PROV_TRYING ? a->prov_detail : a->prov_ap;
    if (headline[0]) {
        wg_text(c, &wg_font_msg, cx, 132, WG_ALIGN_CENTER,
                WG_RGBA(WG_R(ink), WG_G(ink), WG_B(ink), ia), headline);
    }

    if (a->prov_stage == WG_PROV_TRYING) {
        /* A slow sweep rather than a spinner. It says the device is working
           without implying it knows how long it will take. */
        float phase = a->scene.t * 1.1f;
        for (int i = 0; i < 3; i++) {
            float k = 0.5f + 0.5f * sinf(phase - (float)i * 0.7f);
            wg_disc(c, (float)cx - 14.0f + (float)i * 14.0f, 168.0f, 3.0f,
                    WG_RGBA(WG_R(accent), WG_G(accent), WG_B(accent), (unsigned)(ia * (0.25f + 0.75f * k))));
        }
        return;
    }

    if (hint[0]) {
        wg_text(c, &wg_font_label, cx, 172, WG_ALIGN_CENTER,
                WG_RGBA(WG_R(dim), WG_G(dim), WG_B(dim), (unsigned)(ia * 0.9f)), hint);
    }
}
