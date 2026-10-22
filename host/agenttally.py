#!/usr/bin/env python3
"""AgentTally host bridge.

Collects agent session events (Claude Code hooks, a generic HTTP API, Codex
notify) and mirrors the live session list to an AgentTally display over USB
serial. A desktop simulator can attach over TCP and behaves like a device.

Standard library only. Works on macOS and Linux; on Windows install pyserial.

  agenttally.py serve            run the bridge (foreground)
  agenttally.py install          add Claude Code hooks + autostart service
  agenttally.py uninstall        remove both again
  agenttally.py status           show sessions known to the running bridge
  agenttally.py send ...         push a generic event (for scripts/other agents)
  agenttally.py demo             fill the bridge with fake sessions
  agenttally.py device KEY=VAL   change a device setting (brightness=, invert=...)
"""

import argparse
import glob
import json
import os
import platform
import queue
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import unicodedata
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = "0.1.0"
PROTO = 1
HTTP_PORT = 47321
SIM_PORT = 47322
BAUD = 115200
HOME = os.path.expanduser("~/.agent-tally")
STATE_FILE = os.path.join(HOME, "state.json")
HOOK_MARK = "agent-tally"
MAX_SESSIONS = 24

# USB-serial bridges used on Sunton "Cheap Yellow Display" boards.
KNOWN_USB = {(0x1A86, 0x7523), (0x1A86, 0x55D4), (0x1A86, 0x55D3), (0x10C4, 0xEA60), (0x303A, 0x1001)}

STATES = ("working", "waiting", "done", "idle")
STATE_RANK = {"waiting": 0, "done": 1, "working": 2, "idle": 3}

log_lock = threading.Lock()
VERBOSE = False


def log(*a):
    with log_lock:
        print(time.strftime("%H:%M:%S"), *a, flush=True)


def dbg(*a):
    if VERBOSE:
        log(*a)


# ---------------------------------------------------------------- text helpers

_EXTRA = set("–—‘’“”•…←→")


def _glyph_ok(c):
    o = ord(c)
    return 0x20 <= o < 0x7F or 0xA0 <= o < 0x180 or c in _EXTRA


def _fold_char(c):
    if _glyph_ok(c):
        return c
    base = "".join(x for x in unicodedata.normalize("NFKD", c) if _glyph_ok(x))
    return base  # emoji, CJK... are dropped


def ascii_fold(s, limit):
    """Keep what the device fonts can draw (ASCII, Latin-1, Latin Extended-A: ş ğ ı ü ö ç ß ...)."""
    if not s:
        return ""
    s = re.sub(r"\s+", " ", str(s))
    s = "".join(_fold_char(c) for c in s)
    s = re.sub(r" {2,}", " ", s).strip()
    if len(s) > limit:
        s = s[: limit - 1].rstrip() + "…"
    return s


def strip_markdown(s):
    s = re.sub(r"```.*?```", " [code] ", s or "", flags=re.S)
    s = re.sub(r"`([^`]*)`", r"\1", s)
    s = re.sub(r"\*\*([^*]*)\*\*", r"\1", s)
    s = re.sub(r"^#+\s*", "", s, flags=re.M)
    s = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", s)
    return s


def first_sentence(text, limit=96):
    text = re.sub(r"\s+", " ", text or "").strip()
    m = re.match(r"(.+?[.!?])(\s|$)", text)
    s = m.group(1) if m else text
    return s if len(s) <= limit else s[: limit - 1].rstrip() + "\u2026"


def tool_summary(name, inp):
    """One short line describing what a tool call is doing."""
    inp = inp if isinstance(inp, dict) else {}
    base = lambda p: os.path.basename(str(p).rstrip("/")) or str(p)
    if name == "Bash":
        return "$ " + (inp.get("description") or inp.get("command") or "").split("\n")[0]
    if name in ("Read", "Edit", "Write", "MultiEdit", "NotebookEdit"):
        p = inp.get("file_path") or inp.get("notebook_path") or ""
        return f"{name} {base(p)}"
    if name in ("Grep", "Glob"):
        return f"{name} {inp.get('pattern', '')}"
    if name == "WebFetch":
        m = re.match(r"https?://([^/]+)", inp.get("url", ""))
        return "Fetch " + (m.group(1) if m else "web")
    if name == "WebSearch":
        return "Search " + inp.get("query", "")
    if name in ("Task", "Agent"):
        return "Agent: " + (inp.get("description") or inp.get("subagent_type") or "subagent")
    if name == "TodoWrite":
        return "Updating todo list"
    if name == "Skill":
        return "Skill " + str(inp.get("skill", ""))
    if name.startswith("mcp__"):
        parts = name.split("__")
        return f"{parts[1]}: {parts[-1]}" if len(parts) >= 3 else name
    return name


# ---------------------------------------------------------------- session store

