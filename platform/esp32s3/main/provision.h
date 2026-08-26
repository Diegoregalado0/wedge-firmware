#ifndef PROVISION_H
#define PROVISION_H

#include <stdbool.h>

#include "wedge/app.h"

/* Reports what setup is doing so the panel can say it. Runs on the HTTP task. */
typedef void (*provision_cb_t)(void *ctx, wg_prov_stage_t stage, const char *ap, const char *detail);

/* True when no working network is stored and none is compiled in. */
bool provision_needed(void);

/* Raises the access point and portal, and does not return: it restarts the
   device once credentials have been proven to work. */
void provision_run(provision_cb_t cb, void *ctx);

/* Called from the Wi-Fi event handler so the portal can tell whether the
   credentials it is testing actually connected. */
void provision_note_sta_event(bool got_ip);

#endif
