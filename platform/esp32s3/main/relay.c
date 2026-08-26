#include "relay.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "relay";

/* Nothing else reports whether a phone actually arrived, which is the single
   fact that separates "the person has not joined yet" from "they joined and
   the page would not load". */
static void on_ap_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != WIFI_EVENT) {
        return;
    }
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "a phone joined; its traffic now leaves as this device");
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "the phone left");
    }
}

static esp_netif_t *s_ap;
static char s_ap_name[32];
static bool s_active;

bool relay_active(void)
{
    return s_active;
}

const char *relay_ap_name(void)
{
    return s_ap_name;
}

esp_err_t relay_start(void)
{
    if (s_active) {
        return ESP_OK;
    }

    /* The upstream resolver, taken from the network the station is already on.
       This is the one thing that must not be answered locally: setup spoofs
       every name to itself so a phone lands on the Wedge's own page, and doing
       that here would send the phone to the Wedge instead of to the portal it
       actually needs to reach. */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_dns_info_t dns = { 0 };
    if (!sta || esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK ||
        dns.ip.u_addr.ip4.addr == 0) {
        ESP_LOGW(TAG, "no upstream resolver yet, not lending the connection");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_ap) {
        s_ap = esp_netif_create_default_wifi_ap();
        if (!s_ap) {
            return ESP_FAIL;
        }
    }

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    /* Deliberately not "Wedge Setup": that name belongs to first-time setup,
       where the Wedge serves its own page and answers every name itself. This
       one does the opposite, so sharing a name would mean the same words on a
       phone meaning two different things. */
    snprintf(s_ap_name, sizeof(s_ap_name), "Wedge Sign-in %02X%02X", mac[4], mac[5]);

    /* Both of these have to land before the interface comes up, because the
       DHCP server refuses option changes once it is running. */
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &dns));
    uint8_t offer_dns = 1;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &offer_dns, sizeof(offer_dns)));

    wifi_config_t ap = { 0 };
    memcpy(ap.ap.ssid, s_ap_name, strlen(s_ap_name));
    ap.ap.ssid_len = (uint8_t)strlen(s_ap_name);
    ap.ap.max_connection = 2;
    /* Open, for the same reason setup is: the only thing crossing this network
       is a sign-in the person is about to type anyway, and a password on it is
       one more thing to get wrong while standing in front of a broken clock.
       The radio is already on the channel the station is using; a soft access
       point cannot choose its own while the station is associated. */
    ap.ap.authmode = WIFI_AUTH_OPEN;

    static bool handlers_added;
    if (!handlers_added) {
        handlers_added = true;
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                            on_ap_event, NULL, NULL);
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                            on_ap_event, NULL, NULL);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    /* The whole point: packets arriving on the access point leave through the
       station with the Wedge's own source address, so the portal is looking at
       this device and not at the phone that is doing the tapping. */
    esp_err_t err = esp_netif_napt_enable(s_ap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not enable routing: %s", esp_err_to_name(err));
        esp_wifi_set_mode(WIFI_MODE_STA);
        return err;
    }

    s_active = true;
    esp_netif_ip_info_t sta_ip = { 0 };
    esp_netif_get_ip_info(sta, &sta_ip);
    ESP_LOGI(TAG, "lending the connection as \"%s\"", s_ap_name);
    ESP_LOGI(TAG, "  handing out resolver " IPSTR ", routing out via " IPSTR,
             IP2STR(&dns.ip.u_addr.ip4), IP2STR(&sta_ip.ip));
    return ESP_OK;
}

void relay_stop(void)
{
    if (!s_active) {
        return;
    }
    esp_netif_napt_disable(s_ap);
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_active = false;
    ESP_LOGI(TAG, "connection is working, access point down");
}
