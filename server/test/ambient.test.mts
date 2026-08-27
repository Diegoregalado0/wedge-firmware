/* The standing lines. The device holds a fixed number of fixed-length slots,
   so the server must not hand it anything that will not fit. */

import { test } from "node:test";
import assert from "node:assert/strict";
import { AMBIENT_MAX, AMBIENT_TEXT_MAX, sanitize } from "../lib/ambient.ts";

test("blank and whitespace-only lines are dropped", () => {
  assert.deepEqual(sanitize(["a", "", "   ", "b"]), ["a", "b"]);
});

test("inner whitespace is collapsed", () => {
  assert.deepEqual(sanitize(["too    many   spaces"]), ["too many spaces"]);
});

test("lines are cut to the device's buffer", () => {
  const [line] = sanitize(["x".repeat(AMBIENT_TEXT_MAX + 50)]);
  assert.equal(line.length, AMBIENT_TEXT_MAX);
});

test("the bank is capped at the number of slots", () => {
  const many = Array.from({ length: AMBIENT_MAX + 10 }, (_, i) => `line ${i}`);
  assert.equal(sanitize(many).length, AMBIENT_MAX);
});

test("smart punctuation is normalised, as it is for messages", () => {
  assert.deepEqual(sanitize(["it’s fine — really…"]), ["it's fine - really..."]);
});

test("non-strings and non-arrays are rejected rather than crashing", () => {
  assert.deepEqual(sanitize(["ok", 5, null, {}] as unknown), ["ok"]);
  assert.deepEqual(sanitize("not an array" as unknown), []);
  assert.deepEqual(sanitize(undefined as unknown), []);
});
