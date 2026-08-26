import { SESSION_COOKIE_NAME, checkPassword, issueSession } from "@/lib/auth";

export const dynamic = "force-dynamic";

export async function POST(req: Request) {
  let body: { password?: string };
  try {
    body = (await req.json()) as { password?: string };
  } catch {
    return Response.json({ error: "invalid json" }, { status: 400 });
  }
  if (!checkPassword(body.password ?? "")) {
    /* A fixed delay on failure, so repeated guesses cost the attacker time
     * without costing the sender anything on the one path that matters. */
    await new Promise((r) => setTimeout(r, 600));
    return Response.json({ error: "wrong password" }, { status: 401 });
  }
  const session = await issueSession();
  return Response.json(
    { ok: true },
    {
      headers: {
        "set-cookie": `${SESSION_COOKIE_NAME}=${encodeURIComponent(session)}; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=${30 * 24 * 3600}`,
      },
    },
  );
}
