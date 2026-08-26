/* Message storage.
 *
 * Three backends, chosen by what the environment provides:
 *
 *   Upstash Redis  if KV_REST_API_* is set. Preferred: atomic per-field writes.
 *   Vercel Blob    otherwise. One blob per message, so a read acknowledgement
 *                  and a newly sent message never touch the same object and
 *                  cannot lose each other's write.
 *   In-process map when neither is present, so `next dev` runs unprovisioned.
 *
 * Blob is public-access, which means a message body is readable by anyone
 * holding its exact URL. Those URLs carry a 30-character random suffix, are
 * never returned to a client, and are only reachable through the authenticated
 * routes in app/api. That is unguessable rather than access-controlled, and it
 * is the honest description of the tradeoff: switching to Redis removes it.
 */

import { del, list, put } from "@vercel/blob";

import type { Message, MessageInput } from "./types";

const REDIS_KEY = "wedge:messages";
const BLOB_PREFIX = "messages/";

const redisUrl = process.env.KV_REST_API_URL ?? process.env.UPSTASH_REDIS_REST_URL;
const redisToken = process.env.KV_REST_API_TOKEN ?? process.env.UPSTASH_REDIS_REST_TOKEN;
const blobToken = process.env.BLOB_READ_WRITE_TOKEN;

type Backend = "redis" | "blob" | "memory";

export const backend: Backend = redisUrl && redisToken ? "redis" : blobToken ? "blob" : "memory";

const memory = new Map<string, Message>();

async function redis(command: unknown[]): Promise<unknown> {
  const res = await fetch(redisUrl!, {
    method: "POST",
    headers: { Authorization: `Bearer ${redisToken}`, "Content-Type": "application/json" },
    body: JSON.stringify(command),
    cache: "no-store",
  });
  if (!res.ok) {
    throw new Error(`redis ${res.status}: ${await res.text()}`);
  }
  return ((await res.json()) as { result: unknown }).result;
}

function parse(raw: string): Message | null {
  try {
    return JSON.parse(raw) as Message;
  } catch {
    /* A single corrupt row must not take down the list. Section 41 asks that an
       invalid message be rejected and logged, not that it be fatal. */
    return null;
  }
}

/* Blob pathnames are `messages/{id}-{random}.json`, so a message can be found
   by prefix without keeping a separate index that could drift out of sync. */
function idFromPathname(pathname: string): string {
  const name = pathname.slice(BLOB_PREFIX.length).replace(/\.json$/, "");
  const cut = name.lastIndexOf("-");
  return cut === -1 ? name : name.slice(0, cut);
}

async function blobEntries(): Promise<{ id: string; url: string }[]> {
  const { blobs } = await list({ token: blobToken, prefix: BLOB_PREFIX });
  return blobs.map((b) => ({ id: idFromPathname(b.pathname), url: b.url }));
}

export async function listAll(): Promise<Message[]> {
  if (backend === "memory") {
    return [...memory.values()];
  }
  if (backend === "redis") {
    const raw = (await redis(["HVALS", REDIS_KEY])) as string[] | null;
    return (raw ?? []).map(parse).filter((m): m is Message => m !== null);
  }

  const entries = await blobEntries();
  const bodies = await Promise.all(
    entries.map(async (e) => {
      const res = await fetch(e.url, { cache: "no-store" });
      return res.ok ? parse(await res.text()) : null;
    }),
  );
  return bodies.filter((m): m is Message => m !== null);
}

export async function get(id: string): Promise<Message | null> {
  if (backend === "memory") {
    return memory.get(id) ?? null;
  }
  if (backend === "redis") {
    const raw = (await redis(["HGET", REDIS_KEY, id])) as string | null;
    return raw ? parse(raw) : null;
  }
  const entry = (await blobEntries()).find((e) => e.id === id);
  if (!entry) return null;
  const res = await fetch(entry.url, { cache: "no-store" });
  return res.ok ? parse(await res.text()) : null;
}

export async function put_(message: Message): Promise<void> {
  if (backend === "memory") {
    memory.set(message.id, message);
    return;
  }
  if (backend === "redis") {
    await redis(["HSET", REDIS_KEY, message.id, JSON.stringify(message)]);
    return;
  }
  /* Blob has no in-place update, so an edit writes a new object and deletes the
     old one. The write happens first: losing the previous copy before the new
     one exists would lose the message outright. */
  const stale = (await blobEntries()).filter((e) => e.id === message.id).map((e) => e.url);
  await put(`${BLOB_PREFIX}${message.id}.json`, JSON.stringify(message), {
    access: "public",
    token: blobToken,
    contentType: "application/json",
    addRandomSuffix: true,
  });
  if (stale.length > 0) {
    await del(stale, { token: blobToken });
  }
}

export { put_ as put };

export async function remove(id: string): Promise<void> {
  if (backend === "memory") {
    memory.delete(id);
    return;
  }
  if (backend === "redis") {
    await redis(["HDEL", REDIS_KEY, id]);
    return;
  }
  const urls = (await blobEntries()).filter((e) => e.id === id).map((e) => e.url);
  if (urls.length > 0) {
    await del(urls, { token: blobToken });
  }
}

export function create(input: MessageInput): Message {
  const now = Math.floor(Date.now() / 1000);
  return {
    id: crypto.randomUUID().replace(/-/g, "").slice(0, 16),
    text: input.text.trim(),
    type: input.type,
    priority: input.priority ?? 1,
    created_at: now,
    available_at: input.available_at ?? now,
    expires_at: input.expires_at ?? 0,
    status: "available",
    read_at: null,
  };
}
