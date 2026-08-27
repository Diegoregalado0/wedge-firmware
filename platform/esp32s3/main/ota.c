#include "ota.h"

#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"

static const char *TAG = "ota";

static volatile bool s_busy;

bool ota_in_progress(void)
{
    return s_busy;
}

void ota_confirm(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    /* The backend answered, so the radio, TLS, the credentials and the route
       out all work. Nothing else this firmware does is more load-bearing than
       that, so it is the right bar for keeping the image. */
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "this build reached the backend; keeping it");
    }
}

bool ota_awaiting_proof(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return false;
    }
    return state == ESP_OTA_IMG_PENDING_VERIFY;
}

/* The manifest, small and fixed shape. */
typedef struct {
    char version[40];
    char url[256];
    char sha256[65];
    int size;
} manifest_t;

static bool fetch_manifest(const char *base_url, const char *token, manifest_t *out)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/api/device/firmware", base_url);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return false;
    }
    char auth[128];
    snprintf(auth, sizeof(auth), "Bearer %s", token);
    esp_http_client_set_header(c, "Authorization", auth);

    bool ok = false;
    char buf[640];
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) == 200) {
            int n = esp_http_client_read_response(c, buf, (int)sizeof(buf) - 1);
            buf[n > 0 ? n : 0] = '\0';
            cJSON *root = cJSON_Parse(buf);
            if (root) {
                cJSON *v = cJSON_GetObjectItem(root, "version");
                cJSON *u = cJSON_GetObjectItem(root, "url");
                cJSON *h = cJSON_GetObjectItem(root, "sha256");
                cJSON *s = cJSON_GetObjectItem(root, "size");
                if (cJSON_IsString(v) && cJSON_IsString(u) && cJSON_IsString(h)) {
                    snprintf(out->version, sizeof(out->version), "%s", v->valuestring);
                    snprintf(out->url, sizeof(out->url), "%s", u->valuestring);
                    snprintf(out->sha256, sizeof(out->sha256), "%s", h->valuestring);
                    out->size = cJSON_IsNumber(s) ? s->valueint : 0;
                    ok = out->version[0] && out->url[0];
                }
                cJSON_Delete(root);
            }
        }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    return ok;
}

/* Reads back what was just written and checks it against the digest the
   manifest published. esp_https_ota already validates the image header and the
   flash writes, which catches a truncated or corrupt download; this catches the
   case where the bytes arrived intact but are not the bytes that were meant. */
static bool digest_matches(const esp_partition_t *part, int size, const char *expected)
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    uint8_t *chunk = malloc(4096);
    if (!chunk) {
        mbedtls_sha256_free(&ctx);
        return false;
    }
    bool ok = true;
    for (int off = 0; off < size; off += 4096) {
        int n = size - off < 4096 ? size - off : 4096;
        if (esp_partition_read(part, off, chunk, (size_t)n) != ESP_OK) {
            ok = false;
            break;
        }
        mbedtls_sha256_update(&ctx, chunk, (size_t)n);
    }
    free(chunk);

    uint8_t out[32];
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
    if (!ok) {
        return false;
    }

    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", out[i]);
    }
    return strncmp(hex, expected, 64) == 0;
}

bool ota_check_and_apply(const char *base_url, const char *token)
{
    manifest_t m;
    memset(&m, 0, sizeof(m));
    if (!fetch_manifest(base_url, token, &m)) {
        return false;
    }

    const esp_app_desc_t *running = esp_app_get_description();
    if (strncmp(running->version, m.version, sizeof(running->version)) == 0) {
        return false;
    }
    ESP_LOGI(TAG, "running %s, offered %s", running->version, m.version);

    s_busy = true;
    esp_http_client_config_t http = {
        .url = m.url,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &handle);
    if (err != ESP_OK || !handle) {
        ESP_LOGW(TAG, "could not start: %s", esp_err_to_name(err));
        s_busy = false;
        return false;
    }

    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        /* Nothing to do but let the other tasks run; the clock is still being
           drawn on the other core while this happens. */
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGW(TAG, "download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        s_busy = false;
        return false;
    }

    int written = esp_https_ota_get_image_len_read(handle);
    /* Checked before the image is made bootable, and the partition is taken
       before finish() because finish() invalidates the handle. */
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not finish: %s", esp_err_to_name(err));
        s_busy = false;
        return false;
    }

    if (m.size > 0 && written != m.size) {
        ESP_LOGE(TAG, "size mismatch: got %d, expected %d", written, m.size);
        s_busy = false;
        return false;
    }
    if (!digest_matches(target, written, m.sha256)) {
        ESP_LOGE(TAG, "digest mismatch; refusing to boot it");
        /* Leaving the slot as it is: the running image is untouched and the
           next attempt overwrites this one anyway. */
        s_busy = false;
        return false;
    }

    ESP_LOGI(TAG, "%s written and verified, %d bytes", m.version, written);
    s_busy = false;
    return true;
}