class Store:
    def __init__(self):
        self.lock = threading.RLock()
        self.sessions = {}          # id -> dict
        self.listeners = []         # callables(kind, session_or_id)
        self.dirty_save = False

    # persistence ---------------------------------------------------------
    def load(self):
        try:
            with open(STATE_FILE) as f:
                data = json.load(f)
            for s in data.get("sessions", []):
                self.sessions[s["id"]] = s
            log(f"restored {len(self.sessions)} session(s)")
        except FileNotFoundError:
            pass
        except Exception as e:
            log("state restore failed:", e)

    def save(self):
        with self.lock:
            if not self.dirty_save:
                return
            self.dirty_save = False
            data = {"sessions": list(self.sessions.values())}
        os.makedirs(HOME, exist_ok=True)
        tmp = STATE_FILE + ".tmp"
        with open(tmp, "w") as f:
            json.dump(data, f)
        os.replace(tmp, STATE_FILE)

    # mutation ------------------------------------------------------------
    def _emit(self, kind, payload):
        self.dirty_save = True
        for fn in list(self.listeners):
            try:
                fn(kind, payload)
            except Exception as e:
                log("listener error:", e)

    def get(self, sid):
        with self.lock:
            return self.sessions.get(sid)

    def upsert(self, sid, quiet=False, **fields):
        """quiet=True: bookkeeping change that should not count as activity."""
        now = time.time()
        with self.lock:
            s = self.sessions.get(sid)
            created = s is None
            if created:
                s = {"id": sid, "src": "agent", "name": sid, "cwd": "", "state": "idle", "detail": "",
                     "since": now, "updated": now, "prompt": "", "reply": "", "sub": 0, "ack": True,
                     "pid": 0, "transcript": "", "events": []}
                self.sessions[sid] = s
            new_state = fields.pop("state", None)
            event = fields.pop("event", None)
            for k, v in fields.items():
                if v is not None:
                    s[k] = v
            if new_state and new_state != s["state"]:
                s["state"] = new_state
                s["since"] = now
                if new_state == "done":
                    s["ack"] = False
                elif new_state in ("working", "waiting"):
                    s["ack"] = True
            if event:
                s["events"] = (s["events"] + [[int(now), event]])[-8:]
            s["updated"] = now
            if not quiet:
                s["act"] = now
            self._evict()
            self._emit("upsert", dict(s))
            return s

    def remove(self, sid):
        with self.lock:
            if self.sessions.pop(sid, None) is not None:
                self._emit("remove", sid)

    def resolve(self, wire_id):
        """Display ids are cut to 40 characters; map one back to the full session id."""
        with self.lock:
            if wire_id in self.sessions:
                return wire_id
            hits = [k for k in self.sessions if k.startswith(wire_id)] if len(wire_id) >= 40 else []
            return hits[0] if len(hits) == 1 else wire_id

    def ack(self, sid):
        with self.lock:
            s = self.sessions.get(sid)
            if s and not s.get("ack"):
                s["ack"] = True
                self._emit("upsert", dict(s))

    def _evict(self):
        if len(self.sessions) <= MAX_SESSIONS:
            return
        # drop the oldest idle/done sessions first
        victims = sorted(self.sessions.values(), key=lambda s: (STATE_RANK[s["state"]] < 2, s["updated"]))
        for s in victims[: len(self.sessions) - MAX_SESSIONS]:
            self.sessions.pop(s["id"], None)
            self._emit("remove", s["id"])

    def snapshot(self):
        with self.lock:
            return [dict(s) for s in self.sessions.values()]


STORE = Store()


# ---------------------------------------------------------------- claude code adapter

def pid_is_agent(pid):
    """True if pid is alive and looks like a Claude Code / node process."""
    if not pid or pid <= 1:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    try:
        out = subprocess.run(["ps", "-p", str(pid), "-o", "command="], capture_output=True, text=True,
                             timeout=2).stdout.lower()
    except Exception:
        return True
    return "claude" in out or "node" in out


_TITLE_CACHE = {}  # transcript path -> (checked_at, mtime, (custom, ai))


def transcript_titles(path):
    """(custom, ai) titles Claude Code wrote into the transcript: the terminal tab title.

    `custom-title` comes from /rename, `ai-title` is generated automatically. Both
    repeat through the file, so the tail usually has them; read the whole file
    once if it doesn't. Cached, and re-read at most every 5 s when the file changed.
    """
    if not path:
        return (None, None)
    now = time.time()
    hit = _TITLE_CACHE.get(path)
    try:
        mtime = os.path.getmtime(path)
    except OSError:
        return hit[2] if hit else (None, None)
    if hit and (hit[1] == mtime or now - hit[0] < 5):
        return hit[2]
    found = {"custom-title": None, "ai-title": None}
    try:
        with open(path, "rb") as f:
            size = f.seek(0, 2)
            for start in ([max(0, size - 262144)] + ([0] if size > 262144 else [])):
                f.seek(start)
                for raw in f.read().splitlines():
                    if b'-title"' not in raw:
                        continue
                    try:
                        d = json.loads(raw)
                    except ValueError:
                        continue
                    if d.get("type") == "custom-title" and d.get("customTitle"):
                        found["custom-title"] = d["customTitle"]
                    elif d.get("type") == "ai-title" and d.get("aiTitle"):
                        found["ai-title"] = d["aiTitle"]
                if found["custom-title"] or found["ai-title"]:
                    break
    except OSError:
        pass
    titles = (found["custom-title"], found["ai-title"])
    _TITLE_CACHE[path] = (now, mtime, titles)
    return titles


def claude_title(payload, cwd):
    """Same name as the terminal tab: /rename title, then Claude's own title, then the folder."""
    custom, ai = transcript_titles(payload.get("transcript_path"))
    return custom or payload.get("session_title") or ai or os.path.basename(cwd.rstrip("/")) or cwd or "claude"


def handle_claude_hook(payload, pid_hint=0):
    ev = payload.get("hook_event_name", "")
    raw_id = payload.get("session_id") or ""
    if not raw_id:
        return
    sid = "claude:" + raw_id
    cwd = payload.get("cwd") or ""
    cur = STORE.get(sid)
    base = {"src": "claude", "cwd": cwd, "transcript": payload.get("transcript_path") or None}
    base["name"] = claude_title(payload, cwd)
    if pid_hint and (cur is None or cur.get("pid") != pid_hint) and pid_is_agent(pid_hint):
        base["pid"] = pid_hint

    is_sub = bool(payload.get("agent_id"))
    tool = payload.get("tool_name") or ""
    tin = payload.get("tool_input") or {}

    if ev == "SessionStart":
        src = payload.get("source", "startup")
        STORE.upsert(sid, state="idle", detail=f"Session {src}", event=f"start ({src})", **base)
    elif ev == "SessionEnd":
        STORE.remove(sid)
    elif ev == "UserPromptSubmit":
        STORE.upsert(sid, state="working", detail="Thinking...", prompt=payload.get("prompt", ""),
                     event="prompt", **base)
    elif ev == "PreToolUse":
        if tool == "AskUserQuestion" and not is_sub:
            qs = tin.get("questions") or []
            q = qs[0].get("question", "") if qs and isinstance(qs[0], dict) else ""
            STORE.upsert(sid, state="waiting", detail="Question: " + q, event="question", **base)
        elif tool == "ExitPlanMode" and not is_sub:
            STORE.upsert(sid, state="waiting", detail="Plan ready for review", event="plan", **base)
        else:
            d = tool_summary(tool, tin)
            if is_sub:
                d = "[sub] " + d
            STORE.upsert(sid, state="working", detail=d, **base)
    elif ev == "PermissionRequest":
        STORE.upsert(sid, state="waiting", detail="Allow " + tool_summary(tool, tin) + "?",
                     event="permission " + tool, **base)
    elif ev in ("PostToolUse", "PostToolUseFailure"):
        # Leaving a permission/question prompt means Claude is moving again.
        if cur is None or cur["state"] != "working":
            STORE.upsert(sid, state="working", detail="Thinking...", **base)
        else:
            STORE.upsert(sid, **base)
    elif ev == "Notification":
        ntype = payload.get("notification_type", "")
        msg = payload.get("message", "")
        if ntype in ("permission_prompt", "elicitation_dialog", "elicitation_url_dialog", "agent_needs_input"):
            STORE.upsert(sid, state="waiting", detail=msg or "Needs your input", event=ntype, **base)
        elif ntype in ("idle_prompt", "agent_completed"):
            if cur is None or cur["state"] == "working":
                STORE.upsert(sid, state="done", detail="Waiting for your next prompt", event=ntype, **base)
        elif not ntype and "permission" in msg.lower():
            STORE.upsert(sid, state="waiting", detail=msg, event="notification", **base)
    elif ev == "Elicitation":
        STORE.upsert(sid, state="waiting", detail=payload.get("message") or "MCP needs input", event="elicitation",
                     **base)
    elif ev == "Stop":
        reply = strip_markdown(payload.get("last_assistant_message") or "")
        STORE.upsert(sid, state="done", detail=first_sentence(reply) or "Finished - your turn", reply=reply,
                     event="done", **base)
    elif ev == "StopFailure":
        STORE.upsert(sid, state="done", detail="Stopped: " + str(payload.get("error_type") or "API error"),
                     event="stop failure", **base)
    elif ev == "SubagentStart":
        n = (cur or {}).get("sub", 0) + 1
        STORE.upsert(sid, sub=n, detail="Agent: " + str(payload.get("agent_type", "subagent")),
                     event="subagent start", **base)
    elif ev == "SubagentStop":
        n = max(0, (cur or {}).get("sub", 0) - 1)
        STORE.upsert(sid, sub=n, event="subagent done", **base)
    elif ev == "PreCompact":
        STORE.upsert(sid, state="working", detail="Compacting context...", event="compact", **base)
    else:
        STORE.upsert(sid, **base)


