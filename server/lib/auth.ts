/* Two separate credentials, per section 39.
 *
 * The device holds a bearer token that authenticates the appliance and nothing
 * else, so it can be revoked without touching the sender's access. The sender
 * holds a password that never reaches the device. Neither is embedded in
 * source; both come from the environment. */

const DEVICE_TOKEN = process.env.DEVICE_TOKEN ?? "";
const SENDER_PASSWORD = process.env.SENDER_PASSWORD ?? "";
const SESSION_COOKIE = "wedge_session";

/* Length-independent comparison. These are short secrets compared on every
 * poll, which is exactly the shape a timing attack likes. */
function constantTimeEqual(a: string, b: string): boolean {
  const enc = new TextEncoder();
  const ab = enc.encode(a);
  const bb = enc.encode(b);
  let diff = ab.length ^ bb.length;
  const n = Math.max(ab.length, bb.length);
  for (let i = 0; i < n; i++) {
    diff |= (ab[i] ?? 0) ^ (bb[i] ?? 0);
  }
  return diff === 0;
}

export function deviceAuthorized(req: Request): boolean {
  if (!DEVICE_TOKEN) return false;
  const header = req.headers.get("authorization") ?? "";
  const prefix = "Bearer ";
  if (!header.startsWith(prefix)) return false;
  return constantTimeEqual(header.slice(prefix.length).trim(), DEVICE_TOKEN);
}

async function sign(value: string): Promise<string> {
  const key = await crypto.subtle.importKey(
    "raw",
    new TextEncoder().encode(SENDER_PASSWORD),
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"],
  );
  const mac = await crypto.subtle.sign("HMAC", key, new TextEncoder().encode(value));
  return Array.from(new Uint8Array(mac))
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

export async function issueSession(): Promise<string> {
  const issued = String(Date.now());
  return `${issued}.${await sign(issued)}`;
}

export async function senderAuthorized(req: Request): Promise<boolean> {
  if (!SENDER_PASSWORD) return false;
  const cookie = req.headers.get("cookie") ?? "";
  const match = cookie.match(new RegExp(`${SESSION_COOKIE}=([^;]+)`));
  if (!match) return false;
  const [issued, mac] = decodeURIComponent(match[1]).split(".");
  if (!issued || !mac) return false;
  /* Thirty days. This is a personal tool on a personal phone, and forcing a
   * re-login every week would mean the messages simply stop being sent. */
  if (Date.now() - Number(issued) > 30 * 24 * 3600 * 1000) return false;
  return constantTimeEqual(await sign(issued), mac);
}

export function checkPassword(candidate: string): boolean {
  if (!SENDER_PASSWORD) return false;
  return constantTimeEqual(candidate, SENDER_PASSWORD);
}

export const SESSION_COOKIE_NAME = SESSION_COOKIE;
