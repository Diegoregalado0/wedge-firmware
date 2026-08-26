/* The device's copy of the standing lines, fetched on the same schedule as
   messages. It falls back to its own built-in set if this ever fails, so this
   endpoint being down is not an outage. */

import { deviceAuthorized } from "@/lib/auth";
import { readLines } from "@/lib/ambient";

export const dynamic = "force-dynamic";

export async function GET(req: Request) {
  if (!deviceAuthorized(req)) {
    return Response.json({ error: "unauthorized" }, { status: 401 });
  }
  return Response.json({ lines: await readLines() }, { headers: { "cache-control": "no-store" } });
}