CLAUDE_EVENTS = ["SessionStart", "SessionEnd", "UserPromptSubmit", "PreToolUse", "PostToolUse",
                 "PostToolUseFailure", "PermissionRequest", "Notification", "Stop", "StopFailure",
                 "SubagentStart", "SubagentStop", "PreCompact", "Elicitation"]


def transcript_interrupted(path, after):
    """Esc does not fire any hook. Detect it from the transcript tail instead."""
    try:
        if not path or os.path.getmtime(path) < after:
            return False
        with open(path, "rb") as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - 16384))
            tail = f.read().decode("utf-8", "replace").splitlines()
    except OSError:
        return False
    for line in reversed(tail[-6:]):
        if '"type":"user"' not in line.replace(" ", ""):
            continue
        return "[Request interrupted by user" in line
    return False


# ---------------------------------------------------------------- generic + codex adapters

def handle_generic(ev):
    """{source, id, state, title?, detail?, cwd?, prompt?, reply?}; state 'ended' removes it."""
    src = ascii_fold(ev.get("source") or "agent", 16).lower() or "agent"
    rid = str(ev.get("id") or ev.get("session") or "default")
    sid = f"{src}:{rid}"
    state = ev.get("state", "working")
    if state == "ended":
        STORE.remove(sid)
        return
    if state == "needs_input":
        state = "waiting"
    if state not in STATES:
        raise ValueError(f"state must be one of {STATES + ('ended',)}")
    cur = STORE.get(sid)
    fields = {"src": src, "detail": ev.get("detail"), "cwd": ev.get("cwd"), "prompt": ev.get("prompt"),
              "reply": ev.get("reply")}
    if ev.get("title") or cur is None:
        fields["name"] = ev.get("title") or os.path.basename((ev.get("cwd") or "").rstrip("/")) or rid
    if ev.get("pid"):
        fields["pid"] = int(ev["pid"])
    if ev.get("sub") is not None:
        fields["sub"] = int(ev["sub"])
    STORE.upsert(sid, state=state, event=ev.get("event") or state, **fields)
    if ev.get("age"):  # backdate (demo data, replays)
        STORE.upsert(sid, since=time.time() - float(ev["age"]), act=time.time() - float(ev["age"]), quiet=True)


def handle_codex_notify(payload):
    """Codex CLI `notify` program hook: called with a JSON argument per event."""
    if payload.get("type") != "agent-turn-complete":
        return
    msgs = payload.get("input-messages") or []
    handle_generic({"source": "codex", "id": payload.get("thread-id") or payload.get("turn-id") or "codex",
                    "state": "done", "cwd": payload.get("cwd"), "detail": "Finished - your turn",
                    "prompt": msgs[-1] if msgs else None,
                    "reply": payload.get("last-assistant-message") or ""})


# ---------------------------------------------------------------- focus the session's terminal

ITERM_FOCUS = """
tell application id "com.googlecode.iterm2"
  repeat with w in windows
    repeat with t in tabs of w
      repeat with s in sessions of t
        if tty of s is "%s" then
          tell w to select
          tell t to select
          tell s to select
          activate
          return "ok"
        end if
      end repeat
    end repeat
  end repeat
  return "notfound"
end tell
"""

TERMINAL_FOCUS = """
tell application "Terminal"
  repeat with w in windows
    repeat with t in tabs of w
      if tty of t is "%s" then
        set selected of t to true
        set index of w to 1
        activate
        return "ok"
      end if
    end repeat
  end repeat
  return "notfound"
end tell
"""


def _ps(pid, fields):
    spec = ",".join(f + "=" for f in fields.split(","))
    out = subprocess.run(["ps", "-o", spec, "-p", str(pid)], capture_output=True, text=True, timeout=2)
    return out.stdout.strip()


def terminal_of(pid):
    """(tty device, .app bundle path) of the terminal a process runs in."""
    tty = _ps(pid, "tty")
    tty = "/dev/" + tty if tty and tty not in ("??", "-") else None
    bundle, p = None, pid
    for _ in range(25):
        line = _ps(p, "ppid,comm")
        if not line:
            break
        ppid, _, comm = line.strip().partition(" ")
        m = re.search(r"^(.*?\.app)/Contents/MacOS/", comm.strip())
        if m:
            bundle = m.group(1)
            break
        p = int(ppid)
        if p <= 1:
            break
    return tty, bundle


