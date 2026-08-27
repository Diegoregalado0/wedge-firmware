"use client";

import { useEffect, useMemo, useState } from "react";

import { toPanelText } from "@/lib/text";
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

/* One shape for every timestamp on the page. Composed from the parts rather
   than taken from a locale string, because the locale wants to write "at"
   between the date and the time. */
function stamp(t: number): string {
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
 * her table. Those are different things to know.
 *
 * A scheduled message is fetched ahead of its hour so the device can show it
 * without the network, so having been handed over is not on its own enough to
 * call it delivered; it also has to be due. Until then it is still Sent. */
function stateOf(m: Message): { label: string; at: number | null; tone: string } {
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

export default function Page() {
  const [authed, setAuthed] = useState(false);
  const [password, setPassword] = useState("");
  const [authError, setAuthError] = useState("");

  const [text, setText] = useState("");
  /* Everything from this device is from the same person, so the kicker is
     fixed rather than chosen every time. */
  const type: MessageType = "affection";

  const [when, setWhen] = useState("");
  const [scheduleOpen, setScheduleOpen] = useState(false);
  /* The last set known to be on the server. Saving compares against this, so
     the button can be honest about whether anything is actually pending. */
  const [savedLines, setSavedLines] = useState<string[]>([]);
  const [messages, setMessages] = useState<Message[]>([]);
  const [lines, setLines] = useState<string[]>([]);
  const [linesBusy, setLinesBusy] = useState(false);
  const [linesNote, setLinesNote] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  /* The preview and the counter both run the panel's own normalisation, so
     what is counted and shown is what the device will be asked to draw. */
  const panelText = useMemo(() => toPanelText(text), [text]);
  const remaining = 280 - panelText.length;
  const overlong = remaining < 0;
  const linesDirty = useMemo(
    () => lines.length !== savedLines.length || lines.some((l, i) => l !== savedLines[i]),
    [lines, savedLines],
  );

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
      setSavedLines(a.lines);
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
    setSavedLines(data.lines);
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

  async function send(e?: React.FormEvent) {
    e?.preventDefault();
    if (overlong || !text.trim()) return;
    setBusy(true);
    setError("");
    const available_at = when ? Math.floor(new Date(when).getTime() / 1000) : undefined;
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

      <Preview text={panelText} type={type} />

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

        {/* Sending now is the whole point, so it is the button. Scheduling is
            the rare case and opens a dialog, because a date field parked on
            screen with nothing in it invites the question of whether it needs
            filling in. */}
        <div className="send">
          <button
            type="submit"
            className="send-main"
            disabled={busy || overlong || !text.trim()}
          >
            {busy ? "Sending" : "Send now"}
          </button>
          <button
            type="button"
            className="send-schedule"
            onClick={() => setScheduleOpen(true)}
            disabled={busy || overlong || !text.trim()}
          >
            Schedule
          </button>
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
        {/* Apart, because they do opposite things and sat close enough to be
            hit by accident. Saving stays unavailable until there is an actual
            change to save, so the button says whether anything is pending
            rather than always looking the same. */}
        <div className="lines-actions">
          <button
            type="button"
            className="ghost"
            onClick={() => setLines([...lines, ""])}
            disabled={lines.length >= 12 || linesBusy}
          >
            Add a line
          </button>
          <button
            type="button"
            className="primary"
            onClick={() => void saveLines(lines)}
            disabled={linesBusy || !linesDirty}
          >
            {linesBusy ? "Saving" : linesDirty ? "Save changes" : "Saved"}
          </button>
        </div>
        {linesNote ? <p className="muted">{linesNote}</p> : null}
      </section>

      {scheduleOpen ? (
        <div
          className="scrim"
          role="dialog"
          aria-modal="true"
          aria-label="Schedule this message"
          onClick={() => setScheduleOpen(false)}
        >
          <div className="modal" onClick={(e) => e.stopPropagation()}>
            <h2>Schedule</h2>
            <p className="muted">It stays hidden until this time, then waits on her table.</p>
            <label>
              <span>Send at</span>
              <input
                type="datetime-local"
                value={when}
                onChange={(e) => setWhen(e.target.value)}
                autoFocus
              />
            </label>
            <div className="modal-actions">
              <button
                type="button"
                className="ghost"
                onClick={() => {
                  setWhen("");
                  setScheduleOpen(false);
                }}
              >
                Cancel
              </button>
              <button
                type="button"
                className="primary"
                disabled={!when || busy}
                onClick={() => {
                  setScheduleOpen(false);
                  void send();
                }}
              >
                Schedule it
              </button>
            </div>
          </div>
        </div>
      ) : null}

      <section className="history">
        <h2>Sent</h2>
        {messages.length === 0 ? <p className="muted">Nothing yet.</p> : null}
        <ul>
          {messages.map((m) => (
            <li key={m.id}>
              <div>
                <p className="text">{m.text}</p>
                <p className="meta">
                  <span className={`dot ${stateOf(m).tone}`} />
                  {stateOf(m).label}
                  {stateOf(m).at !== null ? ` ${stamp(stateOf(m).at as number)}` : ""}
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
