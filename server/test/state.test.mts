/* Which of the three places a message has got to. The interesting case is a
   scheduled one: the device is handed it ahead of its hour so it can show it
   without the network, so having been delivered is not on its own enough to
   say it is on the device. */

import { test } from "node:test";
import assert from "node:assert/strict";
import { stamp, stateOf } from "../lib/state.ts";
import type { Message } from "../lib/types.ts";

const now = Math.floor(Date.now() / 1000);

function msg(over: Partial<Message> = {}): Message {
  return {
    id: "abc",
    text: "hello",
    type: "affection",
    priority: 1,
    created_at: now - 600,
    available_at: now - 600,
    expires_at: 0,
    status: "available",
    delivered_at: null,
    read_at: null,
    ...over,
  };
}

test("not yet fetched by the device reads Sent", () => {
  assert.equal(stateOf(msg()).label, "Sent");
});

test("fetched and due reads Delivered", () => {
  assert.equal(stateOf(msg({ delivered_at: now - 60 })).label, "Delivered");
});

test("fetched but not yet due still reads Sent", () => {
  const m = msg({ delivered_at: now - 60, available_at: now + 3600 });
  assert.equal(stateOf(m).label, "Sent");
});

test("read reads Read whatever else is set", () => {
  const m = msg({ status: "read", delivered_at: now - 60, read_at: now - 10 });
  assert.equal(stateOf(m).label, "Read");
  assert.equal(stateOf(m).at, now - 10);
});

test("only Read carries a time", () => {
  assert.equal(stateOf(msg()).at, null);
  assert.equal(stateOf(msg({ delivered_at: now })).at, null);
  assert.notEqual(stateOf(msg({ status: "read", read_at: now })).at, null);
});

test("a read message with no read_at falls back rather than showing 1970", () => {
  const m = msg({ status: "read", read_at: null });
  assert.equal(stateOf(m).at, m.created_at);
});

test("the timestamp is the agreed shape", () => {
  /* 2026-08-27 17:14:32 UTC, read back in whatever zone the test runs in, so
     the assertion is on the shape rather than on a particular hour. */
  const s = stamp(1787850872);
  assert.match(s, /^[A-Z][a-z]+ \d{1,2}, \d{4}, \d{1,2}:\d{2} (am|pm)$/);
});
