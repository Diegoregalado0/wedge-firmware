#ifndef OTA_H
#define OTA_H

#include <stdbool.h>

/* Firmware updates over the air.
 *
 * The device is a gift. Once it is on someone else's bedside table, a fix that
 * needs a cable is a fix that does not happen, so it has to be able to update
 * itself. The hazard is the obvious one: a bad image on a device nobody can
 * reach is worse than no update mechanism at all.
 *
 * What stops that is the bootloader's rollback. A freshly written image boots
 * once as pending, and unless the firmware itself declares it healthy the next
 * restart goes back to the slot it came from. This calls ota_confirm only
 * after the new build has reached the backend, which is the narrowest useful
 * definition of working: it means the radio, the TLS stack, the credentials
 * and the network path all still function. A build that cannot do that undoes
 * itself without anyone being told, which is exactly what should happen. */

/* Checks for and applies an update. Blocks; call from the network task, never
   from the one drawing frames. Returns true if an image was written, in which
   case the caller should restart when nothing is on screen. */
bool ota_check_and_apply(const char *base_url, const char *token);

/* Declares the running image healthy, cancelling the pending rollback. Safe to
   call on every successful poll: it does nothing once already confirmed. */
void ota_confirm(void);

/* True while an update is being downloaded, so the rest of the device can stay
   out of the way. */
bool ota_in_progress(void);

/* True when the running image is on trial and has not yet proved itself.
   Rollback only happens at boot, so an image that cannot reach the backend
   would otherwise sit there broken forever, having never been confirmed and
   having no reason to restart. The caller gives it a while and then restarts
   it, which is what lets the bootloader put the working one back. */
bool ota_awaiting_proof(void);

#endif
