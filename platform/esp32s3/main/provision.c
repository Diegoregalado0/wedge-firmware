/* Wi-Fi setup over a captive portal.
 *
 * The device raises its own access point, answers every DNS query with its own
 * address, and serves one page on every URL. iOS probes for a captive portal the
 * moment it joins a network, gets that page instead of the reply it expects, and
 * opens it by itself. So setup is: join a network in Wi-Fi settings, pick your
 * own from the list that appears, type the password. No app, no account, no
 * cable, and nothing to install on a phone that is about to be handed back.
 *
 * The alternative was Espressif's own provisioning component, which is smaller
 * but requires the recipient to install an app first. For a device that is a
 * consumer appliance, that install is the whole cost of onboarding.
 */

#include "provision.h"

#include <string.h>
#include <sys/socket.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "net.h"
#include "store.h"

static const char *TAG = "prov";

#define PORTAL_IP "192.168.4.1"
/* Per attempt, not in total. An all-channel scan takes a couple of seconds
   before the association even starts, and DHCP on a busy guest network is not
   quick either. */
#define CONNECT_TIMEOUT_MS 15000
#define CONNECT_ATTEMPTS 3
#define BIT_STA_GOT_IP BIT0
#define BIT_STA_FAILED BIT1

static provision_cb_t s_cb;
static void *s_cb_ctx;
static char s_ap_name[32];
static httpd_handle_t s_httpd;
static EventGroupHandle_t s_events;
static TaskHandle_t s_dns_task;
static volatile bool s_saved;
static volatile int s_last_reason;

static void report(wg_prov_stage_t stage, const char *detail)
{
    if (s_cb) {
        s_cb(s_cb_ctx, stage, s_ap_name, detail ? detail : "");
    }
}

/* ---- DNS ----------------------------------------------------------------
   Answers any A query with our own address, which is what makes the phone's
   captive-portal probe land here instead of on the internet. */

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in server = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[256];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        /* A query needs a 12-byte header and at least one question. Anything
           shorter, or a reply rather than a query, is ignored rather than
           parsed: this socket is reachable by anyone on the setup network. */
        if (n < 13 || n > (int)sizeof(buf) - 16 || (buf[2] & 0x80)) {
            continue;
        }

        /* Walk the question's labels to find where it ends. */
        int p = 12;
        while (p < n && buf[p] != 0) {
            int len = buf[p];
            if (len > 63) {
                p = n;
                break;
            }
            p += len + 1;
        }
        if (p + 5 > n) {
            continue;
        }
        int qend = p + 5; /* terminator plus QTYPE and QCLASS */

        /* Decoded back to dotted form only for the log line; the wire format
           is left untouched below. */
        char name[192];
        int ni = 0;
        for (int i = 12; i < p && buf[i] != 0 && ni < (int)sizeof(name) - 2;) {
            int len = buf[i++];
            if (ni > 0) {
                name[ni++] = '.';
            }
            for (int k = 0; k < len && i < p && ni < (int)sizeof(name) - 2; k++, i++) {
                name[ni++] = (char)buf[i];
            }
        }
        name[ni] = '\0';
        ESP_LOGI(TAG, "dns query for \"%s\" from %s", name, inet_ntoa(from.sin_addr));

        buf[2] = 0x81; /* response, recursion desired */
        buf[3] = 0x80; /* recursion available, no error */
        buf[6] = 0;
        buf[7] = 1; /* one answer */
        buf[8] = 0;
        buf[9] = 0;
        buf[10] = 0;
        buf[11] = 0;

        uint8_t *ans = &buf[qend];
        const uint8_t rr[] = {
            0xC0, 0x0C,             /* pointer back to the question's name */
            0x00, 0x01, 0x00, 0x01, /* A, IN */
            0x00, 0x00, 0x00, 0x0A, /* ten second TTL */
            0x00, 0x04,             /* four bytes of address */
        };
        memcpy(ans, rr, sizeof(rr));
        uint32_t ip = inet_addr(PORTAL_IP);
        memcpy(ans + sizeof(rr), &ip, 4);
        sendto(sock, buf, qend + (int)sizeof(rr) + 4, 0, (struct sockaddr *)&from, from_len);
    }
}

