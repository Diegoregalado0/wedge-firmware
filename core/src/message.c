#include "wedge/message.h"

#include <string.h>

void wg_msg_cache_init(wg_msg_cache_t *c)
{
    memset(c, 0, sizeof(*c));
}

wg_message_t *wg_msg_cache_find(wg_msg_cache_t *c, const char *id)
{
    for (int i = 0; i < c->count; i++) {
        if (c->items[i].state != WG_MSG_STATE_EMPTY && strncmp(c->items[i].id, id, WG_MSG_ID_MAX) == 0) {
            return &c->items[i];
        }
    }
    return NULL;
}

wg_message_t *wg_msg_cache_put(wg_msg_cache_t *c, const wg_message_t *m)
{
    wg_message_t *slot = wg_msg_cache_find(c, m->id);
    if (slot) {
        /* Re-delivery of a message the device has already shown must not undo
           the read state, or a flaky poll would replay it every five minutes. */
        wg_msg_state_t keep = slot->state;
        bool ack = slot->read_ack_pending;
        *slot = *m;
        if (keep > m->state) {
            slot->state = keep;
            slot->read_ack_pending = ack;
        }
        return slot;
    }

    if (c->count < WG_MSG_CACHE) {
        slot = &c->items[c->count++];
        *slot = *m;
        return slot;
    }

    /* Full: evict the lowest-priority read or archived entry, and failing that
       refuse the newcomer rather than dropping something not yet seen. */
    wg_message_t *victim = NULL;
    for (int i = 0; i < c->count; i++) {
        wg_message_t *it = &c->items[i];
        if (it->state == WG_MSG_STATE_READ || it->state == WG_MSG_STATE_ARCHIVED) {
            if (!victim || it->priority < victim->priority) {
                victim = it;
            }
        }
    }
    if (!victim) {
        return NULL;
    }
    *victim = *m;
    return victim;
}

wg_message_t *wg_msg_cache_next(wg_msg_cache_t *c, int64_t now)
{
    wg_message_t *best = NULL;
    for (int i = 0; i < c->count; i++) {
        wg_message_t *it = &c->items[i];
        if (it->state != WG_MSG_STATE_AVAILABLE && it->state != WG_MSG_STATE_PENDING) {
            continue;
        }
        if (it->available_at > now) {
            continue;
        }
        if (it->expires_at > 0 && it->expires_at <= now) {
            continue;
        }
        if (!best || it->priority > best->priority ||
            (it->priority == best->priority && it->available_at < best->available_at)) {
            best = it;
        }
    }
    return best;
}

int wg_msg_cache_pending(const wg_msg_cache_t *c, int64_t now)
{
    int n = 0;
    for (int i = 0; i < c->count; i++) {
        const wg_message_t *it = &c->items[i];
        if ((it->state == WG_MSG_STATE_AVAILABLE || it->state == WG_MSG_STATE_PENDING) &&
            it->available_at <= now && (it->expires_at <= 0 || it->expires_at > now)) {
            n++;
        }
    }
    return n;
}

wg_message_t *wg_msg_cache_kept(wg_msg_cache_t *c, int64_t now)
{
    wg_message_t *best = NULL;
    for (int i = 0; i < c->count; i++) {
        wg_message_t *it = &c->items[i];
        if (it->state != WG_MSG_STATE_READ) {
            continue;
        }
        if (it->read_at <= 0 || now - it->read_at > WG_KEEP_SECONDS) {
            continue;
        }
        if (!best || it->read_at > best->read_at) {
            best = it;
        }
    }
    return best;
}

void wg_msg_cache_sweep(wg_msg_cache_t *c, int64_t now)
{
    int w = 0;
    for (int i = 0; i < c->count; i++) {
        wg_message_t *it = &c->items[i];
        bool expired = it->expires_at > 0 && it->expires_at <= now && it->state != WG_MSG_STATE_READ;
        bool done = it->state == WG_MSG_STATE_ARCHIVED;
        /* A read message is kept for a day so it can be returned to, then it
           retires on its own without ever having been dismissed by hand. */
        if (it->state == WG_MSG_STATE_READ && it->read_at > 0 && now - it->read_at > WG_KEEP_SECONDS) {
            done = true;
        }
        /* A read message with an unsent acknowledgement stays until the server
           has been told, otherwise it comes back on the next poll. */
        if ((expired || done) && !it->read_ack_pending) {
            continue;
        }
        if (w != i) {
            c->items[w] = *it;
        }
        w++;
    }
    c->count = w;
}

const char *wg_msg_type_name(wg_msg_type_t t)
{
    switch (t) {
    case WG_MSG_GOOD_MORNING: return "good_morning";
    case WG_MSG_GOOD_NIGHT: return "good_night";
    case WG_MSG_ENCOURAGEMENT: return "encouragement";
    case WG_MSG_ADVICE: return "advice";
    case WG_MSG_COMPLIMENT: return "compliment";
    case WG_MSG_AFFECTION: return "affection";
    case WG_MSG_SPECIAL_EVENT: return "special_event";
    default: return "normal";
    }
}

const char *wg_msg_type_kicker(wg_msg_type_t t)
{
    switch (t) {
    case WG_MSG_GOOD_MORNING: return "GOOD MORNING";
    case WG_MSG_GOOD_NIGHT: return "GOODNIGHT";
    case WG_MSG_ENCOURAGEMENT: return "FOR TODAY";
    case WG_MSG_ADVICE: return "A THOUGHT";
    case WG_MSG_COMPLIMENT: return "SOMETHING TRUE";
    case WG_MSG_AFFECTION: return "FROM DIEGO";
    case WG_MSG_SPECIAL_EVENT: return "TODAY";
    default: return "FOR YOU";
    }
}
