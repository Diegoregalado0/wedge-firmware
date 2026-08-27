/* Publishing a build. Sender-authenticated, because handing the appliance a
 * new firmware is strictly more powerful than sending it a message. */

import { senderAuthorized } from "@/lib/auth";
import { publish, readManifest } from "@/lib/firmware";

export const dynamic = "force-dynamic";
export const maxDuration = 60;

export async function GET(req: Request) {
  if (!(await senderAuthorized(req))) {
    return Response.json({ error: "unauthorized" }, { status: 401 });
  }
  return Response.json({ manifest: await readManifest() });
}

export async function POST(req: Request) {
  if (!(await senderAuthorized(req))) {
    return Response.json({ error: "unauthorized" }, { status: 401 });
  }
  const version = new URL(req.url).searchParams.get("version")?.trim() ?? "";
  if (!version || version.length > 32) {
    /* The device's app description field is 32 bytes and it compares against
     * exactly what it is running, so a version it cannot store is useless. */
    return Response.json({ error: "version is required, 32 chars or fewer" }, { status: 400 });
  }
  const binary = await req.arrayBuffer();
  if (binary.byteLength < 64 * 1024) {
    return Response.json({ error: "that is too small to be a firmware image" }, { status: 400 });
  }
  /* The image starts with the ESP32 magic byte. Checking it here means a
   * mistyped path in the release script fails now rather than on the device. */
  if (new Uint8Array(binary)[0] !== 0xe9) {
    return Response.json({ error: "not an ESP32 image" }, { status: 400 });
  }
  return Response.json({ manifest: await publish(binary, version) }, { status: 201 });
}
