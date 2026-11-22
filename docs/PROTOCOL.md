# Protocols

## Bridge HTTP API (127.0.0.1:47321)

| Method | Path | Body | Purpose |
|---|---|---|---|
| POST | `/hook/claude` | Claude Code hook JSON (stdin of the hook) | Header `X-Agent-Pid` = the `claude` process (`$PPID` in the hook shell) |
| POST | `/api/event` | `{source, id, state, title?, detail?, cwd?, prompt?, reply?, sub?, age?, pid?}` or a list of them | Generic agents. `state`: `working`, `waiting` (or `needs_input`), `done`, `idle`, `ended` (removes) |
| POST | `/hook/codex` | Codex `notify` JSON | Used by `agenttally.py codex-notify` |
| POST | `/api/ack` | `{id}` | Mark a finished session as seen |
| POST | `/api/device` | `{brightness?, gamma?, invert?, ...}` | Forwarded to every display as a `cfg` message |
| POST | `/api/clear` | `{}` | Forget all sessions |
| POST | `/api/demo-mode` | `{on, host?}` | Demo mode for recordings: hide the real sessions from the displays and play a scripted loop; `host` is the name shown in the footer (`agenttally demo-mode on\|off`) |
| POST | `/api/serial/release` | `{seconds?}` | Let go of the serial port for a firmware update (5..600 s, default 120). Used by the web flasher |
| POST | `/api/serial/resume` | `{}` | End that pause now |
| GET | `/api/sessions` | – | Sessions and connected devices |
| GET | `/api/device` | – | Connected devices with their last reported settings, and `serial_paused` |
| GET | `/api/health` | – | `{ok, version, serial_paused}` |
| GET | `/` | – | Small live HTML view of the same list |

If `pid` is known the session is removed when that process exits. On macOS it also enables the Focus button for that session.

Every POST must carry `Content-Type: application/json`, otherwise the bridge answers 415. Web pages may only call the bridge from the web flasher's origin (`https://ibrahim-dogan.github.io`), from `http://localhost` / `http://127.0.0.1` pages, and from origins listed in the `AGENTTALLY_ORIGINS` environment variable (comma-separated). A POST or CORS preflight with any other `Origin` gets 403; GET responses to other origins carry no CORS header, so a browser won't let the page read them.

## Serial link (USB, 115200 8N1)

Newline-delimited JSON, UTF-8, one object per line, at most 2047 bytes. Lines that don't start with `{` (ROM boot messages) are ignored by both sides. The host opens the port with DTR and RTS released, so opening it doesn't reset the board on most adapters. The display treats the link as alive while it hears something at least every 15 s.

### Host → display

| `t` | Fields | Meaning |
|---|---|---|
| `hi` | `v`, `host`, `now`, `tz` | Answer to `hello`. `now` is Unix time, `tz` the local UTC offset in seconds (used for the clock) |
| `clr` | `now`, `tz` | Forget all sessions (sent before a full snapshot) |
| `s` | `id`, `src`, `name`, `st`, `det`, `age`, `act`, `sub`, `ack`, `fo`, `cwd`, `pr`, `rp` | Insert or update a session. `st` is `working`, `waiting`, `done` or `idle`; `age` is seconds in that state, `act` seconds since its last activity; `ack` = 0 means *done but not seen yet*; `fo` = 1 means the bridge can focus its terminal, so the detail view shows the Focus button. A session that arrives as, or changes to, `waiting` or unseen `done` raises the alert (frame, card glow and LED pulse three times). The display lists waiting sessions first (longest-waiting first), then active ones (working, or done and unseen), then quiet ones; within those two groups the most recently active comes first, re-sorted at most every 30 s |
| `d` | `id` | Remove a session |
| `ping` | `now`, `tz` | Keep-alive, every 5 s |
| `who` | `v` | Ask the display to send `hello` |
| `focused` | `id`, `ok`, `m` | Result of a `focus` request: `ok` 1 or 0, `m` a short text for the button (the app name, or why it failed) |
| `m` | `on`, `x`, `y`, `b` | Mouse pointer (macOS helper): position in display pixels, `b` = 1 while the button is held. `on` = 0 hides the pointer |
| `ms` | `dy` | Mouse wheel: scroll the list by `dy` pixels |
| `mb` | – | Right click: back to the list |
| `cfg` | any of `brightness` (10..255), `autodim`, `invert`, `flip`, `swaprb`, `led`, `sound`, `gamma` (0..4), `panel` (0 auto, 1 ILI9341, 2 ST7789; restarts), `test` (1 shows colour bars), `beep`, `flash` (alert frame), `sleep` + `sleep_from` / `sleep_to` (night hours, local time: screen and LED off, a touch shows the screen for 30 s), `led_rgb` + `led_pat` + `led_ms` + `led_label` (LED test) | Change settings (persisted in NVS). The display answers with its full `cfg` |

Text fields are already trimmed by the bridge (name 40 chars, detail 96, cwd 96, prompt 200, reply 300) and limited to characters the fonts contain.

### Display → host

| `t` | Fields | Meaning |
|---|---|---|
| `hello` | `v`, `fw`, `board`, `panel`, `w`, `h` | On boot, on `who`, and every 3 s while no host is heard |
| `ack` | `id` | The user opened a finished session |
| `focus` | `id` | The user tapped Focus in the detail view; the bridge answers with `focused` |
| `cfg` | current settings | Reply to every `cfg` |
| `log` | `m` | Diagnostics (panel probe result on boot) |

### Simulator

`agenttally.py serve` also listens on TCP `127.0.0.1:47322`. The desktop simulator connects there and speaks exactly the same protocol, so it behaves like a second display.
