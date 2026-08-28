/* The device's poll endpoint. Called every five minutes, so it must be cheap
 * and must never fail in a way that leaves the appliance without its cache. */

import { deviceAuthorized } from "@/lib/auth";
import { listAll, put } from "@/lib/store";
import { toDevice } from "@/lib/types";

export const dynamic = "force-dynamic";

export async function GET(req: Request) {
  if (!deviceAuthorized(req)) {
    return Response.json({ error: "unauthorized" }, { status: 401 });
  }

  const now = Math.floor(Date.now() / 1000);
  const all = await listAll();
  const pending = all
    .filter((m) => m.status !== "read")
    .filter((m) => m.expires_at === 0 || m.expires_at > now)
    /* Scheduled messages are sent ahead of their hour so the device holds them
     * locally and can present them even if the network is down at the moment
     * they come due. A goodnight message must not depend on Wi-Fi at 21:00. */
    .sort((a, b) => b.priority - a.priority || a.available_at - b.available_at)
    .slice(0, 8);

  /* Handing a message over is the moment it stops being in the post and starts
   * being on the device, so that is where it gets stamped. Only the first time:
   * the device re-fetches anything it has not acknowledged, and the interesting
   * time is when it first arrived, not when it was last seen. Written after
   * the response is prepared and never allowed to fail the poll, because a
   * device that cannot fetch its messages is a worse outcome than a timestamp
   * that is missing. */
  /* Loose, deliberately: messages written before this field existed come back
     without it, and those should be stamped on the next poll rather than
     staying blank forever. */
  const undelivered = pending.filter((m) => m.delivered_at == null);
  const body = Response.json(
    { messages: pending.map(toDevice), server_time: now },
    { headers: { "cache-control": "no-store" } },
  );
  if (undelivered.length > 0) {
    try {
      await Promise.all(
        undelivered.map((m) => put({ ...m, delivered_at: now })),
      );
    } catch {
      /* Left for the next poll to stamp. */
    }
  }
  return body;
}
