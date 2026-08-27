/* Publishing a build and telling the device about it. Handing the appliance new
   firmware is strictly more powerful than sending it a message, so the two
   sides of this are authenticated differently and both are checked. */

import { test } from "node:test";
import assert from "node:assert/strict";

process.env.DEVICE_TOKEN = "device-token";
process.env.SENDER_PASSWORD = "sender-password";
delete process.env.BLOB_READ_WRITE_TOKEN;

const { POST: publish, GET: readAsSender } = await import("../app/api/firmware/route.ts");
const { GET: deviceManifest } = await import("../app/api/device/firmware/route.ts");
const { POST: login } = await import("../app/api/auth/route.ts");

const DEVICE = { authorization: "Bearer device-token" };

async function session(): Promise<string> {
  const res = await login(
    new Request("http://x/api/auth", {
      method: "POST",
      body: JSON.stringify({ password: "sender-password" }),
    }),
  );
  return (res.headers.get("set-cookie") ?? "").split(";")[0];
}

/* An image large enough to be plausible, starting with the ESP32 magic byte. */
function image(size = 128 * 1024): Uint8Array {
  const b = new Uint8Array(size);
  b[0] = 0xe9;
  for (let i = 1; i < size; i++) b[i] = i & 0xff;
  return b;
}

function post(cookie: string, body: BodyInit, version = "v0.1.0") {
  return publish(
    new Request(`http://x/api/firmware?version=${encodeURIComponent(version)}`, {
      method: "POST",
      headers: { cookie },
      body,
    }),
  );
}

test("the device cannot publish firmware with its own token", async () => {
  const res = await publish(
    new Request("http://x/api/firmware?version=v1", { method: "POST", headers: DEVICE, body: image() }),
  );
  assert.equal(res.status, 401);
});

test("the sender cannot read the manifest without a session", async () => {
  assert.equal((await readAsSender(new Request("http://x/api/firmware"))).status, 401);
});

test("the device cannot read the manifest without its token", async () => {
  assert.equal((await deviceManifest(new Request("http://x/api/device/firmware"))).status, 401);
});

test("a version is required and must fit the device's field", async () => {
  const c = await session();
  assert.equal((await post(c, image(), "")).status, 400);
  assert.equal((await post(c, image(), "v".repeat(33))).status, 400);
});

test("something that is not an ESP32 image is refused", async () => {
  const notAnImage = image();
  notAnImage[0] = 0x00;
  assert.equal((await post(await session(), notAnImage)).status, 400);
});

test("something too small to be firmware is refused", async () => {
  assert.equal((await post(await session(), image(1024))).status, 400);
});

test("publishing records the version, size and digest", async () => {
  const bin = image();
  const res = await post(await session(), bin, "v0.2.0");
  assert.equal(res.status, 201);
  const { manifest } = await res.json();
  assert.equal(manifest.version, "v0.2.0");
  assert.equal(manifest.size, bin.byteLength);
  assert.match(manifest.sha256, /^[0-9a-f]{64}$/);
});

test("the digest is the real one, so a corrupted download can be caught", async () => {
  const bin = image();
  const { manifest } = await (await post(await session(), bin, "v0.3.0")).json();
  const expected = Array.from(
    new Uint8Array(await crypto.subtle.digest("SHA-256", bin)),
  )
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
  assert.equal(manifest.sha256, expected);
});

test("the device is told about the newest published build", async () => {
  await post(await session(), image(), "v0.4.0");
  const res = await deviceManifest(new Request("http://x/api/device/firmware", { headers: DEVICE }));
  assert.equal(res.status, 200);
  const m = await res.json();
  assert.equal(m.version, "v0.4.0");
  assert.ok(m.url);
  assert.ok(m.sha256);
});
