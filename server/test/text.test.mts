/* The panel's atlases stop at ASCII 126, and a phone keyboard substitutes
   characters above it without being asked. This is the class of bug that made
   "you're" arrive on the device as "youre". */

import { test } from "node:test";
import assert from "node:assert/strict";
import { toPanelText } from "../lib/text.ts";

test("typographic apostrophes become the ASCII one", () => {
  assert.equal(toPanelText("you’re"), "you're");
  assert.equal(toPanelText("‘quoted’"), "'quoted'");
});

test("smart quotes, dashes and ellipsis are folded", () => {
  assert.equal(toPanelText("“hello”"), '"hello"');
  assert.equal(toPanelText("a—b"), "a-b");
  assert.equal(toPanelText("a–b"), "a-b");
  assert.equal(toPanelText("wait…"), "wait...");
});

test("non-breaking and zero-width spaces are normalised away", () => {
  assert.equal(toPanelText("a b"), "a b");
  assert.equal(toPanelText("a​b"), "ab");
});

test("anything with no glyph is dropped rather than left as a gap", () => {
  assert.equal(toPanelText("café"), "caf");
  assert.equal(toPanelText("hi \u{1F600}"), "hi ");
});

test("plain ASCII and newlines survive untouched", () => {
  const s = "Good morning.\nIt is 7:30 & all is well (really).";
  assert.equal(toPanelText(s), s);
});

test("every printable ASCII character is preserved", () => {
  let all = "";
  for (let c = 0x20; c <= 0x7e; c++) all += String.fromCharCode(c);
  assert.equal(toPanelText(all), all);
});