def focus_session(sid):
    """Bring the terminal tab of a session to the front. Returns (ok, message)."""
    s = STORE.get(STORE.resolve(sid))
    if not s or not s.get("pid"):
        return False, "unknown process"
    if sys.platform != "darwin":
        return False, "macOS only"
    tty, bundle = terminal_of(s["pid"])
    if not bundle:
        return False, "no app"
    app = os.path.basename(bundle)[:-4]
    script = None
    if tty and "iterm" in app.lower():
        script = ITERM_FOCUS % tty
    elif tty and app == "Terminal":
        script = TERMINAL_FOCUS % tty
    if script:
        r = subprocess.run(["osascript", "-e", script], capture_output=True, text=True, timeout=8)
        if r.stdout.strip() == "ok":
            return True, app
        dbg("focus:", r.stdout.strip(), r.stderr.strip()[:200])
        if "not allowed" in r.stderr or "-1743" in r.stderr:
            return False, "allow automation"
    # other terminals / IDEs: at least bring the app forward
    r = subprocess.run(["open", "-a", bundle], capture_output=True, timeout=8)
    return r.returncode == 0, app


# ---------------------------------------------------------------- device protocol

def wire_session(s):
    now = time.time()
    return {"t": "s", "id": s["id"][:40], "src": ascii_fold(s.get("src"), 10), "name": ascii_fold(s.get("name"), 40),
            "st": s["state"], "det": ascii_fold(s.get("detail"), 96), "age": int(max(0, now - s["since"])),
            "act": int(max(0, now - s.get("act", s["updated"]))),
            "sub": int(s.get("sub") or 0), "ack": 1 if s.get("ack") else 0,
            "fo": 1 if s.get("pid") and sys.platform == "darwin" else 0,
            "cwd": ascii_fold(s.get("cwd", "").replace(os.path.expanduser("~"), "~", 1), 96),
            "pr": ascii_fold(s.get("prompt"), 200), "rp": ascii_fold(strip_markdown(s.get("reply") or ""), 300)}


def tz_offset():
    return time.localtime().tm_gmtoff


def host_label():
    name = os.environ.get("AGENTTALLY_NAME") or socket.gethostname().split(".")[0]
    return ascii_fold(name, 24)


class DeviceLink:
    """One connected display (serial or simulator socket). Thread-safe send()."""

    def __init__(self, name):
        self.name = name
        self.q = queue.Queue(maxsize=512)
        self.alive = True
        self.info = {}
        self.last_rx = time.time()
        self.last_hello = 0.0
        self.kind = "sim"

    def send(self, obj):
        if not self.alive:
            return
        try:
            self.q.put_nowait(json.dumps(obj, separators=(",", ":"), ensure_ascii=False) + "\n")
        except queue.Full:
            pass

    def send_snapshot(self):
        self.send({"t": "clr", "now": int(time.time()), "tz": tz_offset()})
        for s in sorted(STORE.snapshot(), key=lambda s: s["updated"]):
            self.send(wire_session(s))

    def on_line(self, line):
        self.last_rx = time.time()
        line = line.strip()
        dbg(f"[{self.name}] << {line[:120]}")
        if not line.startswith("{"):
            if line:
                dbg(f"[{self.name}] {line}")
            return
        try:
            msg = json.loads(line)
        except ValueError:
            dbg(f"[{self.name}] bad json: {line[:80]}")
            return
        t = msg.get("t")
        if t == "hello":
            if self.info and time.time() - self.last_hello < 2:
                return
            self.last_hello = time.time()
            self.info = dict(msg, cfg=self.info.get("cfg"))
            log(f"[{self.name}] device: {msg.get('board')} fw {msg.get('fw')} {msg.get('w')}x{msg.get('h')}")
            self.send({"t": "hi", "v": PROTO, "host": host_label(), "now": int(time.time()), "tz": tz_offset()})
            self.send_snapshot()
            self.send({"t": "cfg"})  # ask for the current settings (web device tools)
        elif t == "ack":
            STORE.ack(STORE.resolve(str(msg.get("id", ""))))
        elif t == "cfg":
            self.info["cfg"] = msg
        elif t == "focus":
            sid = str(msg.get("id", ""))

            def run(link=self):
                ok, m = focus_session(sid)
                log(f"focus {sid[:24]}: {'ok' if ok else 'failed'} ({m})")
                link.send({"t": "focused", "id": sid[:40], "ok": 1 if ok else 0, "m": ascii_fold(m, 24)})

            threading.Thread(target=run, daemon=True).start()
        elif t == "log":
            log(f"[{self.name}] {msg.get('m')}")


class Hub:
    """Fans store changes out to every connected display."""

    def __init__(self):
        self.links = []
        self.lock = threading.Lock()
        STORE.listeners.append(self.on_change)

    def add(self, link):
        with self.lock:
            self.links.append(link)

    def drop(self, link):
        link.alive = False
        with self.lock:
            if link in self.links:
                self.links.remove(link)

    def on_change(self, kind, payload):
        msg = wire_session(payload) if kind == "upsert" else {"t": "d", "id": payload[:40]}
        with self.lock:
            for l in self.links:
                l.send(msg)

    def broadcast(self, obj):
        with self.lock:
            for l in self.links:
                l.send(obj)


HUB = Hub()


def pump(link, read_fn, write_fn):
    """Run reader+writer for a link until either side fails."""
    def writer():
        while link.alive:
            try:
                data = link.q.get(timeout=0.5)
            except queue.Empty:
                continue
            try:
                dbg(f"[{link.name}] >> {data.strip()[:120]}")
                write_fn(data.encode())
            except Exception as e:
                dbg(f"[{link.name}] write failed: {e}")
                link.alive = False

    wt = threading.Thread(target=writer, daemon=True)
    wt.start()
    buf = b""
    try:
        while link.alive:
            chunk = read_fn()
            if chunk is None:
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                link.on_line(line.decode("utf-8", "replace"))
            if len(buf) > 8192:
                buf = b""
    except Exception as e:
        dbg(f"[{link.name}] read failed: {e}")
    link.alive = False
    HUB.drop(link)


# ---------------------------------------------------------------- serial transport

class PosixSerial:
    """Minimal termios serial port that does NOT pulse DTR/RTS (which would reset the ESP32)."""

    def __init__(self, path, baud):
        import termios
        import fcntl
        self.path = path
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            fcntl.ioctl(self.fd, getattr(termios, "TIOCEXCL", 0x2000740D))
        except OSError:
            pass
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = 0                                   # iflag
        attrs[1] = 0                                   # oflag
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL   # cflag (no HUPCL: keep DTR on close)
        attrs[3] = 0                                   # lflag
        speed = getattr(termios, f"B{baud}")
        attrs[4] = attrs[5] = speed
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        # Release DTR and RTS together: EN high, GPIO0 high -> normal run, no reset.
        bits = getattr(termios, "TIOCM_DTR", 2) | getattr(termios, "TIOCM_RTS", 4)
        tiocmbic = getattr(termios, "TIOCMBIC", 0x8004746B if sys.platform == "darwin" else 0x5417)
        try:
            fcntl.ioctl(self.fd, tiocmbic, bits.to_bytes(4, sys.byteorder))
        except OSError:
            pass
        fl = fcntl.fcntl(self.fd, fcntl.F_GETFL)
        fcntl.fcntl(self.fd, fcntl.F_SETFL, fl & ~os.O_NONBLOCK)

    def read(self):
        import select
        r, _, _ = select.select([self.fd], [], [], 0.5)
        if not r:
            return b""
        data = os.read(self.fd, 1024)
        if data == b"":
            return None       # device unplugged
        return data

    def write(self, data):
        view = memoryview(data)
        while view:
            n = os.write(self.fd, view)
            view = view[n:]

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass


