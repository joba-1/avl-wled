#!/bin/bash
# avl-wled installer. Builds the binary, installs files, creates a
# system user, and enables the systemd service.
#
# Re-running is safe: an existing /etc/avl-wled.conf will not be
# overwritten.

set -euo pipefail

PREFIX="${PREFIX:-/usr/local}"
SYSCONF="${SYSCONF:-/etc}"
UNITDIR="${UNITDIR:-/etc/systemd/system}"
USER_NAME="${USER_NAME:-avl-wled}"
STATE_DIR="/var/lib/avl-wled"

here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"

if [ "$(id -u)" -ne 0 ]; then
    echo "this installer needs root (try: sudo $0)" >&2
    exit 1
fi

echo "==> checking build deps"
need() { command -v "$1" >/dev/null || { echo "missing: $1" >&2; exit 1; }; }
need g++
need make
if ! echo '#include <curl/curl.h>' | g++ -E -x c++ - >/dev/null 2>&1; then
    echo "missing libcurl headers (install libcurl4-openssl-dev / libcurl-devel)" >&2
    exit 1
fi

echo "==> building"
make clean
make

echo "==> creating system user '$USER_NAME'"
if ! id "$USER_NAME" >/dev/null 2>&1; then
    useradd --system --home-dir "$STATE_DIR" --shell /usr/sbin/nologin \
            --comment "avl-wled service" "$USER_NAME"
fi

echo "==> installing files"
make install PREFIX="$PREFIX" SYSCONF="$SYSCONF" UNITDIR="$UNITDIR"

install -d -o "$USER_NAME" -g "$USER_NAME" -m 0755 "$STATE_DIR"

echo "==> reloading systemd"
systemctl daemon-reload

echo
echo "Done."
echo
echo "Next steps:"
echo "  1. Edit $SYSCONF/avl-wled.conf (set ical_url and wled_host)."
echo "  2. systemctl enable --now avl-wled"
echo "  3. journalctl -u avl-wled -f"
echo
echo "Acknowledge endpoint will then be at: http://<this-host>:8765/ack"
