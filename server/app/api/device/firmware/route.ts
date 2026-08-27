/* What the device should be running. Answered with the same bearer token as the
 * message poll, and deliberately says nothing about who published it. */

import { deviceAuthorized } from "@/lib/auth";
import { readManifest } from "@/lib/firmware";

export const dynamic = "force-dynamic";

export async function GET(req: Request) {
  if (!deviceAuthorized(req)) {
    return Response.json({ error: "unauthorized" }, { status: 401 });
  }
  const manifest = await readManifest();
  if (!manifest) {
    /* Nothing published yet is a normal state, not an error: a device that has
     * never been offered an update should not log a failure every day. */
    return Response.json({ version: null }, { headers: { "cache-control": "no-store" } });
  }
  return Response.json(manifest, { headers: { "cache-control": "no-store" } });
}
