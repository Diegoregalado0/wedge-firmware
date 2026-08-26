#ifndef STORE_H
#define STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t store_init(void);

/* Blob persistence for the app's own state. Writes go through NVS, which is
   already transactional at the page level, so a power cut during a write
   leaves the previous value intact rather than a half-written one. */
void store_save(const void *blob, size_t len);
size_t store_load(void *blob, size_t len);

/* Wi-Fi credentials, kept in a separate namespace from general configuration
   so a config reset does not take the network down with it. */
bool store_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
void store_set_wifi(const char *ssid, const char *pass);

/* Erases the stored network so the device comes back up into setup. */
void store_clear_wifi(void);

/* Last known wall-clock time. There is no battery-backed clock on this board,
   so without this every power cut puts the device back in 1970 and the panel
   shows a confidently wrong hour until the network answers. Restoring the last
   value means the clock is approximately right from the first frame and exact
   a few seconds later. */
bool store_time(int64_t *out);
void store_set_time(int64_t t);

#endif