class PySerial:
    def __init__(self, path, baud):
        import serial  # type: ignore
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = path, baud, 0.5
        s.dtr = False
        s.rts = False
        s.open()
        self.s = s

    def read(self):
        return self.s.read(1024)

    def write(self, data):
        self.s.write(data)

    def close(self):
        self.s.close()


def open_serial(path, baud):
    try:
        import serial  # noqa: F401
        return PySerial(path, baud)
    except ImportError:
        if os.name == "nt":
            raise RuntimeError("install pyserial on Windows: pip install pyserial")
        return PosixSerial(path, baud)


def usb_ids_present():
    """macOS: which USB vendor/product ids are attached (to vet cu.usbmodem* ports)."""
    if sys.platform != "darwin":
        return set()
    try:
        out = subprocess.run(["ioreg", "-p", "IOUSB", "-l", "-w0"], capture_output=True, text=True, timeout=3).stdout
    except Exception:
        return set()
    ids, vid = set(), None
    for line in out.splitlines():
        m = re.search(r'"idVendor" = (\d+)', line)
        if m:
            vid = int(m.group(1))
        m = re.search(r'"idProduct" = (\d+)', line)
        if m and vid is not None:
            ids.add((vid, int(m.group(1))))
    return ids


def candidate_ports():
    try:
        from serial.tools import list_ports  # type: ignore
        return [p.device for p in list_ports.comports() if (p.vid, p.pid) in KNOWN_USB]
    except ImportError:
        pass
    if sys.platform == "darwin":
        ports = sorted(glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/cu.wchusbserial*") +
                       glob.glob("/dev/cu.SLAB_USBtoUART*"))
        if usb_ids_present() & KNOWN_USB:
            ports += sorted(p for p in glob.glob("/dev/cu.usbmodem*") if p not in ports)
        return ports
    return sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))


# The web flasher asks the bridge to let go of the serial port while it installs
# firmware; the pause ends on its own so a closed browser tab can't keep it.
SERIAL_PAUSED_UNTIL = 0.0


def serial_paused():
    return time.time() < SERIAL_PAUSED_UNTIL


def serial_release(seconds):
    global SERIAL_PAUSED_UNTIL
    SERIAL_PAUSED_UNTIL = time.time() + max(5.0, min(float(seconds), 600.0))
    log(f"serial port released for {int(SERIAL_PAUSED_UNTIL - time.time())}s (firmware update)")


def serial_resume():
    global SERIAL_PAUSED_UNTIL
    if SERIAL_PAUSED_UNTIL:
        log("serial port: resuming")
    SERIAL_PAUSED_UNTIL = 0.0


def serial_manager(fixed_port, baud, stop):
    """Keep one link per attached display; rescan every few seconds."""
    busy = {}
    while not stop.is_set():
        if serial_paused():
            stop.wait(1)
            continue
        ports = [fixed_port] if fixed_port else candidate_ports()
        for path in ports:
            if path in busy and busy[path].is_alive():
                continue
            busy[path] = threading.Thread(target=serial_session, args=(path, baud), daemon=True)
            busy[path].start()
        stop.wait(3)


def serial_session(path, baud):
    try:
        port = open_serial(path, baud)
    except Exception as e:
        dbg(f"cannot open {path}: {e}")
        time.sleep(5)
        return
    link = DeviceLink(os.path.basename(path))
    link.kind = "serial"
    HUB.add(link)
    # Ask the device to introduce itself; a board that never answers is not ours.
    started = last_who = time.time()
    link.send({"t": "who", "v": PROTO})

    def read():
        nonlocal last_who
        now = time.time()
        if serial_paused():
            return None  # closes the port for the web flasher
        if not link.info:
            if now - started > 8:
                dbg(f"{path}: no AgentTally hello, ignoring for now")
                return None
            if now - last_who > 2.5:  # the board may still be booting after the port opened
                last_who = now
                link.send({"t": "who", "v": PROTO})
        return port.read()

    pump(link, read, port.write)
    port.close()
    if link.info:
        log(f"[{link.name}] disconnected")
        time.sleep(1)
    else:
        time.sleep(10)


# ---------------------------------------------------------------- simulator transport (TCP)

def sim_server(port, stop):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(4)
    srv.settimeout(1)
    while not stop.is_set():
        try:
            conn, addr = srv.accept()
        except socket.timeout:
            continue
        conn.settimeout(0.5)
        link = DeviceLink(f"sim:{addr[1]}")
        HUB.add(link)

        def read(c=conn):
            try:
                d = c.recv(4096)
                return d if d else None
            except socket.timeout:
                return b""

        threading.Thread(target=lambda c=conn, l=link, r=read: (pump(l, r, c.sendall), c.close()),
                         daemon=True).start()


# ---------------------------------------------------------------- housekeeping

def sweeper(stop, done_to_idle_min):
    last_ping = 0
    while not stop.wait(5):
        now = time.time()
        for s in STORE.snapshot():
            # Session process gone without SessionEnd (terminal closed, killed...)
            if s.get("pid") and not pid_is_agent(s["pid"]):
                log(f"{s['id']}: process {s['pid']} gone")
                STORE.remove(s["id"])
                continue
            # keep names in step with the terminal title (/rename, or Claude's own title)
            if s["src"] == "claude" and s.get("transcript"):
                title = claude_title({"transcript_path": s["transcript"]}, s.get("cwd") or "")
                if title and title != s.get("name"):
                    STORE.upsert(s["id"], name=title, quiet=True)
            if s["src"] == "claude" and s["state"] in ("working", "waiting") and now - s["updated"] > 4:
                if transcript_interrupted(s.get("transcript"), s["updated"] - 1):
                    STORE.upsert(s["id"], state="idle", detail="Interrupted", event="interrupted")
                    continue
            if s["state"] == "done" and done_to_idle_min and now - s["since"] > done_to_idle_min * 60:
                STORE.upsert(s["id"], state="idle", ack=True, quiet=True)
            # Never heard of again (no pid tracking): forget after 12h.
            if now - s["updated"] > 12 * 3600 and not s.get("pid"):
                STORE.remove(s["id"])
        if now - last_ping > 5:
            last_ping = now
            HUB.broadcast({"t": "ping", "now": int(now), "tz": tz_offset()})
        try:
            STORE.save()
        except Exception as e:
            log("save failed:", e)