/* ---- portal -------------------------------------------------------------- */

static const char k_page_head[] =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Set up your Wedge</title><style>"
    ":root{color-scheme:dark}"
    "body{margin:0 auto;max-width:420px;background:#0b0b0f;color:#ece9e6;"
    "font:17px/1.5 -apple-system,BlinkMacSystemFont,system-ui,sans-serif;padding:40px 22px 64px}"
    "h1{font-size:26px;line-height:1.2;font-weight:600;letter-spacing:-.02em;margin:0 0 8px}"
    ".sub{color:#9a958f;margin:0 0 28px}"
    "label{display:block;font-size:15px;font-weight:600;margin:0 0 8px}"
    /* 17px is not a style choice: below 16 iOS zooms the page in when the
       field takes focus, and the person is then pinching to find the button. */
    "select,input{width:100%;box-sizing:border-box;background:#141417;border:1px solid #2f2f36;"
    "border-radius:12px;color:#ece9e6;font:inherit;font-size:17px;padding:14px;margin:0 0 20px}"
    "button{width:100%;background:#e8935c;color:#1a1005;border:0;border-radius:12px;"
    "font:inherit;font-weight:600;font-size:17px;padding:16px;cursor:pointer}"
    "button[disabled]{opacity:.45}"
    "a{color:#e8935c;display:inline-block;margin-top:22px;font-size:15px}"
    "small{display:block;color:#6b6762;font-size:14px;margin-top:22px}"
    ".bad{background:#2a1512;border:1px solid #5c2b24;color:#f0a89b;border-radius:12px;"
    "padding:13px 15px;margin:0 0 22px;font-size:15px}"
    ".note{background:#1a1a20;border:1px solid #33333d;color:#b9b4ae;border-radius:12px;"
    "padding:13px 15px;margin:0 0 20px;font-size:15px}"
    "[hidden]{display:none}"
    "</style>"
    "<h1>Set up your Wedge</h1>"
    "<p class=sub>Pick your Wi-Fi so it can receive messages.</p>";

/* Everything after the network list. The password field and the typed-name
   field both start hidden and are revealed only when they are the thing being
   asked for: a setup screen that shows every field it might ever need is a
   setup screen that asks the person to work out which ones apply to them. */
static const char k_page_tail[] =
    "<div id=pw>"
    "<label for=p>Password</label>"
    "<input id=p name=p type=password autocomplete=current-password>"
    "</div>"
    "<p id=ent class=note hidden>This network asks people to sign in on a web page. "
    "The Wedge cannot do that. A home network or a phone hotspot will work.</p>"
    "<div id=man hidden>"
    "<label for=m>Network name</label>"
    "<input id=m name=m autocomplete=off autocapitalize=none autocorrect=off spellcheck=false>"
    "</div>"
    "<button id=go type=submit>Connect</button>"
    "</form>"
    "<a href='#' id=more>I do not see my network</a>"
    "<small>The password is saved on the Wedge and sent nowhere else.</small>"
    "<script>"
    "var f=document.getElementById('f'),s=document.getElementById('s'),"
    "nw=document.getElementById('nw'),pw=document.getElementById('pw'),"
    "ent=document.getElementById('ent'),man=document.getElementById('man'),"
    "go=document.getElementById('go'),more=document.getElementById('more');"
    /* p is disabled, not merely hidden. A hidden field still posts its value,
       so a password typed for one network and then left behind when an open
       one is picked would still be sent, and the driver raises its own
       security floor to WPA2 the moment a password is present. The open
       network then refuses the association and the page says to check the
       password, which is exactly the wrong thing to tell someone joining a
       network that has no password at all. */
    "function upd(){"
    "var p=document.getElementById('p');"
    "if(!man.hidden){pw.hidden=false;p.disabled=false;ent.hidden=true;go.disabled=false;return;}"
    "var o=s.options[s.selectedIndex];"
    "var e=!!(o&&o.getAttribute('data-e'));"
    "var op=!!(o&&o.getAttribute('data-o'));"
    "pw.hidden=op||e;p.disabled=pw.hidden;ent.hidden=!e;go.disabled=e;}"
    "s.onchange=upd;"
    "more.onclick=function(ev){ev.preventDefault();man.hidden=false;nw.hidden=true;"
    "more.hidden=true;upd();};"
    /* Deferred a tick: disabling a submit button from inside the submit
       handler itself cancels the submission in some browsers. */
    "f.onsubmit=function(){setTimeout(function(){go.disabled=true;"
    "go.textContent='Connecting…';},0);};"
    "upd();"
    "</script>";

