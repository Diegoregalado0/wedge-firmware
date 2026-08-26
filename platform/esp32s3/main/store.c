/* NVS-backed persistence. Section 40 asks that a power loss not corrupt the
   active configuration; NVS is already log-structured and page-atomic, so the
   requirement is met by using it correctly rather than by adding a journal. */

#include "store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "store";

#define NS_STATE "wedge"
#define NS_WIFI "wedge_wifi"

esp_err_t store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* A partition that cannot be opened is erased and rebuilt rather than
           left to fail every boot. Losing cached messages is recoverable; a
           device that will not start is not. */
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

void store_save(const void *blob, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NS_STATE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, "state", blob, len) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

size_t store_load(void *blob, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NS_STATE, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    size_t n = len;
    if (nvs_get_blob(h, "state", blob, &n) != ESP_OK) {
        n = 0;
    }
    nvs_close(h);
    return n;
}

bool store_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NS_WIFI, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_str(h, "ssid", ssid, &ssid_len) == ESP_OK &&
              nvs_get_str(h, "pass", pass, &pass_len) == ESP_OK;
    nvs_close(h);
    return ok;
}

void store_set_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NS_WIFI, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "wifi credentials stored");
}

bool store_time(int64_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NS_STATE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    int64_t v = 0;
    bool ok = nvs_get_i64(h, "clock", &v) == ESP_OK;
    nvs_close(h);
    if (ok) {
        *out = v;
    }
    return ok;
}

void store_set_time(int64_t t)
{
    nvs_handle_t h;
    if (nvs_open(NS_STATE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_i64(h, "clock", t) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

void store_clear_wifi(void)
{
    nvs_handle_t h;
    if (nvs_open(NS_WIFI, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "wifi credentials erased");
}