# ---------------------------------------------------------------- HTTP API

DASHBOARD = """<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width">
<title>AgentTally</title><style>
:root{color-scheme:dark}body{background:#0e0f13;color:#e8e6e3;font:14px/1.4 system-ui;margin:0;padding:16px;max-width:520px}
h1{font-size:13px;letter-spacing:.12em;color:#d97757;margin:0 0 12px}.c{background:#1a1c23;border-radius:10px;
padding:10px 12px;margin:0 0 8px;border-left:4px solid #6b7280}.n{font-weight:600}.d{color:#8a8f98;font-size:13px}
.working{border-color:#f2b33d}.waiting{border-color:#ff5a5f}.done{border-color:#3fcf8e}.t{float:right;color:#8a8f98}
</style><h1>AGENTTALLY</h1><div id=l></div><script>
async function r(){const j=await (await fetch('/api/sessions')).json();const o={waiting:0,done:1,working:2,idle:3};
j.sessions.sort((a,b)=>o[a.state]-o[b.state]||b.since-a.since);document.getElementById('l').innerHTML=
j.sessions.map(s=>`<div class="c ${s.state}"><span class=t>${s.state} · ${Math.round((Date.now()/1e3-s.since)/60)}m</span>
<div class=n>${esc(s.name)}</div><div class=d>${esc(s.detail)}</div></div>`).join('')||'<p class=d>No active agents.</p>'}
const esc=s=>(s||'').replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));r();setInterval(r,1500)</script>"""


WEB_ORIGINS = {"https://ibrahim-dogan.github.io"} | set(filter(None, os.environ.get("AGENTTALLY_ORIGINS", "").split(",")))


def origin_allowed(origin):
    """The web flasher (GitHub Pages or a local copy) may talk to the bridge from the browser."""
    if not origin:
        return False
    if origin in WEB_ORIGINS:
        return True
    return bool(re.match(r"^http://(127\.0\.0\.1|localhost)(:\d+)?$", origin))


class Api(BaseHTTPRequestHandler):
    server_version = "AgentTally/" + VERSION

    def log_message(self, fmt, *args):
        dbg("http", fmt % args)

    def _cors(self):
        origin = self.headers.get("Origin")
        if origin_allowed(origin):
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Vary", "Origin")

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self._cors()
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        origin = self.headers.get("Origin")
        if not origin_allowed(origin):
            self.send_response(403)
            self.end_headers()
            return
        self.send_response(204)
        self._cors()
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Allow-Private-Network", "true")
        self.send_header("Access-Control-Max-Age", "600")
        self.end_headers()

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(min(n, 2_000_000)) if n else b""
        return json.loads(raw or b"{}")

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            body = DASHBOARD.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path.startswith("/api/sessions"):
            sess = [{k: v for k, v in s.items() if k != "transcript"} for s in STORE.snapshot()]
            self._json(200, {"sessions": sess, "devices": [l.info for l in HUB.links if l.info]})
        elif self.path == "/api/health":
            self._json(200, {"ok": True, "version": VERSION, "serial_paused": serial_paused()})
        elif self.path.startswith("/api/device"):
            self._json(200, {"devices": [dict(l.info, kind=l.kind) for l in HUB.links if l.info],
                             "serial_paused": serial_paused()})
        else:
            self._json(404, {"error": "not found"})

    def do_POST(self):
        # Only JSON bodies: a browser can't send those cross-site without a CORS preflight,
        # which only the web flasher's origins pass. Blocks drive-by requests from other sites.
        if not (self.headers.get("Content-Type") or "").startswith("application/json"):
            return self._json(415, {"error": "Content-Type must be application/json"})
        origin = self.headers.get("Origin")
        if origin and not origin_allowed(origin):
            return self._json(403, {"error": "origin not allowed"})
        try:
            data = self._body()
            if self.path.startswith("/hook/claude"):
                pid = int(self.headers.get("X-Agent-Pid") or 0)
                dbg("hook", data.get("hook_event_name"), data.get("tool_name", ""))
                handle_claude_hook(data, pid)
            elif self.path.startswith("/api/event"):
                for ev in data if isinstance(data, list) else [data]:
                    handle_generic(ev)
            elif self.path.startswith("/hook/codex"):
                handle_codex_notify(data)
            elif self.path.startswith("/api/ack"):
                STORE.ack(str(data.get("id", "")))
            elif self.path.startswith("/api/serial/release"):
                serial_release(data.get("seconds", 120))
            elif self.path.startswith("/api/serial/resume"):
                serial_resume()
            elif self.path.startswith("/api/device"):
                HUB.broadcast(dict(data, t="cfg"))
            elif self.path.startswith("/api/clear"):
                for s in STORE.snapshot():
                    STORE.remove(s["id"])
            else:
                return self._json(404, {"error": "not found"})
            self._json(200, {"ok": True})
        except Exception as e:
            self._json(400, {"error": str(e)})


# ---------------------------------------------------------------- commands

def cmd_serve(a):
    global VERBOSE
    VERBOSE = a.verbose
    os.makedirs(HOME, exist_ok=True)
    STORE.load()
    stop = threading.Event()
    try:
        httpd = ThreadingHTTPServer(("127.0.0.1", a.http_port), Api)
    except OSError as e:
        sys.exit(f"port {a.http_port} busy - is agenttally already running? ({e})")
    httpd.daemon_threads = True
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    threading.Thread(target=sweeper, args=(stop, a.done_to_idle), daemon=True).start()
    if not a.no_serial:
        threading.Thread(target=serial_manager, args=(a.port, a.baud, stop), daemon=True).start()
    if a.sim_port:
        threading.Thread(target=sim_server, args=(a.sim_port, stop), daemon=True).start()
    log(f"AgentTally bridge {VERSION} - http://127.0.0.1:{a.http_port}  sim tcp:{a.sim_port or 'off'}")

    def bye(*_):
        stop.set()

    signal.signal(signal.SIGTERM, bye)
    signal.signal(signal.SIGINT, bye)
    while not stop.wait(1):
        pass
    STORE.dirty_save = True
    STORE.save()
    httpd.shutdown()


