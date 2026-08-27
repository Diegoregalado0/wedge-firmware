/* The firmware the device should be running.
 *
 * Stored the same way the standing lines are: one document, newest wins, so a
 * crash between writing a new one and deleting the old leaves two valid
 * manifests rather than none. The binary itself goes to blob storage under an
 * unguessable path, which is the same protection the message bodies have.
 */

import { del, list, put } from "@vercel/blob";

const PREFIX = "firmware/";
const blobToken = process.env.BLOB_READ_WRITE_TOKEN;

export interface Manifest {
  version: string;
  url: string;
  sha256: string;
  size: number;
  published_at: number;
}

let memory: Manifest | null = null;

export async function readManifest(): Promise<Manifest | null> {
  if (!blobToken) return memory;
  const { blobs } = await list({ token: blobToken, prefix: PREFIX });
  const docs = blobs.filter((b) => b.pathname.endsWith(".json"));
  if (docs.length === 0) return null;
  const newest = docs.sort((a, b) => (a.uploadedAt < b.uploadedAt ? 1 : -1))[0];
  const res = await fetch(newest.url, { cache: "no-store" });
  if (!res.ok) return null;
  try {
    return (await res.json()) as Manifest;
  } catch {
    return null;
  }
}

export async function publish(binary: ArrayBuffer, version: string): Promise<Manifest> {
  const digest = await crypto.subtle.digest("SHA-256", binary);
  const sha256 = Array.from(new Uint8Array(digest))
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");

  const manifest: Manifest = {
    version,
    url: "",
    sha256,
    size: binary.byteLength,
    published_at: Math.floor(Date.now() / 1000),
  };

  if (!blobToken) {
    memory = { ...manifest, url: "memory://firmware" };
    return memory;
  }

  const bin = await put(`${PREFIX}wedge.bin`, binary, {
    token: blobToken,
    access: "public",
    addRandomSuffix: true,
    contentType: "application/octet-stream",
  });
  manifest.url = bin.url;

  /* The manifest is written after the binary it points at, so a manifest that
   * exists always refers to something the device can actually fetch. */
  const before = await list({ token: blobToken, prefix: PREFIX });
  await put(`${PREFIX}manifest.json`, JSON.stringify(manifest), {
    token: blobToken,
    access: "public",
    addRandomSuffix: true,
    contentType: "application/json",
  });
  /* Only now are the previous ones removed, and only the ones that were there
   * before this publish began. */
  const stale = before.blobs.map((b) => b.url);
  if (stale.length > 0) {
    await del(stale, { token: blobToken }).catch(() => {});
  }
  return manifest;
}
