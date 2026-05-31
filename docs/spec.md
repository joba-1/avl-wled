# avl-wled — Specification

## Purpose
Drive a WLED-controlled LED strip as a passive, glanceable reminder that
an AVL waste-collection date is approaching. The strip lights up in
defined warning windows and is dismissed by hitting an HTTP endpoint
(physical button, Home Assistant, phone shortcut).

Source brief: see `AGENTS.md` at the repo root.

## Architecture

Single-process Linux daemon, written in C++17, built as one standalone
binary. Dependencies at runtime: `libcurl` only. Distribution:
`/usr/local/bin/avl-wled` + systemd unit + `/etc/avl-wled.conf`.

```
                +--------------------------+
   AVL iCal --> | fetcher (weekly, curl)   |
                +-----------+--------------+
                            v
                    /var/lib/avl-wled/calendar.ics
                            |
                            v
                +--------------------------+
                | scheduler (hourly tick)  |
                |   parse iCal             |
                |   compute active codes   |
                +-----+--------------+-----+
                      |              |
                      |              v
                      |     /var/lib/avl-wled/acked
                      v
                +--------------------------+
                | WLED JSON pusher (curl)  |---> http://wled.local/json/state
                +--------------------------+
                      ^
                      |  (recompute + push)
                +-----+--------------------+
                | HTTP /ack endpoint       |  <-- button / HA / phone
                | HTTP /status endpoint    |
                +--------------------------+
```

### Components

| Component | File / function | Notes |
| --- | --- | --- |
| Config loader | `loadConfigFile` / `applyKV` | INI-style `key = value`, every key overridable via `--key=value` on the CLI |
| iCal fetcher | `httpGetToFile` + `doFetch` | curl GET, follow redirects, write to cache file |
| iCal parser | `parseIcal` + `unfold` + `parseIcalDt` | Line unfolding, `VEVENT` blocks, supports DATE and DATE-TIME (`Z`/local) |
| Scheduler | `main` loop | 30 s tick; weekly fetch + hourly recompute |
| Active-code engine | `recomputeActive` | Drops past events, drops acknowledged UIDs, applies night-time suppression to non-urgent warnings |
| WLED encoder | `buildWledJson` | Status segment + one segment per active event; unused segments cleared with `stop:0` |
| WLED pusher | `httpPostJson` + `pushWled` | curl POST to `http://wled_host/json/state` |
| HTTP server | `httpServerLoop` | Tiny socket-based HTTP/1.0 server, thread, `GET/POST /ack` + `GET /status` |
| State | `loadAcked` / `saveAcked` | Plain text, one UID per line, in `state_file` |

### Concurrency
Two threads: scheduler (main) and HTTP server. Shared state guarded by a
single `std::mutex` (`g_mu`). Wakeup via `std::condition_variable` so
`SIGTERM` is responsive.

### Failure model
- iCal fetch failure: keep using the cached file, log to stderr, retry on
  next interval. Service stays up.
- WLED unreachable: log, continue. Next state change retries.
- Malformed iCal entry: skipped silently (defensive parser).
- State file unwritable: error to stderr; ack still applied in-memory
  until restart.

## Design decisions & deviations

**C++ instead of Python (deviates from CodingStandards §5).**
Standard says CLI tools are written in Python. We deliberately picked
C++17 + libcurl here because the target is a long-running service on a
small Linux box, and the user wanted to avoid pulling in Python module
dependencies (system pip, distro packages, venv, icalendar lib, etc.).
The footprint is a single static-ish binary depending only on libcurl,
which is present on essentially every Linux host. The iCal subset we
need (`VEVENT`, `DTSTART`, `SUMMARY`, `UID`) is small enough to parse by
hand without a library.

**No external JSON library.** WLED's `/json/state` payload is small and
constructed by direct string assembly. Avoids pulling in nlohmann/json.

**Hand-rolled HTTP server.** Only one endpoint that needs to receive
requests (`/ack`). A 100-line socket loop is preferable to bringing in
microhttpd or similar.

**Single config file, all keys CLI-overridable.** Matches CodingStandards
§2 (central config) and gives the operator an easy way to tweak a single
parameter without editing the file.

**State = a flat set of acknowledged UIDs.** No database. The data is
tiny (UIDs of events the user has dismissed, garbage-collected as soon
as those events fall off the iCal feed). SQLite would be overkill.

**syslog via journald.** Logs go to stderr; under systemd they land in
the journal, which forwards to syslog per CodingStandards §10.

## Configuration

See `avl-wled.conf` in the repo root for the annotated example. Reference
table is in `README.md`.

## Color/segment model

- LEDs are split into `N+1` equal segments where `N` = number of active
  codes. The first segment is the *status* segment (red if any active
  code is urgent, otherwise green); the remaining `N` are one per
  upcoming event, colored from a configurable
  SUMMARY-keyword → RGB map.
- If `N+1 > max_segments`, segments are capped at `max_segments`.
- When `N == 0`, all segments are sent `stop:0` and the strip is turned
  off via `"on": false`.

## Out of scope (for now)
- Multiple WLED targets.
- Web UI (a passive LED strip + an `/ack` URL is the whole product).
- Per-category urgency overrides.
- HTTPS for `/ack` (intended for LAN only — put it behind nginx if you
  expose it).
