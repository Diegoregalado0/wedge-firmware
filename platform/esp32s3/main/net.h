#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"
#include "wedge/message.h"

typedef struct {
    const char *base_url;
    const char *device_id;
    const char *token;
} net_config_t;

/* Callbacks run on the network task. They must only enqueue, never draw. */
typedef struct {
    void (*on_wifi)(bool up);
    void (*on_time)(void);
    void (*on_message)(const wg_message_t *m);
    void (*on_poll_failed)(void);
    /* Fired when a poll actually reached the backend, which is the only proof
       the connection carries traffic rather than merely having an address. */
    void (*on_poll_ok)(void);
    /* The standing lines, replaced wholesale. */
    void (*on_ambient)(const char *const *lines, int count);
} net_callbacks_t;

/* Fills in everything about joining a network that is not the name and the
   password. Shared with setup so the network that was tested during setup and
   the network joined on every boot afterwards are joined the same way; they
   drifted apart once already and the symptom was a device that provisioned
   fine and then would not reconnect. */
void net_apply_sta_defaults(wifi_sta_config_t *sta);

/* What the connection can actually carry, as opposed to whether it has an
   address. Answered with plain HTTP on purpose: a TLS failure cannot tell a
   sign-in page apart from a dead route, because both look like a handshake
   that never completed. */
typedef enum {
    NET_REACH_OK = 0,   /* reached the internet unmodified */
    NET_REACH_PORTAL,   /* something answered, but not what was asked for */
    NET_REACH_NONE,     /* nothing answered at all */
} net_reach_t;

net_reach_t net_probe_reachability(void);

esp_err_t net_init(const net_config_t *cfg, const net_callbacks_t *cb);
bool net_connected(void);

/* How long the stored network has been unreachable, in seconds, or 0 while it
   is joined. A router that was replaced looks exactly like one that is briefly
   off, and only the passage of time separates them. */
uint32_t net_unreachable_seconds(void);
void net_poll_messages(void);
void net_poll_ambient(void);
/* Returns false if the acknowledgement could not be delivered, in which case
   the caller must keep it and try again. */
bool net_ack_read(const char *id);

#endif
