/* How a message reads in the sent list: which of the three places it has got
 * to, and the one timestamp worth showing. Kept out of the component so it can
 * be tested without rendering anything. */

import type { Message } from "./types";

/* One shape for every timestamp on the page. Composed from the parts rather
   than taken from a locale string, because the locale wants to write "at"
   between the date and the time. */
export function stamp(t: number): string {
  const d = new Date(t * 1000);
  const date = d.toLocaleDateString("en-US", {
    month: "long",
    day: "numeric",
    year: "numeric",
  });
  const time = d.toLocaleTimeString("en-US", {
    hour: "numeric",
    minute: "2-digit",
    hour12: true,
  });
  return `${date}, ${time.replace(/\s(AM|PM)$/, (_m, p: string) => " " + p.toLowerCase())}`;
}

/* Where a message has got to, as three places rather than two.
 *
 * Sent and read were the only states the server knew, which put a message
 * still waiting for the next poll in the same bucket as one already lit up on
 * the device. Those are different things to know.
 *
 * A scheduled message is fetched ahead of its hour so the device can show it
 * without the network, so having been handed over is not on its own enough to
 * call it delivered; it also has to be due. Until then it is still Sent. */
export function stateOf(m: Message): { label: string; at: number | null; tone: string } {
  /* Only the reading is worth a time. The other two are steps on the way and
     the question they answer is where it has got to, not when: a message that
     is sent or waiting is going to move again shortly, and stamping those puts
     three times on a row where one of them matters. */
  if (m.status === "read") {
    return { label: "Read", at: m.read_at ?? m.created_at, tone: "done" };
  }
  if (m.delivered_at && m.available_at * 1000 <= Date.now()) {
    return { label: "Delivered", at: null, tone: "there" };
  }
  return { label: "Sent", at: null, tone: "sending" };
}
