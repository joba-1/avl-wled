# avl-wled — Test Plan

Conforms to CodingStandards §9: unit tests before commit, component on
minor, full system on major.

## Levels

### Unit (run before every commit)

Wired up. Pure helpers live in `core.h`; the suite is `tests/unit.cpp`
using the vendored single-header `tests/doctest.h`, with iCal fixtures
under `tests/data/`. Run with:

```sh
make test
```

Minimum cases:

| Function | Cases |
| --- | --- |
| `parseIcalDt` | DATE-only, local DATE-TIME, UTC `Z` DATE-TIME, malformed input → 0 |
| `unfold` | folded line with leading space, folded with tab, CRLF endings, no-fold input |
| `parseIcal` | empty file, single VEVENT, multiple VEVENTs, VEVENT without UID (synthetic UID), nested junk between events |
| `parseHex` | `FF0000`, `#00ff00`, `123`, garbage → black |
| `isNightTime` | non-wrapping window, wrapping window (22→7), equal start/end → false |
| `colorFor` | exact keyword hit, case-insensitive, no match → normal_color |
| `recomputeActive` | past event dropped; acked event dropped; urgent always present; warn suppressed at night; sort order |
| `buildWledJson` | empty active → on:false + cleared segments; 1 active urgent → 2 segments + status red; N > max_segments → capped |

### Component / integration (run on minor release)

1. **iCal-fetch component**
   - Spin up a local HTTP server serving a known fixture iCal.
   - Run `avl-wled --ical_url=http://127.0.0.1:PORT/feed.ics
     --wled_host=127.0.0.1:NONE --check_interval=5 ...`
   - Assert `/var/lib/.../calendar.ics` matches fixture bytes and
     `/status` lists the expected events.

2. **WLED-push component**
   - Stand up a stub HTTP server that records POSTs.
   - Stub iCal with one upcoming event inside `urgent_window`.
   - Assert the recorded body matches expected JSON (status red,
     one event segment).

3. **Ack roundtrip**
   - With one active event, `curl /ack`, then `curl /status`, assert
     the active list is empty and the UID is in the state file.

4. **Night-time suppression**
   - Set `night_start`/`night_end` to bracket "now"; event 12 h ahead
     (warn window, not urgent) → assert no segments lit.
   - Same event 1 h ahead (urgent) → assert lit.

### System (run on every major release)

Deploy on the actual target host with a real WLED device:

- Confirm the systemd unit starts, runs as `avl-wled`, writes to
  `/var/lib/avl-wled/`, and survives reboot.
- Acknowledge from a phone shortcut → strip clears within ~1 s.
- Leave the daemon up for ≥ 25 h to verify the hourly tick (event
  ages out correctly).
- Restart-and-restore: stop service, edit acked file, restart,
  confirm state restored.

## Manual smoke checks after a config change

```sh
sudo systemctl restart avl-wled
curl http://localhost:8765/status
journalctl -u avl-wled -n 20
```

## Known gaps

- Component/integration and system levels are still manual (no
  automated stub WLED server / fixture HTTP server yet).
- No fuzzing of the iCal parser; the parser is defensive against
  short/missing fields but has not been fuzz-tested.
