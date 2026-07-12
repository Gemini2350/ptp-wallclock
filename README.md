# ptp-wallclock

<p align="center"><img src="./clock2.png" alt="High-precision LED wall clock showing 19:05:37.23269755" width="800" height="auto"/></p>

# ptp-wallclock

`ptp-wallclock` is a C++ application for Raspberry Pi that acts as a PTP
(IEEE 1588 Precision Time Protocol, PTPv2) client and displays the
synchronized wall-clock time on an attached LED matrix display.

The project is intended as a lightweight, hardware-based visualization of PTP
time synchronization, useful for experiments, demos, and educational purposes.
I've used it to demonstrate that PTP is really distributing the Time at my Speech at Chaos Computer Club,
[Excuse me, what precise time is It?](https://media.ccc.de/v/39c3-excuse-me-what-precise-time-is-it).

---

## Features

- Real PTPv2 (IEEE 1588) client with the end-to-end delay mechanism:
  Sync / Follow_Up are correlated with their local arrival time, Delay_Req /
  Delay_Resp measure the network path delay, and the displayed time is
  corrected accordingly (correction fields included, one-step and two-step
  masters supported)
- Automatic PTP domain detection (locks onto the first domain with Announce
  traffic, rescans on timeout), or a fixed domain 0–255
- Displays time on an RGB LED matrix (UTC, TAI, or local time zone)
- Optional second display line: date, grandmaster ID and/or priorities &
  clock quality (alternating every 4 seconds)
- Grandmaster changes are shown on the display itself ("! NEUER GM !")
- Built-in web interface (port 8080) for settings and live status —
  grandmaster identity, priority 1/2, clock class, clock accuracy, variance,
  steps removed, time source, TAI−UTC offset, and measured path delay
- Grandmaster change notification (web + red highlight on the matrix)
- One-step installation with systemd service
- Runs entirely in user space, no kernel PTP support needed

---

## Hardware Requirements

- Raspberry Pi (tested on Raspberry Pi 3/4 - 5 not working at the moment)
- RGB LED matrix compatible with the `rpi-rgb-led-matrix` library
-- [Adafruit RGB Matrix HAT](https://www.adafruit.com/product/2345)
-- 2 x [HUB75 LED Panel 32x64 Pixel](https://www.waveshare.com/RGB-Matrix-P3-64x32.htm) (32 x 128 total)
- Network interface receiving PTP packets (typically Ethernet)
- PTP-capable network environment (PTP grandmaster or PTP-enabled switch)

---

## Installation

### Easy way: Raspberry Pi image

You can download my Raspberry Pi image and just flash the ISO with Rufus:
[Releases](https://github.com/Gemini2350/ptp-wallclock/releases)

### Recommended: installer script

On a Raspberry Pi OS system, one command does everything (fetches and builds
the `rpi-rgb-led-matrix` library, compiles the clock, installs fonts and a
systemd service that starts on boot):

```bash
git clone https://github.com/Gemini2350/ptp-wallclock.git
cd ptp-wallclock
sudo ./install.sh
```

Afterwards:

```bash
systemctl status ptp-wallclock     # service status
journalctl -u ptp-wallclock -f     # logs
```

The settings page is served on `http://<pi-address>:8080`.

### Manual build

If you prefer to build by hand (with the matrix library in
`/opt/rpi-rgb-led-matrix`, or pass `MATRIX_DIR=`):

```bash
make
sudo ./ptp-clock
```

---

## Web Interface

The clock serves a settings page on `http://<pi-address>:8080` with:

- **Display color** — color picker for the LED matrix text
- **Grandmaster ID** — show the current PTP grandmaster identity as a second
  line on the matrix
- **Priorities & clock quality** — show priority 1/2, clock class and clock
  accuracy on the matrix
- **Date** — show the date on the matrix (if several second-line options are
  enabled, the line alternates every 4 seconds)
- **Time display** — UTC, TAI, or local time with a configurable time zone
  (IANA names such as `Europe/Berlin`)
- **PTP domain** — automatic detection (default) or a fixed domain number
  (0–255); the detected domain is shown in the status panel
- **Network interface** — selectable from the interfaces present on the
  system, applied without restart
- **Grandmaster change notification** — when enabled, a grandmaster change
  shows `! NEUER GM !` in red on the matrix for 10 seconds and triggers a
  browser notification / banner on the settings page

The status panel shows live data decoded from the Announce messages
(grandmaster identity, priorities, clock class/accuracy/variance, steps
removed, time source), the TAI−UTC offset, and the measured mean path delay
with a Delay_Req/Delay_Resp counter.

> Note: browser push notifications require the page to be allowed to notify;
> on plain HTTP some browsers only show the in-page banner.

## Configuration file

Settings are persisted as simple `key=value` pairs. The file is
`/var/lib/ptp-wallclock/ptp-wallclock.conf` when installed via `install.sh`,
otherwise `ptp-wallclock.conf` in the working directory (override with the
`PTP_WALLCLOCK_CONF` environment variable). One setting is only available
in the file (restart required):

| Key         | Default | Meaning                              |
|-------------|---------|--------------------------------------|
| `http_port` | `8080`  | Port of the web interface            |

The network interface (`iface`, default `eth0`) can be changed in the web
interface — the clock leaves the multicast group on the old interface and
joins on the new one without a restart. If the interface has no IPv4 address
yet (e.g. DHCP still running at boot), the clock keeps retrying every 5
seconds instead of exiting.

## Privileged ports (why sudo?)

PTP uses UDP ports 319 and 320, which are privileged ports on Linux. The
systemd service handles this cleanly: it starts as root, binds the PTP ports
and initializes the GPIO, and then the `rpi-rgb-led-matrix` library drops
privileges to the `daemon` user by itself. No `sysctl` tweaking is needed.

For manual runs you can either use `sudo ./ptp-clock` (recommended for best
matrix performance) or grant the binary the bind capability once:

```bash
sudo setcap cap_net_bind_service+ep ./ptp-clock
./ptp-clock
```

If binding fails, the program now exits with exactly this hint instead of
silently misbehaving.

## Accuracy

Packets are timestamped in user space (no hardware timestamping), so the
achievable accuracy is in the sub-millisecond range on a Raspberry Pi —
plenty for a wall clock display, but this is a visualization tool, not a
reference clock. Sync/Follow_Up and Delay_Resp measurements are smoothed
with a small exponential filter.

## References

[Excuse me, what precise time is It?](https://media.ccc.de/v/39c3-excuse-me-what-precise-time-is-it).

## Open Issues

- Best Master Clock Algorithm (BMCA) is not implemented. The clock passively
  follows whichever master is sending Sync in the configured domain.

- PTPv1 (IEEE 1588-2002) is not supported. The implementation targets PTPv2
  (IEEE 1588-2008) only.

- Only software timestamps are used (see Accuracy above).
