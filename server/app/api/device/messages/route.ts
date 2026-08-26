/* The device's poll endpoint. Called every five minutes, so it must be cheap
 * and must never fail in a way that leaves the appliance without its cache. */

import { deviceAuthorized } from "@/lib/auth";
import { listAll } from "@/lib/store";
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
    .slice(0, 8)
    .map(toDevice);

  return Response.json(
    { messages: pending, server_time: now },
    { headers: { "cache-control": "no-store" } },
  );
}
