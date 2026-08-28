/* Wi-Fi, time, and the backend client.

   Everything here runs on its own task and never touches app state directly.
   Results are posted to a queue the UI task drains, which is the whole reason
   a five second DNS timeout cannot stutter the clock. */

#include "net.h"

#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "provision.h"
#include "store.h"
#include "wedge/app.h"

static const char *TAG = "net";

#define HTTP_BUF 4096

static net_callbacks_t s_cb;
static char s_base[128];
static char s_device_id[40];
static char s_token[96];
static volatile bool s_connected;
static int s_backoff_s = 1;

static esp_timer_handle_t s_retry_timer;
/* When the station last held an address, in milliseconds of uptime. Zero means
   it never has, which is the case for a device carried to a new house. */
static int64_t s_last_up_ms;

static void retry_connect(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* The reason code is the whole diagnosis for a device with no console
           and no keyboard: 201 is NO_AP_FOUND, meaning the stored network is
           simply not here, which looks identical from the outside to a wrong
           password (204/15) or a router refusing the association. Worth one
           line on a path that only runs when something is already wrong. */
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "disconnected, reason %d, retrying in %ds", d ? d->reason : -1, s_backoff_s);
        if (s_connected) {
            s_last_up_ms = (int64_t)(esp_timer_get_time() / 1000);
        }
        s_connected = false;
        provision_note_sta_event(false);
        if (s_cb.on_wifi) {
            s_cb.on_wifi(false);
        }
        /* Exponential backoff, capped, on a timer rather than by sleeping
           here. This runs on the event loop task, and waiting in it stops
           every other Wi-Fi and IP event for the length of the wait: with a
           network that has genuinely gone away the backoff reaches a minute,
           which is long enough to stall the setup access point that gets
           raised in exactly that situation. */
        if (!s_retry_timer) {
            const esp_timer_create_args_t args = { .callback = retry_connect, .name = "wifi_retry" };
            esp_timer_create(&args, &s_retry_timer);
        }
        if (s_retry_timer) {
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, (uint64_t)s_backoff_s * 1000000ULL);
        }
        s_backoff_s = s_backoff_s < 64 ? s_backoff_s * 2 : 64;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_last_up_ms = 0;
        s_backoff_s = 1;
        provision_note_sta_event(true);
        if (s_cb.on_wifi) {
            s_cb.on_wifi(true);
        }
    }
}

static void sntp_synced(struct timeval *tv)
{
    /* Written the moment it is known to be right, so the very next boot starts
       from a real time even if that boot has no network at all. */
    if (tv) {
        store_set_time((int64_t)tv->tv_sec);
    }
    if (s_cb.on_time) {
        s_cb.on_time();
    }
}

void net_apply_sta_defaults(wifi_sta_config_t *sta)
{
    /* Scan every channel and take the strongest radio for this name, rather
       than associating with the first one that answers. This is the whole
       difference on a site where one network name is broadcast by dozens of
       access points: the default fast scan stops at the first hit, which is
       often a distant one that then fails to finish associating, so joining
       appears to work only on the third or fifth attempt. */
    sta->scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta->sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    /* No floor. A weak network is still the network that was chosen. */
    sta->threshold.rssi = -127;
    /* Open is the most permissive floor; the driver raises it by itself when a
       password is supplied, so this does not weaken a secured network. */
    sta->threshold.authmode = WIFI_AUTH_OPEN;
    /* Capable but not demanding: required protected management frames are
       common on newer and campus access points and a station that cannot do
       them is refused, while insisting on them locks out older home routers. */
    sta->pmf_cfg.capable = true;
    sta->pmf_cfg.required = false;
}

