#!/usr/bin/env bash
# One-step installer for ptp-wallclock on Raspberry Pi OS.
# Usage: sudo ./install.sh
set -euo pipefail

MATRIX_DIR=/opt/rpi-rgb-led-matrix
FONT_DIR=/usr/share/fonts/rpi-rgb-led-matrix
STATE_DIR=/var/lib/ptp-wallclock
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$(id -u)" -ne 0 ]; then
    echo "Please run as root: sudo ./install.sh" >&2
    exit 1
fi

echo "==> Checking build tools (git, g++, make)"
missing=""
for tool in git g++ make; do
    command -v "$tool" >/dev/null || missing="$missing $tool"
done
if [ -n "$missing" ]; then
    echo "==> Installing:$missing"
    apt-get update
    apt-get install -y git g++ make
fi

if [ ! -d "$MATRIX_DIR" ]; then
    echo "==> Cloning rpi-rgb-led-matrix to $MATRIX_DIR"
    git clone --depth 1 https://github.com/hzeller/rpi-rgb-led-matrix.git "$MATRIX_DIR"
fi

echo "==> Building rpi-rgb-led-matrix library"
make -C "$MATRIX_DIR/lib"

echo "==> Building ptp-clock"
g++ -O2 -std=c++17 "$SCRIPT_DIR/ptp-clock.cpp" -o "$SCRIPT_DIR/ptp-clock" \
    -I"$MATRIX_DIR/include" -L"$MATRIX_DIR/lib" -lrgbmatrix -lpthread

echo "==> Installing binary, fonts, state directory"
install -m 755 "$SCRIPT_DIR/ptp-clock" /usr/local/bin/ptp-clock
install -d "$FONT_DIR"
install -m 644 "$MATRIX_DIR/fonts/6x13B.bdf" "$MATRIX_DIR/fonts/4x6.bdf" "$FONT_DIR/"
# The matrix library drops privileges to daemon after init;
# the config file must stay writable for that user.
install -d -o daemon -g daemon "$STATE_DIR"

echo "==> Installing systemd service"
install -m 644 "$SCRIPT_DIR/ptp-wallclock.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now ptp-wallclock.service

IP=$(hostname -I 2>/dev/null | awk '{print $1}')
echo
echo "Done. The clock starts automatically on boot."
echo "  Status:   systemctl status ptp-wallclock"
echo "  Logs:     journalctl -u ptp-wallclock -f"
echo "  Settings: http://${IP:-<pi-address>}:8319"
