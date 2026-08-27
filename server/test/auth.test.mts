/* Two separate credentials: a device token that authenticates the appliance and
   a sender password that never reaches it. */

import { test } from "node:test";
import assert from "node:assert/strict";

process.env.DEVICE_TOKEN = "device-token";
process.env.SENDER_PASSWORD = "sender-password";

const { deviceAuthorized, checkPassword, issueSession, senderAuthorized } = await import(
  "../lib/auth.ts"
);

function withAuth(header: string) {
  return new Request("http://x", { headers: { authorization: header } });
}

test("the device token is accepted only as a bearer token", () => {
  assert.equal(deviceAuthorized(withAuth("Bearer device-token")), true);
  assert.equal(deviceAuthorized(withAuth("device-token")), false);
  assert.equal(deviceAuthorized(withAuth("Bearer wrong")), false);
  assert.equal(deviceAuthorized(new Request("http://x")), false);
});

test("a token that is a prefix of the real one is refused", () => {
  assert.equal(deviceAuthorized(withAuth("Bearer device-toke")), false);
  assert.equal(deviceAuthorized(withAuth("Bearer device-tokenx")), false);
});

test("the sender password is checked exactly", () => {
  assert.equal(checkPassword("sender-password"), true);
  assert.equal(checkPassword("sender-passwor"), false);
  assert.equal(checkPassword(""), false);
});

test("a freshly issued session is accepted", async () => {
  const s = await issueSession();
  const req = new Request("http://x", { headers: { cookie: `wedge_session=${encodeURIComponent(s)}` } });
  assert.equal(await senderAuthorized(req), true);
});

test("a session with a forged signature is refused", async () => {
  const s = await issueSession();
  const [issued] = s.split(".");
  const forged = `${issued}.${"0".repeat(64)}`;
  const req = new Request("http://x", {
    headers: { cookie: `wedge_session=${encodeURIComponent(forged)}` },
  });
  assert.equal(await senderAuthorized(req), false);
});

test("a session older than its window is refused", async () => {
  /* Signed correctly, but issued thirty-one days ago. */
  const { issueSession: _i } = await import("../lib/auth.ts");
  const old = String(Date.now() - 31 * 24 * 3600 * 1000);
  const s = await issueSession();
  const mac = s.split(".")[1];
  const req = new Request("http://x", {
    headers: { cookie: `wedge_session=${encodeURIComponent(`${old}.${mac}`)}` },
  });
  assert.equal(await senderAuthorized(req), false);
});

test("no cookie at all is refused", async () => {
  assert.equal(await senderAuthorized(new Request("http://x")), false);
});