/* Written into an HTML text node and an attribute, but an SSID is
   attacker-chosen text off the air: anything with a bracket or a quote in it
   would otherwise rewrite the page around it. */
static void html_escape(const char *in, char *out, size_t cap)
{
    size_t w = 0;
    for (const char *r = in; *r && w + 7 < cap; r++) {
        switch (*r) {
        case '<': memcpy(out + w, "&lt;", 4); w += 4; break;
        case '>': memcpy(out + w, "&gt;", 4); w += 4; break;
        case '&': memcpy(out + w, "&amp;", 5); w += 5; break;
        case '"': memcpy(out + w, "&quot;", 6); w += 6; break;
        case '\'': memcpy(out + w, "&#39;", 5); w += 5; break;
        default: out[w++] = *r; break;
        }
    }
    out[w] = '\0';
}

static bool authmode_is_enterprise(wifi_auth_mode_t m)
{
    return m == WIFI_AUTH_ENTERPRISE || m == WIFI_AUTH_WPA3_ENTERPRISE ||
           m == WIFI_AUTH_WPA2_WPA3_ENTERPRISE || m == WIFI_AUTH_WPA3_ENT_192;
}

static esp_err_t send_portal(httpd_req_t *req, const char *error)
{
    httpd_resp_set_type(req, "text/html");
    /* No caching: iOS keeps probing, and a stale page would hide a failure. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, k_page_head, HTTPD_RESP_USE_STRLEN);
    if (error && error[0]) {
        httpd_resp_send_chunk(req, "<p class=bad>", HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req, error, HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req, "</p>", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, "<form method=post action='/save' id=f>"
                               "<div id=nw><label for=s>Network</label>"
                               "<select id=s name=s>",
                          HTTPD_RESP_USE_STRLEN);

    /* Scanned live. Typing a network name on a phone keyboard is where setup
       goes to die, so the list is the path and the typed field is the escape.

       A previous attempt leaves the station mid-connect, and a scan started in
       that state is refused outright. That is what made the list come back
       empty and stay empty after one failed password: the error was never
       looked at past the start call, so the page rendered a menu with nothing
       in it and no way back except a power cycle. */
    esp_wifi_scan_stop();
    esp_wifi_disconnect();

    uint16_t count = 0;
    const wifi_scan_config_t scan = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 200 } },
    };
    esp_err_t serr = esp_wifi_scan_start(&scan, true);
    if (serr == ESP_OK) {
        esp_wifi_scan_get_ap_num(&count);
    } else {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(serr));
    }

    /* Every record, then dedupe, then trim. Trimming first was the bug that
       hid networks on a busy site: one campus network name is broadcast by
       dozens of access points, so the first twenty records can easily be
       twenty copies of two names and everything else is never looked at. */
    int shown = 0;
    if (count > 0) {
        wifi_ap_record_t *records = calloc(count, sizeof(wifi_ap_record_t));
        if (records) {
            esp_wifi_scan_get_ap_records(&count, records);
            ESP_LOGI(TAG, "scan found %u access points", count);
            for (int i = 0; i < count && shown < 24; i++) {
                const char *ssid = (const char *)records[i].ssid;
                if (!ssid[0]) {
                    continue;
                }
                /* Records arrive strongest first, so the first sighting of a
                   name is its nearest radio and later copies are dropped. */
                bool dup = false;
                for (int j = 0; j < i; j++) {
                    if (strcmp(ssid, (const char *)records[j].ssid) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (dup) {
                    continue;
                }
                char safe[128];
                html_escape(ssid, safe, sizeof(safe));
                bool ent = authmode_is_enterprise(records[i].authmode);
                bool open = records[i].authmode == WIFI_AUTH_OPEN;
                /* The clean name rides in the value, so the label is free to
                   say more without the extra words ending up in the SSID. */
                httpd_resp_send_chunk(req, "<option value='", HTTPD_RESP_USE_STRLEN);
                httpd_resp_send_chunk(req, safe, HTTPD_RESP_USE_STRLEN);
                httpd_resp_send_chunk(req, "'", HTTPD_RESP_USE_STRLEN);
                if (ent) {
                    httpd_resp_send_chunk(req, " data-e=1", HTTPD_RESP_USE_STRLEN);
                } else if (open) {
                    httpd_resp_send_chunk(req, " data-o=1", HTTPD_RESP_USE_STRLEN);
                }
                httpd_resp_send_chunk(req, ">", HTTPD_RESP_USE_STRLEN);
                httpd_resp_send_chunk(req, safe, HTTPD_RESP_USE_STRLEN);
                if (ent) {
                    /* Listed rather than hidden: it is on the phone's own list,
                       so leaving it out here reads as a broken scan. */
                    httpd_resp_send_chunk(req, " (needs a sign-in)", HTTPD_RESP_USE_STRLEN);
                }
                httpd_resp_send_chunk(req, "</option>", HTTPD_RESP_USE_STRLEN);
                shown++;
            }
            free(records);
        }
    }
    if (shown == 0) {
        httpd_resp_send_chunk(req, "<option value=''>No networks found</option>",
                              HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, "</select></div>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, k_page_tail, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t any_get(httpd_req_t *req)
{
    ESP_LOGI(TAG, "http GET %s", req->uri);
    report(WG_PROV_CLIENT, "");
    return send_portal(req, NULL);
}

/* Percent and plus decoding, in place. Form bodies arrive encoded and a Wi-Fi
   password is exactly the kind of string that is full of characters that get
   encoded. */
static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && r[1] && r[2]) {
            int hi = r[1], lo = r[2];
            hi = hi >= 'a' ? hi - 'a' + 10 : (hi >= 'A' ? hi - 'A' + 10 : hi - '0');
            lo = lo >= 'a' ? lo - 'a' + 10 : (lo >= 'A' ? lo - 'A' + 10 : lo - '0');
            if (hi >= 0 && hi < 16 && lo >= 0 && lo < 16) {
                *w++ = (char)((hi << 4) | lo);
                r += 2;
            } else {
                *w++ = *r;
            }
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static bool field(const char *body, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    for (const char *p = body; p && *p;) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t n = end ? (size_t)(end - v) : strlen(v);
            if (n >= cap) {
                n = cap - 1;
            }
            memcpy(out, v, n);
            out[n] = '\0';
            url_decode(out);
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return false;
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[512];
    int total = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, body + got, total - got);
        if (n <= 0) {
            return send_portal(req, "The form did not arrive. Try again.");
        }
        got += n;
    }
    body[got] = '\0';

    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    char manual[33] = { 0 };
    field(body, "s", ssid, sizeof(ssid));
    /* The typed field wins when it has anything in it. Someone who bothered to
       type a name is correcting the list, not agreeing with it. */
    if (field(body, "m", manual, sizeof(manual)) && manual[0]) {
        snprintf(ssid, sizeof(ssid), "%s", manual);
    }
    if (!ssid[0]) {
        return send_portal(req, "Choose a network, or type its name.");
    }
    field(body, "p", pass, sizeof(pass));

    report(WG_PROV_TRYING, ssid);
    ESP_LOGI(TAG, "trying %s", ssid);

    wifi_config_t sta = { 0 };
    memcpy(sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
    memcpy(sta.sta.password, pass, sizeof(sta.sta.password) - 1);
    net_apply_sta_defaults(&sta.sta);
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    s_last_reason = 0;

    /* Several attempts, not one. A first association failing and the next one
       succeeding is ordinary Wi-Fi behaviour, especially on a busy site, and
       the previous code reported failure on the very first disconnect event.
       That is what made joining a working network take five tries: each try
       was one attempt, and the person at the phone was doing the retrying by
       hand without being told that was what they were doing. */
    EventBits_t bits = 0;
    for (int attempt = 1; attempt <= CONNECT_ATTEMPTS; attempt++) {
        xEventGroupClearBits(s_events, BIT_STA_GOT_IP | BIT_STA_FAILED);
        esp_wifi_disconnect();
        esp_wifi_connect();
        bits = xEventGroupWaitBits(s_events, BIT_STA_GOT_IP | BIT_STA_FAILED, pdTRUE, pdFALSE,
                                   pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
        if (bits & BIT_STA_GOT_IP) {
            ESP_LOGI(TAG, "joined %s on attempt %d", ssid, attempt);
            break;
        }
        ESP_LOGW(TAG, "attempt %d/%d failed, reason %d", attempt, CONNECT_ATTEMPTS, s_last_reason);
        /* A network that is genuinely not on the air will not appear by being
           asked again; everything else is worth another go. */
        if (s_last_reason == 201) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(600));
    }
    if (!(bits & BIT_STA_GOT_IP)) {
        /* Credentials are only written once they have actually worked. A device
           that saves whatever it was told and then reboots into a network that
           does not exist has to be reflashed to recover. */
        report(WG_PROV_FAILED, ssid);
        /* The radio knows exactly why it failed, and each of these asks
           something different from the person reading it. Saying "check the
           password" when the network was never found sends them to re-type a
           password that was already right. */
        /* One short sentence that says what to do next. The radio knows
           exactly why it failed and each of these needs something different
           from the reader, but a paragraph of network theory in front of
           someone trying to finish setup is worse than useless. */
        const char *why;
        switch (s_last_reason) {
        case 201: /* NO_AP_FOUND */
            why = "Could not find that network. It may be out of range, or 5 GHz only.";
            break;
        case 2:   /* AUTH_EXPIRE */
        case 15:  /* 4WAY_HANDSHAKE_TIMEOUT */
        case 204: /* HANDSHAKE_TIMEOUT */
            why = "That password did not work. Try typing it again.";
            break;
        case 23:  /* 802_1X_AUTH_FAILED */
            why = "That network needs a sign-in page, which the Wedge cannot use.";
            break;
        case 205: /* CONNECTION_FAIL */
            why = "That network would not let the Wedge in. Try another one.";
            break;
        default:
            why = "Could not connect. Try again.";
            break;
        }
        ESP_LOGW(TAG, "join failed, reason %d", s_last_reason);
        return send_portal(req, why);
    }

    store_set_wifi(ssid, pass);
    s_saved = true;
    report(WG_PROV_TRYING, ssid);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<!doctype html><meta charset=utf-8>"
                            "<meta name=viewport content='width=device-width,initial-scale=1'>"
                            "<title>Connected</title>"
                            "<body style='margin:0 auto;max-width:420px;background:#0b0b0f;"
                            "color:#ece9e6;font:17px/1.5 -apple-system,BlinkMacSystemFont,"
                            "system-ui,sans-serif;padding:40px 22px'>"
                            "<h1 style='font-size:26px;font-weight:600;letter-spacing:-.02em;"
                            "margin:0 0 8px'>You are all set</h1>"
                            "<p style='color:#9a958f;margin:0'>You can close this page. "
                            "The Wedge is starting up now.</p>");
    return ESP_OK;
}

bool provision_needed(void)
{
    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    if (store_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) && ssid[0]) {
        return false;
    }
    /* A compiled-in network still counts as provisioned, so a development build
       does not stop to ask. */
    return strlen(CONFIG_WEDGE_WIFI_SSID) == 0 || strcmp(CONFIG_WEDGE_WIFI_SSID, "changeme") == 0;
}

/* On the setup path net_init never runs, so nothing else has registered a
   station handler. Without one the credential test below would wait out its
   whole timeout and call a network that joined perfectly a failure. */
static void on_sta(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        provision_note_sta_event(true);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Kept so the failure page can say what actually went wrong. "Could
           not join that network" is the same sentence whether the password
           was wrong, the network was out of range, or it is an enterprise
           network this radio cannot speak at all, and those need different
           things from the person reading it. */
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_last_reason = d ? d->reason : -1;
        provision_note_sta_event(false);
    }
}