esp_err_t net_init(const net_config_t *cfg, const net_callbacks_t *cb)
{
    s_cb = *cb;
    snprintf(s_base, sizeof(s_base), "%s", cfg->base_url);
    snprintf(s_device_id, sizeof(s_device_id), "%s", cfg->device_id);
    snprintf(s_token, sizeof(s_token), "%s", cfg->token);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL));

    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    if (!store_wifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
        snprintf(ssid, sizeof(ssid), "%s", CONFIG_WEDGE_WIFI_SSID);
        snprintf(pass, sizeof(pass), "%s", CONFIG_WEDGE_WIFI_PASSWORD);
    }

    wifi_config_t wc = { 0 };
    memcpy(wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    memcpy(wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    net_apply_sta_defaults(&wc.sta);
    /* The panel stays powered continuously, so modem sleep is the only power
       saving that matters and it costs nothing in responsiveness here. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* server_from_dhcp asks lwIP for a router-provided NTP server, but that
       path is a hard failure unless CONFIG_LWIP_DHCP_GET_NTP_SRV is on, and
       when esp_netif_sntp_init fails it fails whole: the pool.ntp.org server
       set below by ESP_NETIF_SNTP_DEFAULT_CONFIG never gets configured
       either, and the clock never syncs at all. The static server is already
       the one this needs. */
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.sync_cb = sntp_synced;
    sntp.start = true;
    esp_netif_sntp_init(&sntp);

    ESP_LOGI(TAG, "wifi starting, ssid %s", ssid);
    return ESP_OK;
}

bool net_connected(void)
{
    return s_connected;
}

uint32_t net_unreachable_seconds(void)
{
    if (s_connected) {
        return 0;
    }
    /* Measured from the last time it worked, or from boot when it never has,
       which is the case that matters: a device carried to a new house has no
       last-known-good moment to count from. */
    int64_t now = (int64_t)(esp_timer_get_time() / 1000);
    int64_t since = s_last_up_ms > 0 ? s_last_up_ms : 0;
    return (uint32_t)((now - since) / 1000);
}

static wg_msg_type_t type_from(const char *s)
{
    if (!s) {
        return WG_MSG_NORMAL;
    }
    if (!strcmp(s, "good_morning")) return WG_MSG_GOOD_MORNING;
    if (!strcmp(s, "good_night")) return WG_MSG_GOOD_NIGHT;
    if (!strcmp(s, "encouragement")) return WG_MSG_ENCOURAGEMENT;
    if (!strcmp(s, "advice")) return WG_MSG_ADVICE;
    if (!strcmp(s, "compliment")) return WG_MSG_COMPLIMENT;
    if (!strcmp(s, "affection")) return WG_MSG_AFFECTION;
    if (!strcmp(s, "special_event")) return WG_MSG_SPECIAL_EVENT;
    return WG_MSG_NORMAL;
}

/* Reads a whole response into a fixed buffer. Anything larger than the buffer
   is a malformed response from the point of view of this device, and is
   rejected rather than grown into: an appliance that can be made to allocate
   by a reply is an appliance that can be made to run out of memory. */
static esp_err_t fetch(const char *url, char *out, size_t cap, int *status)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_FAIL;
    }
    char auth[128];
    snprintf(auth, sizeof(auth), "Bearer %s", s_token);
    esp_http_client_set_header(c, "Authorization", auth);

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        return err;
    }
    esp_http_client_fetch_headers(c);
    *status = esp_http_client_get_status_code(c);
    int n = esp_http_client_read_response(c, out, (int)cap - 1);
    out[n > 0 ? n : 0] = '\0';
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return n >= 0 ? ESP_OK : ESP_FAIL;
}

net_reach_t net_probe_reachability(void)
{
    if (!s_connected) {
        return NET_REACH_NONE;
    }
    /* Apple's own captive check, over plain HTTP so nothing can fail for
       certificate reasons. An untouched internet returns this exact short
       body; a sign-in page returns its own thing, or a redirect to it. */
    static const char *k_url = "http://captive.apple.com/hotspot-detect.html";
    static const char *k_expect = "Success";

    char buf[512];
    esp_http_client_config_t cfg = {
        .url = k_url,
        .timeout_ms = 6000,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return NET_REACH_NONE;
    }
    net_reach_t out = NET_REACH_NONE;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        int n = esp_http_client_read_response(c, buf, (int)sizeof(buf) - 1);
        buf[n > 0 ? n : 0] = '\0';
        if (status == 200 && strstr(buf, k_expect)) {
            out = NET_REACH_OK;
        } else {
            /* Anything that answers but is not the expected body is something
               standing in the way, which is exactly what a portal is. */
            out = NET_REACH_PORTAL;
            ESP_LOGW(TAG, "captive check answered %d, %d bytes, not the expected body", status, n);
        }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    return out;
}

