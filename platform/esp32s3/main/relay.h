#ifndef RELAY_H
#define RELAY_H

#include <stdbool.h>

#include "esp_err.h"

/* Lending the Wedge's connection to a phone, so a captive portal can be
   signed in to on its behalf.

   Some networks hand out an address and then refuse to carry anything until a
   person accepts terms or signs in on a web page. The Wedge has no browser and
   no keyboard, so it cannot do that for itself, and it cannot simply ask a
   phone to do it either: these portals authorise a specific device, and a
   phone signing in on its own connection authorises only the phone.

   So the traffic has to genuinely come from the Wedge. It raises its access
   point alongside the network it just joined and routes the phone's packets
   out through its own station interface. The portal sees the Wedge's address
   and hardware address, the person taps whatever the page asks for, and the
   session that gets opened belongs to the Wedge. */

esp_err_t relay_start(void);
void relay_stop(void);
bool relay_active(void);

/* The name of the access point to join, valid once relay_start has succeeded. */
const char *relay_ap_name(void);

#endif
