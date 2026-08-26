import { senderAuthorized } from "@/lib/auth";
import { remove } from "@/lib/store";

export const dynamic = "force-dynamic";

export async function DELETE(req: Request, ctx: { params: Promise<{ id: string }> }) {
  if (!(await senderAuthorized(req))) {
    return Response.json({ error: "unauthorized" }, { status: 401 });
  }
  const { id } = await ctx.params;
  await remove(id);
  return Response.json({ ok: true });
}
