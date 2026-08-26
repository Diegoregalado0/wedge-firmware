"use client";

import { useEffect, useMemo, useState } from "react";

import { type Message, type MessageType } from "@/lib/types";

const KICKERS: Record<MessageType, string> = {
  normal: "FOR YOU",
  good_morning: "GOOD MORNING",
  good_night: "GOODNIGHT",
  encouragement: "FOR TODAY",
  advice: "A THOUGHT",
  compliment: "SOMETHING TRUE",
  affection: "FROM DIEGO",
  special_event: "TODAY",
};


/* The panel is 536 x 240 with a 26 px margin around the card. Matching those
 * numbers here means the preview wraps where the device wraps, so a message
 * that fits on screen is a message that fit in the box. */
function Preview({ text, type }: { text: string; type: MessageType }) {
  return (
    <div className="preview">
      <div className="panel">
        <div className="card">
          <div className="kicker">{KICKERS[type]}</div>
          <div className="body">{text || "…"}</div>
          <div className="handle" />
        </div>
      </div>
    </div>
  );
}

export default function Page() {
  const [authed, setAuthed] = useState(false);
  const [password, setPassword] = useState("");
  const [authError, setAuthError] = useState("");

  const [text, setText] = useState("");
  /* Everything from this device is from the same person, so the kicker is
     fixed rather than chosen every time. */
  const type: MessageType = "affection";
  const [when, setWhen] = useState("");
  const [scheduling, setScheduling] = useState(false);
  const [menuOpen, setMenuOpen] = useState(false);
  const [messages, setMessages] = useState<Message[]>([]);
  const [lines, setLines] = useState<string[]>([]);
  const [linesBusy, setLinesBusy] = useState(false);
  const [linesNote, setLinesNote] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  const remaining = 280 - text.length;
  const overlong = remaining < 0;

  async function refresh() {
    const res = await fetch("/api/messages");
    if (res.status === 401) {
      setAuthed(false);
      return;
    }
    const data = (await res.json()) as { messages: Message[] };
    setMessages(data.messages);
    setAuthed(true);

    const amb = await fetch("/api/ambient");
    if (amb.ok) {
      const a = (await amb.json()) as { lines: string[] };
      setLines(a.lines);
    }
  }

  async function saveLines(next: string[]) {
    setLinesBusy(true);
    setLinesNote("");
    const res = await fetch("/api/ambient", {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ lines: next }),
    });
    setLinesBusy(false);
    if (!res.ok) {
      const data = (await res.json()) as { error?: string };
      setLinesNote(data.error ?? "Could not save.");
      return;
    }
    const data = (await res.json()) as { lines: string[] };
    setLines(data.lines);
    setLinesNote("Saved. The wedge picks these up within five minutes.");
  }

  useEffect(() => {
    void refresh();
  }, []);

  async function login(e: React.FormEvent) {
    e.preventDefault();
    setAuthError("");
    const res = await fetch("/api/auth", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ password }),
    });
    if (!res.ok) {
      setAuthError("That is not the password.");
      return;
    }
    setPassword("");
    await refresh();
  }

  async function send(e: React.FormEvent) {
    e.preventDefault();
    if (overlong || !text.trim()) return;
    setBusy(true);
    setError("");
    const available_at =
      scheduling && when ? Math.floor(new Date(when).getTime() / 1000) : undefined;
    const res = await fetch("/api/messages", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ text, type, available_at }),
    });
    setBusy(false);
    if (!res.ok) {
      const data = (await res.json()) as { error?: string };
      setError(data.error ?? "Could not send.");
      return;
    }
    setText("");
    setWhen("");
    setScheduling(false);
    await refresh();
  }

  async function drop(id: string) {
    await fetch(`/api/messages/${id}`, { method: "DELETE" });
    await refresh();
  }

  const pending = useMemo(() => messages.filter((m) => m.status !== "read"), [messages]);

  if (!authed) {
    return (
      <main className="gate">
        <form onSubmit={login}>
          <h1>Wedge</h1>
          <p>Sign in to send something.</p>
          <input
            type="password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            placeholder="Password"
            autoFocus
          />
          <button type="submit">Continue</button>
          {authError ? <p className="error">{authError}</p> : null}
        </form>
      </main>
    );
  }

  return (
    <main>
      <header>
        <h1>Wedge</h1>
        <p>
          {pending.length === 0
            ? "Nothing waiting on her table."
            : `${pending.length} waiting on her table.`}
        </p>
      </header>

      <Preview text={text} type={type} />

      <form onSubmit={send} className="composer">
        <label>
          <span>Message</span>
          <textarea
            value={text}
            onChange={(e) => setText(e.target.value)}
            rows={3}
            placeholder="Say the thing."
            autoFocus
          />
          <small className={overlong ? "error" : ""}>
            {remaining} characters left
          </small>
        </label>

        {scheduling ? (
          <label>
            <span>Send at</span>
            <input
              type="datetime-local"
              value={when}
              onChange={(e) => setWhen(e.target.value)}
              autoFocus
            />
          </label>
        ) : null}

        {/* Sending now is the whole point, so it is the button. Scheduling is
            behind the caret because it is the rare case, and a date field
            sitting on screen with nothing in it invites the question of
            whether it needs filling in. */}
        <div className="send">
          <button type="submit" className="send-main" disabled={busy || overlong || !text.trim() || (scheduling && !when)}>
            {busy ? "Sending" : scheduling ? "Schedule" : "Send now"}
          </button>
          <button
            type="button"
            className="send-more"
            aria-label="Delivery options"
            aria-expanded={menuOpen}
            onClick={() => setMenuOpen((v) => !v)}
          >
            &#9662;
          </button>
          {menuOpen ? (
            <div className="send-menu">
              <button
                type="button"
                onClick={() => {
                  setScheduling(false);
                  setWhen("");
                  setMenuOpen(false);
                }}
              >
                Send now
              </button>
              <button
                type="button"
                onClick={() => {
                  setScheduling(true);
                  setMenuOpen(false);
                }}
              >
                Send later
              </button>
            </div>
          ) : null}
        </div>
        {error ? <p className="error">{error}</p> : null}
      </form>

      <section className="standing">
        <h2>Always on screen</h2>
        <ul>
          {lines.map((line, i) => (
            <li key={i}>
              <input
                type="text"
                value={line}
                maxLength={95}
                onChange={(e) => {
                  const next = [...lines];
                  next[i] = e.target.value;
                  setLines(next);
                }}
              />
              <button
                onClick={() => void saveLines(lines.filter((_, j) => j !== i))}
                disabled={lines.length <= 1 || linesBusy}
                aria-label="Remove line"
              >
                Remove
              </button>
            </li>
          ))}
        </ul>
        <div className="field-row">
          <button
            onClick={() => setLines([...lines, ""])}
            disabled={lines.length >= 12 || linesBusy}
          >
            Add a line
          </button>
          <button className="primary" onClick={() => void saveLines(lines)} disabled={linesBusy}>
            {linesBusy ? "Saving" : "Save lines"}
          </button>
        </div>
        {linesNote ? <p className="muted">{linesNote}</p> : null}
      </section>

      <section className="history">
        <h2>Sent</h2>
        {messages.length === 0 ? <p className="muted">Nothing yet.</p> : null}
        <ul>
          {messages.map((m) => (
            <li key={m.id}>
              <div>
                <p className="text">{m.text}</p>
                <p className="meta">
                  {m.status === "read"
                    ? `read ${new Date((m.read_at ?? 0) * 1000).toLocaleString()}`
                    : m.available_at * 1000 > Date.now()
                      ? `waiting until ${new Date(m.available_at * 1000).toLocaleString()}`
                      : "on her table now"}
                </p>
              </div>
              <button onClick={() => void drop(m.id)} aria-label="Delete">
                Remove
              </button>
            </li>
          ))}
        </ul>
      </section>
    </main>
  );
}
