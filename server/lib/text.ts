/* Text on its way to the panel.
 *
 * The device draws from glyph atlases baked over ASCII 32..126 and nothing
 * else. A phone keyboard silently turns a typed apostrophe into U+2019, which
 * is three bytes of UTF-8, all of them above 126, so all three are skipped and
 * "you're" arrives as "youre". The same goes for the curly quotes, the dashes
 * and the ellipsis every mobile keyboard substitutes without being asked.
 *
 * Normalising here rather than on the device means one implementation, applied
 * to messages and standing lines alike, and the sender sees exactly what the
 * panel will show because the preview runs the same function.
 */

const SUBSTITUTIONS: [RegExp, string][] = [
  [/[‘’‚‛′]/g, "'"],
  [/[“”„‟″]/g, '"'],
  [/[‐‑‒–—―]/g, "-"],
  [/…/g, "..."],
  [/[     ]/g, " "],
  [/[​‌‍﻿]/g, ""],
];

export function toPanelText(input: string): string {
  let out = input;
  for (const [pattern, replacement] of SUBSTITUTIONS) {
    out = out.replace(pattern, replacement);
  }
  /* Whatever is left that the panel has no glyph for is dropped rather than
   * drawn as a gap, since a gap is indistinguishable from a spacing bug. */
  return out.replace(/[^\x20-\x7E\n]/g, "");
}
