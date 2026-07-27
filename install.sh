#!/usr/bin/env bash
# One-step installer for ptp-wallclock on Raspberry Pi OS.
# Usage: sudo ./install.sh          — build + install + service
#        sudo ./install.sh --gnss   — additionally prepare the OS for a
#                                     GNSS receiver (NMEA on serial0,
#                                     PPS on GPIO 18; override with
#                                     GNSS_PPS_GPIO=nn). Reboot after.
set -euo pipefail

GNSS_SETUP=0
for arg in "$@"; do
    [ "$arg" = "--gnss" ] && GNSS_SETUP=1
done

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

# GNSS grandmaster mode: let the daemon user reopen the serial port and
# the PPS device after the privilege drop (the initial open happens as
# root, so this only matters for hotplug/reconfiguration)
usermod -a -G dialout daemon 2>/dev/null || true
cat > /etc/udev/rules.d/99-ptp-wallclock-pps.rules <<'EOF'
SUBSYSTEM=="pps", MODE="0660", GROUP="dialout"
EOF
udevadm control --reload-rules 2>/dev/null || true

echo "==> Installing systemd service"
install -m 644 "$SCRIPT_DIR/ptp-wallclock.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable ptp-wallclock.service
# restart (not just start) so re-running install.sh picks up the new binary
systemctl restart ptp-wallclock.service

NEED_REBOOT=0
if [ "$GNSS_SETUP" = 1 ]; then
    echo "==> GNSS setup: serial port + PPS overlay"
    BOOTDIR=/boot/firmware
    [ -f "$BOOTDIR/config.txt" ] || BOOTDIR=/boot
    CFG="$BOOTDIR/config.txt"
    CMD="$BOOTDIR/cmdline.txt"
    PPS_GPIO="${GNSS_PPS_GPIO:-18}"

    if [ -f "$CFG" ]; then
        need_any=0
        grep -q "^enable_uart=1" "$CFG" || need_any=1
        grep -q "^dtoverlay=disable-bt" "$CFG" || need_any=1
        grep -q "^dtoverlay=pps-gpio" "$CFG" || need_any=1
        if [ "$need_any" = 1 ]; then
            [ -f "$CFG.wallclock.bak" ] || cp "$CFG" "$CFG.wallclock.bak"
            {
                echo ""
                echo "# ptp-wallclock: GNSS receiver (NMEA + PPS)"
            } >> "$CFG"
            # enable_uart: the serial port itself; disable-bt: give the
            # good PL011 UART to serial0 instead of Bluetooth (the mini
            # UART's baud rate floats with the core clock)
            grep -q "^enable_uart=1" "$CFG" || \
                { echo "enable_uart=1" >> "$CFG"; echo "    + enable_uart=1"; }
            grep -q "^dtoverlay=disable-bt" "$CFG" || \
                { echo "dtoverlay=disable-bt" >> "$CFG"; \
                  echo "    + dtoverlay=disable-bt"; }
            grep -q "^dtoverlay=pps-gpio" "$CFG" || \
                { echo "dtoverlay=pps-gpio,gpiopin=$PPS_GPIO" >> "$CFG"; \
                  echo "    + dtoverlay=pps-gpio,gpiopin=$PPS_GPIO"; }
            NEED_REBOOT=1
        else
            echo "    boot config already in place"
        fi
    else
        echo "    WARNING: $CFG not found — not Raspberry Pi OS?" >&2
    fi

    # A login console on the serial port would eat the NMEA stream
    if [ -f "$CMD" ] && grep -Eq 'console=(serial0|ttyAMA0|ttyS0),[0-9]+' "$CMD"; then
        [ -f "$CMD.wallclock.bak" ] || cp "$CMD" "$CMD.wallclock.bak"
        sed -Ei 's/console=(serial0|ttyAMA0|ttyS0),[0-9]+ ?//g' "$CMD"
        echo "    - removed the serial login console from cmdline.txt"
        NEED_REBOOT=1
    fi
    systemctl disable --now serial-getty@ttyAMA0.service >/dev/null 2>&1 || true
    systemctl disable --now serial-getty@ttyS0.service >/dev/null 2>&1 || true
    systemctl disable hciuart >/dev/null 2>&1 || true

    # handy for debugging: sudo ppstest /dev/pps0
    apt-get install -y pps-tools >/dev/null 2>&1 || true
fi

IP=$(hostname -I 2>/dev/null | awk '{print $1}')
echo
echo "Done. The clock starts automatically on boot."
echo "  Status:   systemctl status ptp-wallclock"
echo "  Logs:     journalctl -u ptp-wallclock -f"
echo "  Settings: http://${IP:-<pi-address>}:8319"
if [ "$NEED_REBOOT" = 1 ]; then
    echo
    echo "  GNSS boot configuration written — PLEASE REBOOT, then enable"
    echo "  'Use a GNSS receiver' on the settings page."
fi
