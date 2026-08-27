/* The sender and device endpoints, called directly. The store falls back to an
   in-memory backend when no blob or redis credentials are present, so these
   exercise the real handlers with no mocking and no network. */

import { test } from "node:test";
import assert from "node:assert/strict";

process.env.DEVICE_TOKEN = "test-device-token";
process.env.SENDER_PASSWORD = "test-sender-password";
delete process.env.BLOB_READ_WRITE_TOKEN;
delete process.env.KV_REST_API_URL;

const { POST: sendMessage, GET: listMessages } = await import("../app/api/messages/route.ts");
const { GET: devicePoll } = await import("../app/api/device/messages/route.ts");
const { POST: login } = await import("../app/api/auth/route.ts");

const DEVICE = { authorization: "Bearer test-device-token" };

async function session(): Promise<string> {
  const res = await login(
    new Request("http://x/api/auth", {
      method: "POST",
      body: JSON.stringify({ password: "test-sender-password" }),
    }),
  );
  const cookie = res.headers.get("set-cookie") ?? "";
  return cookie.split(";")[0];
}

function send(cookie: string, body: unknown) {
  return sendMessage(
    new Request("http://x/api/messages", {
      method: "POST",
      headers: { cookie, "content-type": "application/json" },
      body: JSON.stringify(body),
    }),
  );
}

test("a message cannot be sent without a session", async () => {
  const res = await send("", { text: "hi" });
  assert.equal(res.status, 401);
});

test("the device cannot poll without its token", async () => {
  const res = await devicePoll(new Request("http://x/api/device/messages"));
  assert.equal(res.status, 401);
});

test("empty text is refused", async () => {
  const res = await send(await session(), { text: "   " });
  assert.equal(res.status, 400);
});

test("text past the panel's buffer is refused rather than silently cut", async () => {
  const res = await send(await session(), { text: "x".repeat(281) });
  assert.equal(res.status, 400);
});

test("the length limit counts normalised text", async () => {
  /* Each ellipsis becomes three characters, so 100 of them is 300 and over. */
  const res = await send(await session(), { text: "…".repeat(100) });
  assert.equal(res.status, 400);
});

test("an unknown type falls back rather than erroring", async () => {
  const res = await send(await session(), { text: "hi", type: "nonsense" });
  assert.equal(res.status, 201);
  const { message } = await res.json();
  assert.equal(message.type, "normal");
});

test("priority is clamped to the allowed range", async () => {
  const res = await send(await session(), { text: "hi", priority: 99 });
  const { message } = await res.json();
  assert.ok(message.priority <= 3);
});

test("smart punctuation is normalised on the way in", async () => {
  const res = await send(await session(), { text: "you’re here" });
  const { message } = await res.json();
  assert.equal(message.text, "you're here");
});

test("a new message starts undelivered and unread", async () => {
  const res = await send(await session(), { text: "fresh" });
  const { message } = await res.json();
  assert.equal(message.delivered_at, null);
  assert.equal(message.read_at, null);
  assert.equal(message.status, "available");
});

test("polling stamps delivery once, and does not move it afterwards", async () => {
  const cookie = await session();
  const created = (await (await send(cookie, { text: "stamp me" })).json()).message;

  await devicePoll(new Request("http://x/api/device/messages", { headers: DEVICE }));
  const afterFirst = (await (await listMessages(
    new Request("http://x/api/messages", { headers: { cookie } }),
  )).json()).messages.find((m: { id: string }) => m.id === created.id);
  assert.ok(afterFirst.delivered_at, "should be stamped by the first poll");

  await new Promise((r) => setTimeout(r, 1100));
  await devicePoll(new Request("http://x/api/device/messages", { headers: DEVICE }));
  const afterSecond = (await (await listMessages(
    new Request("http://x/api/messages", { headers: { cookie } }),
  )).json()).messages.find((m: { id: string }) => m.id === created.id);
  assert.equal(
    afterSecond.delivered_at,
    afterFirst.delivered_at,
    "a later poll must not move the arrival time",
  );
});

test("the device is sent only the fields it parses", async () => {
  await send(await session(), { text: "shape" });
  const res = await devicePoll(new Request("http://x/api/device/messages", { headers: DEVICE }));
  const { messages, server_time } = await res.json();
  assert.ok(typeof server_time === "number");
  assert.ok(messages.length > 0);
  assert.deepEqual(
    Object.keys(messages[0]).sort(),
    ["available_at", "expires_at", "id", "priority", "text", "type"],
  );
});
