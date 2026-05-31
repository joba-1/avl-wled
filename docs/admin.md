# avl-wled — Admin & Operations

## Install

Prereqs:

```sh
# openSUSE
sudo zypper install gcc-c++ make libcurl-devel

# Debian/Ubuntu
sudo apt install build-essential libcurl4-openssl-dev

# Fedora/RHEL
sudo dnf install gcc-c++ make libcurl-devel
```

Then:

```sh
sudo ./install.sh
sudo $EDITOR /etc/avl-wled.conf   # set ical_url and wled_host
sudo systemctl enable --now avl-wled
journalctl -u avl-wled -f
```

`install.sh` builds the binary, creates the `avl-wled` system user,
installs files via `make install`, prepares `/var/lib/avl-wled`, and
reloads systemd. Re-runnable — it preserves an existing config file.

## File layout

| Path | Owner | Purpose |
| --- | --- | --- |
| `/usr/local/bin/avl-wled` | root | Service binary |
| `/etc/avl-wled.conf` | root | Config (non-secret) |
| `/etc/systemd/system/avl-wled.service` | root | Unit |
| `/var/lib/avl-wled/calendar.ics` | avl-wled | Cached iCal |
| `/var/lib/avl-wled/acked` | avl-wled | One UID per line, dismissed events |

## Day-to-day operations

```sh
# tail logs
journalctl -u avl-wled -f

# force a recheck (restart is fastest)
sudo systemctl restart avl-wled

# inspect current state
curl http://localhost:8765/status

# dismiss the oldest reminder
curl http://localhost:8765/ack
```

## Updating

```sh
cd ~/git/avl-wled
git pull
sudo ./install.sh                  # safe to re-run
sudo systemctl restart avl-wled
```

## Configuration changes

Edit `/etc/avl-wled.conf`, then `systemctl restart avl-wled`. There is
no live reload; the daemon is stateless enough that a restart is fine
(acknowledged UIDs persist via the state file).

## Backup & Restore (CodingStandards §13)

State is tiny and non-precious:

- **Source code**: on GitHub (see `docs/spec.md` for repo URL once
  published).
- **Config** (`/etc/avl-wled.conf`): committed to repo as
  `avl-wled.conf`. No secrets. Site-specific values (`ical_url`,
  `wled_host`) are documented but the actual deployed file is also
  rsynced to the NAS NFS share as part of the host's general `/etc`
  snapshot.
- **Persistent state** (`/var/lib/avl-wled/`): the iCal cache regenerates
  itself on next fetch and the acked-UID file is sub-kilobyte. Included
  in the host's nightly NFS rsync.
- **Secrets**: none.

### Restore procedure
1. Reinstall OS + dependencies (see § Install).
2. Clone repo: `git clone <url>`.
3. Restore `/etc/avl-wled.conf` from NAS (or re-derive from the repo
   example).
4. Restore `/var/lib/avl-wled/acked` from NAS if you want to preserve
   dismissals across the reinstall; otherwise let the daemon start
   fresh.
5. `sudo ./install.sh && sudo systemctl enable --now avl-wled`.
6. Smoke test: `curl http://localhost:8765/status` should print event
   counts.

### Restore test cadence
Verify once per major release by reinstalling on a scratch VM and
running the smoke test.

## Troubleshooting

| Symptom | Likely cause | Action |
| --- | --- | --- |
| Service exits with "ical_url and wled_host are required" | Config missing or wrong path | `avl-wled --config=/etc/avl-wled.conf` reproduces; check file ownership |
| LED strip never lights up | WLED unreachable or wrong colors | Check `journalctl -u avl-wled` for `wled: post failed`; `curl http://<wled_host>/json/state` directly |
| Events not appearing | iCal feed format unexpected | Inspect `/var/lib/avl-wled/calendar.ics`; check `/status` shows nonzero `events:` |
| Lights stay on after collection | Past events not yet aged out | They drop on the next hourly tick; or `systemctl restart avl-wled` |
| Strip lit at night | Urgent window (3 h) — by design | Or `night_start`/`night_end` not covering the hour |
| `/ack` returns "nothing to acknowledge" | Already dismissed, or nothing active | Check `/status` |

## Security notes
- `/ack` and `/status` are unauthenticated. Bind to LAN only. If you
  must expose them, front with nginx + basic auth.
- The systemd unit hardens the runtime
  (`ProtectSystem=strict`, `NoNewPrivileges`, `RestrictAddressFamilies`,
  `MemoryDenyWriteExecute`, etc.).