void net_poll_messages(void)
{
    if (!s_connected) {
        return;
    }
    char *buf = malloc(HTTP_BUF);
    if (!buf) {
        return;
    }
    char url[256];
    snprintf(url, sizeof(url), "%s/api/device/messages?device_id=%s", s_base, s_device_id);

    int status = 0;
    esp_err_t err = fetch(url, buf, HTTP_BUF, &status);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "poll failed, status %d", status);
        if (s_cb.on_poll_failed) {
            s_cb.on_poll_failed();
        }
        free(buf);
        return;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        if (s_cb.on_poll_failed) {
            s_cb.on_poll_failed();
        }
        return;
    }

    cJSON *arr = cJSON_GetObjectItem(root, "messages");
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    ESP_LOGI(TAG, "poll ok, %d messages in response", n);
    if (s_cb.on_poll_ok) {
        s_cb.on_poll_ok();
    }
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        cJSON *id = cJSON_GetObjectItem(it, "id");
        cJSON *text = cJSON_GetObjectItem(it, "text");
        if (!cJSON_IsString(id) || !cJSON_IsString(text)) {
            /* Reject the entry and keep going. One malformed message must not
               cost the other four. */
            ESP_LOGW(TAG, "skipping malformed message at %d", i);
            continue;
        }
        wg_message_t m;
        memset(&m, 0, sizeof(m));
        snprintf(m.id, sizeof(m.id), "%s", id->valuestring);
        snprintf(m.text, sizeof(m.text), "%s", text->valuestring);
        cJSON *type = cJSON_GetObjectItem(it, "type");
        m.type = type_from(cJSON_IsString(type) ? type->valuestring : NULL);
        cJSON *av = cJSON_GetObjectItem(it, "available_at");
        m.available_at = cJSON_IsNumber(av) ? (int64_t)av->valuedouble : 0;
        cJSON *ex = cJSON_GetObjectItem(it, "expires_at");
        m.expires_at = cJSON_IsNumber(ex) ? (int64_t)ex->valuedouble : 0;
        cJSON *pr = cJSON_GetObjectItem(it, "priority");
        m.priority = cJSON_IsNumber(pr) ? (uint8_t)pr->valueint : 1;
        m.state = WG_MSG_STATE_AVAILABLE;
        if (s_cb.on_message) {
            s_cb.on_message(&m);
        }
    }
    cJSON_Delete(root);
}

void net_poll_ambient(void)
{
    if (!s_connected) {
        return;
    }
    char *buf = malloc(HTTP_BUF);
    if (!buf) {
        return;
    }
    char url[256];
    snprintf(url, sizeof(url), "%s/api/device/ambient", s_base);
    int status = 0;
    esp_err_t err = fetch(url, buf, HTTP_BUF, &status);
    if (err != ESP_OK || status != 200) {
        /* Not an outage. The device keeps whatever it already had, which is at
           worst the set compiled into it, so there is always something to say. */
        free(buf);
        return;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return;
    }
    cJSON *arr = cJSON_GetObjectItem(root, "lines");
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    if (n > WG_AMBIENT_MAX) {
        n = WG_AMBIENT_MAX;
    }
    const char *lines[WG_AMBIENT_MAX];
    int count = 0;
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        if (cJSON_IsString(it) && it->valuestring[0]) {
            lines[count++] = it->valuestring;
        }
    }
    if (count > 0 && s_cb.on_ambient) {
        s_cb.on_ambient(lines, count);
    }
    cJSON_Delete(root);
}

bool net_ack_read(const char *id)
{
    if (!s_connected) {
        /* Refused, not discarded. The caller keeps it queued: an acknowledgement
           lost while offline means the server hands the same message back on the
           next poll, and something already read is presented again. */
        return false;
    }
    char url[256];
    snprintf(url, sizeof(url), "%s/api/device/messages/%s/read", s_base, id);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return false;
    }
    char auth[128];
    snprintf(auth, sizeof(auth), "Bearer %s", s_token);
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, "{}", 2);
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ack failed for %s", id);
        return false;
    }
    return status >= 200 && status < 300;
}
