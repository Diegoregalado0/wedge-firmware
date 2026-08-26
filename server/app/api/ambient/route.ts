import { senderAuthorized } from "@/lib/auth";
import { AMBIENT_MAX, readLines, sanitize, writeLines } from "@/lib/ambient";

export const dynamic = "force-dynamic";

export async function GET(req: Request) {
  if (!(await senderAuthorized(req))) {
    return Response.json({ error: "unauthorized" }, { status: 401 });
  }
  return Response.json({ lines: await readLines(), max: AMBIENT_MAX });
}

export async function PUT(req: Request) {
  if (!(await senderAuthorized(req))) {
    return Response.json({ error: "unauthorized" }, { status: 401 });
  }
  let body: unknown;
  try {
    body = await req.json();
  } catch {
    return Response.json({ error: "invalid json" }, { status: 400 });
  }
  const lines = sanitize((body as { lines?: unknown }).lines);
  if (lines.length === 0) {
    return Response.json({ error: "at least one line is required" }, { status: 400 });
  }
  return Response.json({ lines: await writeLines(lines) });
}