def api(path, obj=None, port=HTTP_PORT):
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}",
                                 data=json.dumps(obj).encode() if obj is not None else None,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=3) as r:
        return json.loads(r.read())


def cmd_status(a):
    try:
        j = api("/api/sessions", port=a.http_port)
    except Exception:
        sys.exit("bridge not running (start it with: agenttally.py serve)")
    devs = j.get("devices") or []
    print("devices:", ", ".join(f"{d.get('board')} fw{d.get('fw')}" for d in devs) or "none")
    for s in sorted(j["sessions"], key=lambda s: (STATE_RANK[s["state"]], -s["since"])):
        age = int(time.time() - s["since"])
        print(f"  {s['state']:8} {s['name'][:28]:28} {age // 60:4}m  {s['detail'][:60]}")
    if not j["sessions"]:
        print("  (no sessions)")


def cmd_send(a):
    ev = {"source": a.source, "id": a.id, "state": a.state, "title": a.title, "detail": a.detail,
          "cwd": a.cwd or os.getcwd()}
    api("/api/event", {k: v for k, v in ev.items() if v is not None}, port=a.http_port)


def cmd_hook(a):
    """Hook entry for systems without curl: forwards stdin to the bridge, never fails the hook."""
    try:
        data = sys.stdin.buffer.read()
        req = urllib.request.Request(f"http://127.0.0.1:{a.http_port}/hook/{a.kind}", data=data,
                                     headers={"Content-Type": "application/json", "X-Agent-Pid": str(os.getppid())})
        urllib.request.urlopen(req, timeout=1).read()
    except Exception:
        pass


def cmd_codex_notify(a):
    try:
        api("/hook/codex", json.loads(a.payload), port=a.http_port)
    except Exception:
        pass


def cmd_device(a):
    cfg = {}
    for kv in a.settings:
        k, _, v = kv.partition("=")
        cfg[k] = int(v) if v.lstrip("-").isdigit() else v
    api("/api/device", cfg, port=a.http_port)


DEMO = [
    {"source": "claude", "id": "d1", "title": "agent-tally", "state": "waiting", "detail": "Allow $ pio run -t upload?",
     "cwd": "~/code/agent-tally", "age": 12, "prompt": "flash the new firmware to the board"},
    {"source": "claude", "id": "d2", "title": "web-shop", "state": "done", "detail": "All 48 tests pass.",
     "cwd": "~/code/web-shop", "age": 95, "prompt": "why is the checkout test flaky?",
     "reply": "The retry test shared a mock clock with the other tests, so timing leaked between them. "
              "I gave every test its own clock and ran the suite 20 times: all 48 tests pass."},
    {"source": "claude", "id": "d3", "title": "api-gateway", "state": "working", "detail": "Edit routes/limits.ts",
     "cwd": "~/work/api-gateway", "prompt": "add per-key rate limiting", "sub": 2, "age": 260},
    {"source": "codex", "id": "d4", "title": "infra", "state": "working", "detail": "$ terraform plan",
     "cwd": "~/work/infra", "age": 540},
    {"source": "claude", "id": "d5", "title": "café-menü", "state": "idle", "detail": "Waiting for your next prompt",
     "cwd": "~/code/cafe-menu", "age": 2700},
]


def cmd_demo(a):
    for ev in DEMO:
        api("/api/event", ev, port=a.http_port)
    print(f"sent {len(DEMO)} demo sessions; remove with: agenttally.py demo --clear")


def cmd_demo_clear(a):
    for ev in DEMO:
        api("/api/event", {"source": ev["source"], "id": ev["id"], "state": "ended"}, port=a.http_port)


# ---------------------------------------------------------------- install / uninstall

def install_dir_script():
    return os.path.join(HOME, "agenttally.py")


def hook_command(port):
    url = f"http://127.0.0.1:{port}/hook/claude"
    if shutil.which("curl") and os.name != "nt":
        return (f"curl -s -m 1 -X POST -H 'Content-Type: application/json' -H \"X-Agent-Pid: $PPID\" "
                f"--data-binary @- {url}?src={HOOK_MARK} >/dev/null 2>&1 || true")
    return f'"{sys.executable}" "{install_dir_script()}" hook claude  # {HOOK_MARK}'


def settings_path():
    return os.path.join(os.environ.get("CLAUDE_CONFIG_DIR") or os.path.expanduser("~/.claude"), "settings.json")


def load_settings():
    p = settings_path()
    try:
        with open(p) as f:
            return json.load(f)
    except FileNotFoundError:
        return {}


def write_settings(data):
    p = settings_path()
    os.makedirs(os.path.dirname(p), exist_ok=True)
    if os.path.exists(p):
        bak = f"{p}.agent-tally-{time.strftime('%Y%m%d-%H%M%S')}.bak"
        shutil.copy2(p, bak)
        print("backup:", bak)
    tmp = p + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
    os.replace(tmp, p)


def strip_our_hooks(settings):
    hooks = settings.get("hooks") or {}
    for ev in list(hooks):
        groups = []
        for g in hooks[ev]:
            hs = [h for h in g.get("hooks", []) if HOOK_MARK not in str(h.get("command", ""))]
            if hs:
                groups.append(dict(g, hooks=hs))
        if groups:
            hooks[ev] = groups
        else:
            del hooks[ev]
    if not hooks:
        settings.pop("hooks", None)
    return settings


def install_hooks(port):
    s = strip_our_hooks(load_settings())
    hooks = s.setdefault("hooks", {})
    cmd = hook_command(port)
    for ev in CLAUDE_EVENTS:
        hooks.setdefault(ev, []).append({"hooks": [{"type": "command", "command": cmd, "timeout": 3}]})
    write_settings(s)
    print(f"Claude Code hooks installed in {settings_path()} ({len(CLAUDE_EVENTS)} events)")


LAUNCHD_LABEL = "dev.agenttally.bridge"


def service_install(script):
    py = sys.executable
    if sys.platform == "darwin":
        plist = os.path.expanduser(f"~/Library/LaunchAgents/{LAUNCHD_LABEL}.plist")
        os.makedirs(os.path.dirname(plist), exist_ok=True)
        with open(plist, "w") as f:
            f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>{LAUNCHD_LABEL}</string>
  <key>ProgramArguments</key><array><string>{py}</string><string>{script}</string><string>serve</string></array>
  <key>RunAtLoad</key><true/><key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>{HOME}/bridge.log</string>
  <key>StandardErrorPath</key><string>{HOME}/bridge.log</string>
