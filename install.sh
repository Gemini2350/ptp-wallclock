#!/usr/bin/env bash
# One-step installer for ptp-wallclock on Raspberry Pi OS.
# Usage: sudo ./install.sh          — build + install + service
#        sudo ./install.sh --gnss   — additionally prepare the OS for a
#                                     GNSS receiver (NMEA on serial0,
#                                     PPS on GPIO 18; override with
#                                     GNSS_PPS_GPIO=nn). Reboots
#                                     automatically when the boot config
#                                     changed; --no-reboot skips that.
#        sudo ./install.sh --gnss-tune
#                                   — install a boot-time tuning service
#                                     that pins the PPS interrupt to a
#                                     quiet CPU and sets the performance
#                                     governor (reduces PPS interrupt
#                                     latency jitter; combine with
#                                     --gnss)
set -euo pipefail

GNSS_SETUP=0
GNSS_TUNE=0
NO_REBOOT=0
for arg in "$@"; do
    [ "$arg" = "--gnss" ] && GNSS_SETUP=1
    [ "$arg" = "--gnss-tune" ] && GNSS_TUNE=1
    [ "$arg" = "--no-reboot" ] && NO_REBOOT=1
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
REV=$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)
g++ -O2 -std=c++17 -DPTP_WALLCLOCK_REV="\"$REV\"" \
    "$SCRIPT_DIR/ptp-clock.cpp" -o "$SCRIPT_DIR/ptp-clock" \
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

if [ "$GNSS_TUNE" = 1 ]; then
    echo "==> GNSS tuning: performance governor + PPS IRQ affinity"
    cat > /usr/local/lib/ptp-wallclock-tune.sh <<'EOF'
#!/bin/sh
# ptp-wallclock: reduce PPS interrupt latency jitter.
# Lock the CPU clocks (idle-state wakeups and DVFS transitions add
# microsecond-scale, variable interrupt latency) and pin the PPS
# interrupt to CPU 2 — the LED matrix refresh thread claims CPU 3.
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$g" 2>/dev/null || true
done
irq=$(awk -F: '/pps/ {gsub(/ /, "", $1); print $1; exit}' /proc/interrupts)
if [ -n "$irq" ]; then
    echo 4 > "/proc/irq/$irq/smp_affinity" 2>/dev/null || true
fi
# NIC interrupt coalescing: the default (~49 us) delays packet delivery
# by a variable amount. Irrelevant for hardware timestamps (the MAC
# stamps at wire time), but it directly widens software timestamps.
for dev in /sys/class/net/eth* /sys/class/net/en*; do
    [ -e "$dev" ] || continue
    ethtool -C "$(basename "$dev")" tx-usecs 4 rx-usecs 4 2>/dev/null || true
done
exit 0
EOF
    chmod 755 /usr/local/lib/ptp-wallclock-tune.sh
    cat > /etc/systemd/system/ptp-wallclock-tune.service <<'EOF'
[Unit]
Description=PTP Wallclock latency tuning (governor + PPS IRQ affinity)
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/usr/local/lib/ptp-wallclock-tune.sh

[Install]
WantedBy=multi-user.target
EOF
    systemctl daemon-reload
    systemctl enable ptp-wallclock-tune.service >/dev/null
    systemctl restart ptp-wallclock-tune.service
    echo "    applied now and re-applied on every boot"
fi

IP=$(hostname -I 2>/dev/null | awk '{print $1}')
echo
echo "Done. The clock starts automatically on boot."
echo "  Status:   systemctl status ptp-wallclock"
echo "  Logs:     journalctl -u ptp-wallclock -f"
echo "  Settings: http://${IP:-<pi-address>}:8319"
if [ "$NEED_REBOOT" = 1 ]; then
    echo
    if [ "$NO_REBOOT" = 1 ]; then
        echo "  GNSS boot configuration written — PLEASE REBOOT, then enable"
        echo "  'Use a GNSS receiver' on the settings page."
    else
        echo "  GNSS boot configuration written. After the reboot, enable"
        echo "  'Use a GNSS receiver' on the settings page."
        echo "  Rebooting in 5 seconds — Ctrl-C to cancel (--no-reboot skips this)."
        sleep 5
        reboot
    fi
fi
