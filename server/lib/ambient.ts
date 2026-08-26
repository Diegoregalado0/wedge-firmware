/* The standing lines the device shows when nothing has been sent.
 *
 * Stored as one document rather than one blob per line: the device replaces the
 * whole bank in a single write, so a partial set is never a valid state and
 * there is nothing to reconcile. */

import { del, list, put } from "@vercel/blob";

const PREFIX = "ambient/";
const blobToken = process.env.BLOB_READ_WRITE_TOKEN;

export const AMBIENT_MAX = 12;
export const AMBIENT_TEXT_MAX = 95; /* the device's buffer, less its terminator */

/* Mirrors core/src/ambient.c. Kept in sync by hand, which is fine because the
   device falls back to its own copy whenever the network has nothing to say. */
export const DEFAULT_LINES = [
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
];

let memory: string[] | null = null;

export function sanitize(input: unknown): string[] {
  if (!Array.isArray(input)) return [];
  return input
    .filter((l): l is string => typeof l === "string")
    .map((l) => l.trim().replace(/\s+/g, " "))
    .filter((l) => l.length > 0)
    .map((l) => l.slice(0, AMBIENT_TEXT_MAX))
    .slice(0, AMBIENT_MAX);
}

export async function readLines(): Promise<string[]> {
  if (!blobToken) {
    return memory ?? DEFAULT_LINES;
  }
  const { blobs } = await list({ token: blobToken, prefix: PREFIX });
  if (blobs.length === 0) return DEFAULT_LINES;
  /* Newest wins. Writes add a new object before deleting the old one, so a
     crash between the two leaves two valid documents rather than none. */
  const newest = blobs.sort((a, b) => (a.uploadedAt < b.uploadedAt ? 1 : -1))[0];
  const res = await fetch(newest.url, { cache: "no-store" });
  if (!res.ok) return DEFAULT_LINES;
  try {
    const parsed = sanitize(JSON.parse(await res.text()));
    return parsed.length > 0 ? parsed : DEFAULT_LINES;
  } catch {
    return DEFAULT_LINES;
  }
}

export async function writeLines(lines: string[]): Promise<string[]> {
  const clean = sanitize(lines);
  if (clean.length === 0) {
    throw new Error("at least one line is required");
  }
  if (!blobToken) {
    memory = clean;
    return clean;
  }
  const { blobs } = await list({ token: blobToken, prefix: PREFIX });
  await put(`${PREFIX}lines.json`, JSON.stringify(clean), {
    access: "public",
    token: blobToken,
    contentType: "application/json",
    addRandomSuffix: true,
  });
  if (blobs.length > 0) {
    await del(blobs.map((b) => b.url), { token: blobToken });
  }
  return clean;
}