</dict></plist>
""")
        subprocess.run(["launchctl", "unload", plist], capture_output=True)
        subprocess.run(["launchctl", "load", "-w", plist], check=False)
        print("autostart: launchd agent", plist)
    elif sys.platform.startswith("linux") and shutil.which("systemctl"):
        unit = os.path.expanduser("~/.config/systemd/user/agent-tally.service")
        os.makedirs(os.path.dirname(unit), exist_ok=True)
        with open(unit, "w") as f:
            f.write(f"[Unit]\nDescription=AgentTally bridge\n\n[Service]\nExecStart={py} {script} serve\n"
                    f"Restart=always\n\n[Install]\nWantedBy=default.target\n")
        subprocess.run(["systemctl", "--user", "daemon-reload"])
        subprocess.run(["systemctl", "--user", "enable", "--now", "agent-tally.service"])
        print("autostart: systemd user unit", unit)
        print("note: on Linux add yourself to the 'dialout' group for serial access")
    else:
        print(f"no autostart on this OS - run manually: {py} {script} serve")


def service_uninstall():
    link = os.path.expanduser("~/.local/bin/agenttally")
    if os.path.islink(link):
        os.remove(link)
    if sys.platform == "darwin":
        plist = os.path.expanduser(f"~/Library/LaunchAgents/{LAUNCHD_LABEL}.plist")
        if os.path.exists(plist):
            subprocess.run(["launchctl", "unload", "-w", plist], capture_output=True)
            os.remove(plist)
            print("removed", plist)
    elif shutil.which("systemctl"):
        subprocess.run(["systemctl", "--user", "disable", "--now", "agent-tally.service"], capture_output=True)
        unit = os.path.expanduser("~/.config/systemd/user/agent-tally.service")
        if os.path.exists(unit):
            os.remove(unit)
            print("removed", unit)


def cmd_install(a):
    os.makedirs(HOME, exist_ok=True)
    me = os.path.abspath(__file__)
    target = install_dir_script()
    if me != target:
        shutil.copy2(me, target)
    os.chmod(target, 0o755)
    # `agenttally` on the PATH for convenience
    bindir = os.path.expanduser("~/.local/bin")
    try:
        os.makedirs(bindir, exist_ok=True)
        link = os.path.join(bindir, "agenttally")
        if os.path.islink(link) or not os.path.exists(link):
            if os.path.islink(link):
                os.remove(link)
            os.symlink(target, link)
            on_path = bindir in os.environ.get("PATH", "").split(os.pathsep)
            print(f"command: {link}" + ("" if on_path else f"  (add {bindir} to your PATH)"))
    except OSError:
        pass
    if not a.no_hooks:
        install_hooks(a.http_port)
    if not a.no_service:
        service_install(target)
    print("done. Restart running Claude Code sessions to pick up the hooks.")


def cmd_service(a):
    """start / stop / restart the autostart service (e.g. to free the serial port for flashing)."""
    if sys.platform == "darwin":
        plist = os.path.expanduser(f"~/Library/LaunchAgents/{LAUNCHD_LABEL}.plist")
        if not os.path.exists(plist):
            sys.exit("service not installed (run: agenttally.py install)")
        if a.action in ("stop", "restart"):
            subprocess.run(["launchctl", "unload", plist], capture_output=True)
        if a.action in ("start", "restart"):
            subprocess.run(["launchctl", "load", "-w", plist], capture_output=True)
    elif shutil.which("systemctl"):
        subprocess.run(["systemctl", "--user", a.action, "agent-tally.service"])
    else:
        sys.exit("no service manager found")
    print(f"bridge service: {a.action}")


def cmd_uninstall(a):
    write_settings(strip_our_hooks(load_settings()))
    print("Claude Code hooks removed")
    service_uninstall()


def main():
    p = argparse.ArgumentParser(prog="agenttally", description="AgentTally host bridge " + VERSION)
    p.add_argument("--http-port", type=int, default=int(os.environ.get("AGENTTALLY_PORT", HTTP_PORT)))
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("serve", help="run the bridge")
    s.add_argument("--port", help="serial port (default: auto-detect)")
    s.add_argument("--baud", type=int, default=BAUD)
    s.add_argument("--sim-port", type=int, default=SIM_PORT, help="TCP port for the simulator (0 = off)")
    s.add_argument("--no-serial", action="store_true")
    s.add_argument("--done-to-idle", type=float, default=30, help="minutes until an unseen 'done' turns idle")
    s.add_argument("-v", "--verbose", action="store_true")
    s.set_defaults(fn=cmd_serve)

    sub.add_parser("status", help="list sessions").set_defaults(fn=cmd_status)
    sub.add_parser("version", help="print the version").set_defaults(fn=lambda a: print(VERSION))

    s = sub.add_parser("send", help="push a generic agent event")
    s.add_argument("--source", default="agent")
    s.add_argument("--id", default="default")
    s.add_argument("--state", required=True, choices=list(STATES) + ["needs_input", "ended"])
    s.add_argument("--title")
    s.add_argument("--detail")
    s.add_argument("--cwd")
    s.set_defaults(fn=cmd_send)

    s = sub.add_parser("hook", help="(internal) forward hook stdin to the bridge")
    s.add_argument("kind", nargs="?", default="claude")
    s.set_defaults(fn=cmd_hook)

    s = sub.add_parser("codex-notify", help="(internal) Codex CLI notify program")
    s.add_argument("payload")
    s.set_defaults(fn=cmd_codex_notify)

    s = sub.add_parser("device", help="change device settings, e.g. brightness=180 invert=1 flip=1")
    s.add_argument("settings", nargs="+")
    s.set_defaults(fn=cmd_device)

    s = sub.add_parser("demo", help="inject demo sessions")
    s.add_argument("--clear", action="store_true")
    s.set_defaults(fn=lambda a: cmd_demo_clear(a) if a.clear else cmd_demo(a))

    s = sub.add_parser("install", help="install hooks + autostart")
    s.add_argument("--no-hooks", action="store_true")
    s.add_argument("--no-service", action="store_true")
    s.set_defaults(fn=cmd_install)

    sub.add_parser("uninstall", help="remove hooks + autostart").set_defaults(fn=cmd_uninstall)

    s = sub.add_parser("service", help="start/stop the autostart bridge (stop it before flashing)")
    s.add_argument("action", choices=["start", "stop", "restart"])
    s.set_defaults(fn=cmd_service)

    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
