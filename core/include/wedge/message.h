#ifndef WEDGE_MESSAGE_H
#define WEDGE_MESSAGE_H

#include "wedge/wedge.h"

#define WG_MSG_TEXT_MAX 280
#define WG_MSG_ID_MAX 40
#define WG_MSG_CACHE 8

typedef enum {
    WG_MSG_NORMAL = 0,
    WG_MSG_GOOD_MORNING,
    WG_MSG_GOOD_NIGHT,
    WG_MSG_ENCOURAGEMENT,
    WG_MSG_ADVICE,
    WG_MSG_COMPLIMENT,
    WG_MSG_AFFECTION,
    WG_MSG_SPECIAL_EVENT,
    WG_MSG_TYPE_COUNT,
} wg_msg_type_t;

/* The lifecycle in section 10 of the spec. A message never returns to an
   earlier state, which is what keeps a read message from re-presenting itself. */
typedef enum {
    WG_MSG_STATE_EMPTY = 0,
    WG_MSG_STATE_AVAILABLE,
    WG_MSG_STATE_PENDING,
    WG_MSG_STATE_PRESENTED,
    WG_MSG_STATE_READ,
    WG_MSG_STATE_ARCHIVED,
} wg_msg_state_t;

typedef struct {
    char id[WG_MSG_ID_MAX];
    char text[WG_MSG_TEXT_MAX];
    wg_msg_type_t type;
    wg_msg_state_t state;
    int64_t available_at;
    int64_t expires_at;
    uint8_t priority;
    bool read_ack_pending; /* read locally, not yet acknowledged to the server */
    int64_t read_at;       /* when it was opened, for the keep window */
} wg_message_t;

/* Fixed-capacity cache. An always-on appliance that mallocs per message is an
   appliance that fragments its heap over a year of uptime. */
typedef struct {
    wg_message_t items[WG_MSG_CACHE];
    int count;
} wg_msg_cache_t;

void wg_msg_cache_init(wg_msg_cache_t *c);

/* Insert or update by id. Returns the stored message, or NULL if the cache is
   full of unread messages and the newcomer is not higher priority. */
wg_message_t *wg_msg_cache_put(wg_msg_cache_t *c, const wg_message_t *m);

wg_message_t *wg_msg_cache_find(wg_msg_cache_t *c, const char *id);

/* The message the device should offer next: highest priority among those whose
   available_at has passed and which are not yet read, oldest first on a tie. */
wg_message_t *wg_msg_cache_next(wg_msg_cache_t *c, int64_t now);

int wg_msg_cache_pending(const wg_msg_cache_t *c, int64_t now);

/* The most recently read message, while it is still within its keep window.

   A read message stays addressable and is displaced only by a newer one. The
   window is twelve hours: long enough to span a normal waking day, short
   enough that the cache does not accumulate. */
#define WG_KEEP_SECONDS (12 * 3600)
wg_message_t *wg_msg_cache_kept(wg_msg_cache_t *c, int64_t now);

/* Drop expired and archived entries. Called on the scheduler tick, not on the
   render path. */
void wg_msg_cache_sweep(wg_msg_cache_t *c, int64_t now);

const char *wg_msg_type_name(wg_msg_type_t t);

/* The line shown above the body when a message opens. Morning and night
   messages announce themselves differently from an arbitrary note. */
const char *wg_msg_type_kicker(wg_msg_type_t t);

#endif
