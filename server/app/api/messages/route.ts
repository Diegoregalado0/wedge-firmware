/* Sender endpoints. Separate from the device's, with separate credentials. */

import { senderAuthorized } from "@/lib/auth";
import { create, listAll, put } from "@/lib/store";
import { toPanelText } from "@/lib/text";
import { MESSAGE_TYPES, type MessageInput, type MessageType } from "@/lib/types";

export const dynamic = "force-dynamic";

const MAX_TEXT = 280;

export async function GET(req: Request) {
  if (!(await senderAuthorized(req))) {
    return Response.json({ error: "unauthorized" }, { status: 401 });
  }
  const all = await listAll();
  all.sort((a, b) => b.created_at - a.created_at);
  return Response.json({ messages: all });
}

export async function POST(req: Request) {
  if (!(await senderAuthorized(req))) {
    return Response.json({ error: "unauthorized" }, { status: 401 });
  }

  let body: unknown;
  try {
    body = await req.json();
  } catch {
    return Response.json({ error: "invalid json" }, { status: 400 });
  }

  const input = body as Partial<MessageInput>;
  /* Normalised before the length check, so the limit counts what the panel
   * will actually be asked to draw. */
  const text = typeof input.text === "string" ? toPanelText(input.text).trim() : "";
  if (!text) {
    return Response.json({ error: "text is required" }, { status: 400 });
  }
  /* The device's own buffer is 280 bytes and it truncates silently. Rejecting
   * here instead means the sender finds out now rather than discovering a cut
   * sentence on the bedside table. */
  if (text.length > MAX_TEXT) {
    return Response.json(
      { error: `text must be ${MAX_TEXT} characters or fewer` },
      { status: 400 },
    );
  }
  const type = (MESSAGE_TYPES as readonly string[]).includes(input.type ?? "")
    ? (input.type as MessageType)
    : "normal";

  const message = create({
    text,
    type,
    priority: typeof input.priority === "number" ? Math.max(0, Math.min(3, input.priority)) : 1,
    available_at: typeof input.available_at === "number" ? input.available_at : undefined,
    expires_at: typeof input.expires_at === "number" ? input.expires_at : undefined,
  });
  await put(message);
  return Response.json({ message }, { status: 201 });
}
