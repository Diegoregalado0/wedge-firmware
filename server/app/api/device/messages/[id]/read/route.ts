/* Read acknowledgement. Idempotent: the device retries this after an offline
 * period, and a second acknowledgement of the same message must not be an
 * error or the retry loop never terminates. */

import { deviceAuthorized } from "@/lib/auth";
import { get, put } from "@/lib/store";

export const dynamic = "force-dynamic";

export async function POST(req: Request, ctx: { params: Promise<{ id: string }> }) {
  if (!deviceAuthorized(req)) {
    return Response.json({ error: "unauthorized" }, { status: 401 });
  }
  const { id } = await ctx.params;
  const message = await get(id);
  if (!message) {
    return Response.json({ ok: true, unknown: true });
  }
  if (message.status !== "read") {
    message.status = "read";
    message.read_at = Math.floor(Date.now() / 1000);
    await put(message);
  }
  return Response.json({ ok: true });
}
