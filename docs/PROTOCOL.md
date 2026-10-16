# Protocols

## Bridge HTTP API (127.0.0.1:47321)

| Method | Path | Body | Purpose |
|---|---|---|---|
| POST | `/hook/claude` | Claude Code hook JSON (stdin of the hook) | Header `X-Agent-Pid` = the `claude` process (`$PPID` in the hook shell) |
| POST | `/api/event` | `{source, id, state, title?, detail?, cwd?, prompt?, reply?, sub?, age?, pid?}` or a list of them | Generic agents. `state`: `working`, `waiting` (or `needs_input`), `done`, `idle`, `ended` (removes) |
| POST | `/hook/codex` | Codex `notify` JSON | Used by `agenttally.py codex-notify` |
| POST | `/api/ack` | `{id}` | Mark a finished session as seen |
| POST | `/api/device` | `{brightness?, gamma?, invert?, ...}` | Forwarded to every display as a `cfg` message |
| POST | `/api/clear` | – | Forget all sessions |
| GET | `/api/sessions` | – | Sessions and connected devices |
| GET | `/` | – | Small live HTML view of the same list |

If `pid` is known the session is removed when that process exits.

## Serial link (USB, 115200 8N1)

Newline-delimited JSON, UTF-8, one object per line, at most 2047 bytes. Lines that don't start with `{` (ROM boot messages) are ignored by both sides. The host opens the port with DTR and RTS released, so opening it doesn't reset the board on most adapters. The display treats the link as alive while it hears something at least every 15 s.

### Host → display

| `t` | Fields | Meaning |
|---|---|---|
| `hi` | `v`, `host`, `now`, `tz` | Answer to `hello`. `now` is Unix time, `tz` the local UTC offset in seconds (used for the clock) |
| `clr` | `now`, `tz` | Forget all sessions (sent before a full snapshot) |
| `s` | `id`, `src`, `name`, `st`, `det`, `age`, `act`, `sub`, `ack`, `cwd`, `pr`, `rp` | Insert or update a session. `st` is `working`, `waiting`, `done` or `idle`; `age` is seconds in that state, `act` seconds since its last activity; `ack` = 0 means *done but not seen yet*. The display lists waiting sessions longest-waiting first, the rest most recently active first (re-sorted at most every 30 s) |
| `d` | `id` | Remove a session |
| `ping` | `now`, `tz` | Keep-alive, every 5 s |
| `who` | `v` | Ask the display to send `hello` |
| `cfg` | any of `brightness` (10..255), `autodim`, `invert`, `flip`, `swaprb`, `led`, `sound`, `gamma` (0..4), `panel` (0 auto, 1 ILI9341, 2 ST7789; restarts), `test` (1 shows colour bars), `beep` | Change settings (persisted in NVS). The display answers with its full `cfg` |

Text fields are already trimmed by the bridge (name 40 chars, detail 96, cwd 96, prompt 200, reply 300) and limited to characters the fonts contain.

### Display → host

| `t` | Fields | Meaning |
|---|---|---|
| `hello` | `v`, `fw`, `board`, `panel`, `w`, `h` | On boot, on `who`, and every 3 s while no host is heard |
| `ack` | `id` | The user opened a finished session |
| `cfg` | current settings | Reply to every `cfg` |
| `log` | `m` | Diagnostics (panel probe result on boot) |

### Simulator

`agenttally.py serve` also listens on TCP `127.0.0.1:47322`. The desktop simulator connects there and speaks exactly the same protocol, so it behaves like a second display.
