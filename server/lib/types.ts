/* The wire contract between the appliance and this service. The device parses
 * these fields with cJSON and ignores anything it does not recognize, so the
 * shape may grow but existing names must not change meaning. */

export const MESSAGE_TYPES = [
  "normal",
  "good_morning",
  "good_night",
  "encouragement",
  "advice",
  "compliment",
  "affection",
  "special_event",
] as const;

export type MessageType = (typeof MESSAGE_TYPES)[number];

export type MessageStatus = "available" | "read";

export interface Message {
  id: string;
  text: string;
  type: MessageType;
  priority: number;
  created_at: number;
  /* Unix seconds. The device compares these against its own NTP-synced clock,
   * so scheduling is enforced on the device and not by withholding here: a
   * message that arrives early still waits for its hour. */
  available_at: number;
  expires_at: number;
  status: MessageStatus;
  /* When the device first fetched it. A message can sit on the server for up
   * to a poll interval before the Wedge has ever seen it, and that is a
   * genuinely different state from being on the device: one is still in the
   * post, the other has arrived. Null until the device asks for it. */
  delivered_at: number | null;
  read_at: number | null;
}

export interface MessageInput {
  text: string;
  type: MessageType;
  priority?: number;
  available_at?: number;
  expires_at?: number;
}

/* The device only ever needs these fields, and sending it less means less to
 * parse on a chip with an 8 kB task stack. */
export function toDevice(m: Message) {
  return {
    id: m.id,
    text: m.text,
    type: m.type,
    priority: m.priority,
    available_at: m.available_at,
    expires_at: m.expires_at,
  };
}
