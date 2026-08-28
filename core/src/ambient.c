/* The standing lines.
 *
 * Shown when no message is pending, which is the device's normal resting
 * state. Held in a fixed-capacity bank rather than a static table so the set
 * can be edited from the service and survives power loss. Every mutation
 * writes through to the host immediately: a bank that relies on the caller to
 * save is one that loses an edit on the first unclean shutdown.
 */

#include "wedge/app.h"

#include <stdio.h>
#include <string.h>

/* Shipped defaults, intended to be replaced through the service. Present so a
   unit that has never been configured still has something to display. */
static const char *k_defaults[] = {
    "Small steps still count as moving.",
    "You can begin again at any hour.",
    "Rest is part of the work, not a break from it.",
    "Not every day has to be a good one.",
    "Progress is quieter than people expect.",
    "Be gentle with the part of you still learning.",
    "You are allowed to change your mind.",
    "The hard part usually passes.",
    "Attention is a rare kind of generosity.",
    "Do the next small thing.",
    "Notice one good thing today.",
    "You have made it through every day so far.",
};

void wg_ambient_defaults(wg_ambient_bank_t *b)
{
    memset(b, 0, sizeof(*b));
    int n = (int)(sizeof(k_defaults) / sizeof(k_defaults[0]));
    if (n > WG_AMBIENT_MAX) {
        n = WG_AMBIENT_MAX;
    }
    for (int i = 0; i < n; i++) {
        snprintf(b->lines[i], WG_AMBIENT_TEXT, "%s", k_defaults[i]);
    }
    b->count = n;
}

int wg_ambient_count(const wg_app_t *a)
{
    return a->ambient.count;
}

const char *wg_ambient_at(const wg_app_t *a, int index)
{
    if (index < 0 || index >= a->ambient.count) {
        return "";
    }
    return a->ambient.lines[index];
}

/* Persisted layout. Versioned and magic-tagged so a blob written by an older
   build, or a partially erased one, is rejected rather than read as lines. */
#define WG_PERSIST_MAGIC 0x57454447u /* "WEDG" */
#define WG_PERSIST_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    char lines[WG_AMBIENT_MAX][WG_AMBIENT_TEXT];
} wg_persist_t;

static void persist(wg_app_t *a)
{
    if (!a->host.persist) {
        return;
    }
    wg_persist_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic = WG_PERSIST_MAGIC;
    blob.version = WG_PERSIST_VERSION;
    blob.count = (uint16_t)a->ambient.count;
    memcpy(blob.lines, a->ambient.lines, sizeof(blob.lines));
    a->host.persist(a->host.ctx, &blob, sizeof(blob));
}

void wg_ambient_restore(wg_app_t *a)
{
    wg_ambient_defaults(&a->ambient);
    if (!a->host.restore) {
        return;
    }
    wg_persist_t blob;
    size_t n = a->host.restore(a->host.ctx, &blob, sizeof(blob));
    if (n != sizeof(blob) || blob.magic != WG_PERSIST_MAGIC ||
        blob.version != WG_PERSIST_VERSION || blob.count > WG_AMBIENT_MAX) {
        return;
    }
    /* An empty stored bank is not honoured. Something must always be on the
       screen, and a bank emptied by accident should not leave it blank. */
    if (blob.count == 0) {
        return;
    }
    for (int i = 0; i < blob.count; i++) {
        blob.lines[i][WG_AMBIENT_TEXT - 1] = '\0';
    }
    memcpy(a->ambient.lines, blob.lines, sizeof(blob.lines));
    a->ambient.count = blob.count;
}

static bool blank(const char *text)
{
    if (!text) {
        return true;
    }
    for (const char *p = text; *p; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\n') {
            return false;
        }
    }
    return true;
}

bool wg_ambient_set(wg_app_t *a, int index, const char *text)
{
    if (index < 0 || index >= a->ambient.count || blank(text)) {
        return false;
    }
    snprintf(a->ambient.lines[index], WG_AMBIENT_TEXT, "%s", text);
    persist(a);
    return true;
}

bool wg_ambient_add(wg_app_t *a, const char *text)
{
    if (a->ambient.count >= WG_AMBIENT_MAX || blank(text)) {
        return false;
    }
    snprintf(a->ambient.lines[a->ambient.count], WG_AMBIENT_TEXT, "%s", text);
    a->ambient.count++;
    persist(a);
    return true;
}

bool wg_ambient_remove(wg_app_t *a, int index)
{
    /* The last line cannot be removed. The device is defined by having
       something to say, and an empty bank is a blank strip of glass. */
    if (index < 0 || index >= a->ambient.count || a->ambient.count <= 1) {
        return false;
    }
    for (int i = index; i < a->ambient.count - 1; i++) {
        memcpy(a->ambient.lines[i], a->ambient.lines[i + 1], WG_AMBIENT_TEXT);
    }
    a->ambient.count--;
    memset(a->ambient.lines[a->ambient.count], 0, WG_AMBIENT_TEXT);
    persist(a);
    return true;
}

bool wg_ambient_replace(wg_app_t *a, const char *const *lines, int count)
{
    if (!lines || count <= 0) {
        return false;
    }
    if (count > WG_AMBIENT_MAX) {
        count = WG_AMBIENT_MAX;
    }
    /* Built in a scratch bank and swapped in only once it is known good, so a
       reply containing one blank line cannot leave the device holding a
       half-written set. */
    wg_ambient_bank_t next;
    memset(&next, 0, sizeof(next));
    for (int i = 0; i < count; i++) {
        if (blank(lines[i])) {
            continue;
        }
        snprintf(next.lines[next.count], WG_AMBIENT_TEXT, "%s", lines[i]);
        next.count++;
    }
    if (next.count == 0) {
        return false;
    }
    a->ambient = next;
    persist(a);
    return true;
}

void wg_ambient_reset(wg_app_t *a)
{
    wg_ambient_defaults(&a->ambient);
    persist(a);
}