/* Association is silent otherwise: nothing else logs whether a phone actually
   joined the access point, which is the one fact needed to tell "the phone
   never associated" apart from "it associated but never asked this device for
   anything." */
static void on_ap(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != WIFI_EVENT) {
        return;
    }
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "phone associated, mac %02x:%02x:%02x:%02x:%02x:%02x",
                e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "phone left the access point");
    }
}

void provision_note_sta_event(bool got_ip)
{
    if (!s_events) {
        return;
    }
    xEventGroupSetBits(s_events, got_ip ? BIT_STA_GOT_IP : BIT_STA_FAILED);
}

void provision_run(provision_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
    s_events = xEventGroupCreate();

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    /* The last two bytes of the MAC, so two of these in one house are still
       distinguishable on a phone's Wi-Fi list. */
    snprintf(s_ap_name, sizeof(s_ap_name), "Wedge Setup %02X%02X", mac[4], mac[5]);

    /* Two ways in, and they arrive with the radio in different states.

       On a device with no stored network this runs instead of net_init and
       owns the whole stack. On one whose network has disappeared it runs
       alongside a station that is already up and still retrying, so whatever
       is already there must not be built a second time: initialising the event
       loop or the driver again is an error, and aborting here would turn a
       missing router into a boot loop. */
    bool stack_up = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") != NULL;
    if (!stack_up) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();
    }
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    /* The AP's own DHCP server does not hand out a DNS server by default: the
       offer is a flag that ships off, so a phone that joins gets an address
       and a gateway but nothing to resolve names with. Every captive-portal
       probe and the wedge.setup hint both depend on the phone actually asking
       this DNS responder something, so without this nothing after "join the
       network" ever happens. Both calls have to land before esp_wifi_start(),
       which is what brings the DHCP server up. */
    esp_netif_dns_info_t dns = { 0 };
    dns.ip.u_addr.ip4.addr = inet_addr(PORTAL_IP);
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns));
    uint8_t offer_dns = 1;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &offer_dns, sizeof(offer_dns)));

    if (!stack_up) {
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init));
    }
    if (!stack_up) {
        /* net_init registers handlers that already forward these, so a second
           set is only needed when it is not running. */
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                            on_sta, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            on_sta, NULL, NULL));
    }
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                                        on_ap, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                                        on_ap, NULL, NULL));

    wifi_config_t ap = { 0 };
    memcpy(ap.ap.ssid, s_ap_name, strlen(s_ap_name));
    ap.ap.ssid_len = (uint8_t)strlen(s_ap_name);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    /* Open, deliberately. A password on the setup network is one more thing to
       print on a card and mistype, and the only secret that crosses it is about
       to be typed into it anyway. It is up for a few minutes on a home network. */
    ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    /* Already running when the station brought it up, which is not an error. */
    esp_wifi_start();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 4;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_httpd, &cfg) == ESP_OK) {
        const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
        const httpd_uri_t any = { .uri = "/*", .method = HTTP_GET, .handler = any_get };
        httpd_register_uri_handler(s_httpd, &save);
        httpd_register_uri_handler(s_httpd, &any);
    }

    xTaskCreate(dns_task, "prov_dns", 3072, NULL, 4, &s_dns_task);

    report(WG_PROV_WAIT, "");
    ESP_LOGI(TAG, "portal up on %s as \"%s\"", PORTAL_IP, s_ap_name);

    /* Polled independently of the association event as a cross-check: if a
       phone ever appears here without WIFI_EVENT_AP_STACONNECTED having fired,
       the event path itself is the bug rather than the radio link. */
    int last_sta_count = -1;
    for (int elapsed_ms = 0; !s_saved; elapsed_ms += 200) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (elapsed_ms % 2000 == 0) {
            wifi_sta_list_t list;
            if (esp_wifi_ap_get_sta_list(&list) == ESP_OK && list.num != last_sta_count) {
                last_sta_count = list.num;
                ESP_LOGI(TAG, "associated station count: %d", list.num);
            }
        }
    }

    /* Let the success page reach the phone before the radio goes down. */
    vTaskDelay(pdMS_TO_TICKS(1200));
    ESP_LOGI(TAG, "credentials stored, restarting");
    esp_restart();
}
