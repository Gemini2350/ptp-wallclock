# ptp-wallclock

[![Docker](https://github.com/Gemini2350/ptp-wallclock/actions/workflows/docker-publish.yml/badge.svg)](https://github.com/Gemini2350/ptp-wallclock/actions/workflows/docker-publish.yml)
[![Docker Hub](https://img.shields.io/docker/pulls/gemini2350/ptp-wallclock)](https://hub.docker.com/r/gemini2350/ptp-wallclock)

<p align="center"><img src="./clock2.png" alt="High-precision LED wall clock showing 19:05:37.23269755" width="800" height="auto"/></p>

# ptp-wallclock

`ptp-wallclock` is a C++ application for Raspberry Pi that acts as a PTP
(IEEE 1588 Precision Time Protocol, PTPv2) client and displays the
synchronized wall-clock time on an attached LED matrix display — or, in
headless/Docker mode, as a fullscreen clock in the browser.

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
- Best Master Clock Algorithm (BMCA): every master announcing in the domain
  is tracked, the best one is elected via the IEEE 1588 dataset comparison
  (priority 1, clock class, accuracy, variance, priority 2, identity — and
  steps removed for redundant paths to the same grandmaster). Announce
  receipt timeouts are honored, so the clock fails over automatically
- Automatic PTP domain detection (locks onto the first domain with Announce
  traffic, rescans on timeout), or a fixed domain 0–255
- PTP hardware timestamping when the NIC has a PHC (e.g. Raspberry Pi 5,
  CM4, Intel i210/i225): Sync arrival and Delay_Req departure are stamped
  by the hardware and the display runs directly off the PTP hardware
  clock — enabled automatically at startup, with transparent fallback to
  software timestamps. The status panel shows which mode is active
- Displays time on an RGB LED matrix as a configurable list of clock lines:
  each line has a time zone (UTC, TAI, or any IANA zone — world clock!), a
  rendering style, and an optional label — one line is static, several
  alternate every 4 seconds.
  Styles: digital 24h/12h, Unix timestamp, binary (BCD), flip clock,
  DCF77 telegram — all with the full nine fractional digits — plus the
  live PTP analysis charts (offset jitter & path delay, or message rates)
  as display lines on the matrix itself
- Optional second display line: date, grandmaster ID and/or priorities &
  clock quality (alternating every 4 seconds)
- Grandmaster changes are shown on the display itself ("! NEW GM !")
- **GNSS grandmaster mode**: with a GNSS receiver attached (NMEA + PPS),
  the clock disciplines itself from GPS. By default it stays **slave
  only** and *measures* the network grandmaster against GNSS: a live
  "network PTP vs GNSS" chart shows how far your house grandmaster is
  from GPS truth. A separate, deliberately-confirmed **master mode**
  switch additionally lets it take part in the BMCA as a clockClass 6
  grandmaster — sending Announce and two-step Sync/Follow_Up and
  answering Delay_Req. GNSS status (fix, satellites in view with
  per-satellite signal bars, HDOP, PPS age) is shown in the web UI
- Built-in web interface (port 8319) for settings and live status —
  grandmaster identity, priority 1/2, clock class, clock accuracy, variance,
  steps removed, time source, TAI−UTC offset, and measured path delay
- Grandmaster change notification (web + red highlight on the matrix)
- One-step installation with systemd service
- Headless mode for Docker: a fullscreen browser clock (`/clock`) replaces
  the LED panel
- Runs entirely in user space; kernel PTP (PHC) support is used when
  present but never required

---

## Hardware Requirements

These are only needed for the physical LED clock — the
[Docker version](#docker--no-led-hardware-needed) has no hardware
requirements and runs anywhere Docker runs; all it needs is a network that
carries PTP.

- Raspberry Pi (tested on Raspberry Pi 3/4/5 — Pi 5 works since the
  `rpi-rgb-led-matrix` library gained its RP1 backend; `install.sh` always
  builds the current library)
- RGB LED matrix compatible with the `rpi-rgb-led-matrix` library
-- [Adafruit RGB Matrix HAT](https://www.adafruit.com/product/2345)
-- 2 x [HUB75 LED Panel 32x64 Pixel](https://www.waveshare.com/RGB-Matrix-P3-64x32.htm) (32 x 128 total)
- Network interface receiving PTP packets (typically Ethernet)
- PTP-capable network environment (PTP grandmaster or PTP-enabled switch)

---

## Installation

### Installer script (recommended)

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

The settings page is served on `http://<pi-address>:8319`.

### Manual build

If you prefer to build by hand (with the matrix library in
`/opt/rpi-rgb-led-matrix`, or pass `MATRIX_DIR=`):

```bash
make
sudo ./ptp-clock
```

### Docker — no LED hardware needed

The clock also runs headless in a container: the PTP client and web
interface are identical, and the LED panel is replaced by a fullscreen
browser clock at `http://<host>:8319/clock` — glowing digits in the
configured color with all nine fractional digits, date, grandmaster status
line, and the GM change alert. Brightness and blackout from the settings
page apply to it too. Click the page to go fullscreen.

<p align="center"><img src="./browser-clock.png" alt="Fullscreen browser clock showing 20:53:54.812497625 with grandmaster status line" width="800" height="auto"/></p>

Prebuilt multi-arch images (amd64, arm64, arm/v7) are on Docker Hub as
[`gemini2350/ptp-wallclock`](https://hub.docker.com/r/gemini2350/ptp-wallclock),
built by CI from this repository:

```bash
docker compose up -d
```

or manually:

```bash
docker run -d --network host --restart unless-stopped \
    -v ptp-wallclock:/var/lib/ptp-wallclock \
    --name ptp-wallclock gemini2350/ptp-wallclock
```

To build the image yourself instead: `docker build -t ptp-wallclock .`

Notes:

- `--network host` is required so the container receives the PTP multicast
  on UDP 319/320 — this works on Linux hosts (Docker Desktop on
  macOS/Windows does not pass host multicast through).
- The PTP interface is auto-detected by default (the clock joins the
  multicast group on every interface with an IPv4 address). To pin it, set
  `-e PTP_WALLCLOCK_IFACE=eth0` or change it in the web UI (the volume
  keeps the settings).
- For PTP hardware timestamping inside the container the NIC's PHC must be
  passed in and the container needs `CAP_NET_ADMIN`:
  `--cap-add NET_ADMIN --device /dev/ptp0` (matching commented lines are
  in `docker-compose.yml`). Without them the container simply uses
  software timestamps.
- For the GNSS grandmaster mode pass the receiver in as well:
  `--device /dev/serial0 --device /dev/pps0`.
- The same headless binary can be built without Docker: `make headless`.

---

## Web Interface

The clock serves a settings page on `http://<pi-address>:8319`. At the top
it shows the current PTP time as a live, smoothly ticking clock with all
nine fractional digits, just like the matrix (the server sends its TAI time
with every status poll and the browser extrapolates in between; expect a few
milliseconds of network offset). The fractional digits — on the LED matrix
and in the browser alike — use a per-position speed ladder: the tenths
digit is the true value, and every further digit visibly changes faster
than the one before it. The real values change far too fast for any
display, so the fast digits are synthesized sample-and-hold; what you see
is an ordered acceleration instead of uniform flicker. Settings:

- **Display color** — color picker for the LED matrix text
- **Brightness** — 1–100 % slider, applied immediately
- **Blackout** — one-click switch to temporarily turn the display off
  (the clock keeps tracking PTP in the background)
- **Rotate 180°** — for LED panels that are mounted upside down, applied
  live without a restart
- **Clock lines** — the list of clocks shown on the LED matrix. Per line:
  time zone (UTC, TAI, or any IANA zone — so a world clock like
  `New York / Zurich / Tokyo` is just three lines), style (digital 24h/12h,
  Unix timestamp, binary BCD, flip clock, DCF77 telegram — all with the
  full nine fractional digits — or one of the two live PTP analysis
  graphs), and an optional label (blank = none, `%Z` = zone abbreviation).
  One line is shown statically, several alternate every 4 seconds —
  PTP-second aligned, like everything else
- **Grandmaster ID** — show the current PTP grandmaster identity as a second
  line on the matrix
- **Priorities & clock quality** — show priority 1/2, clock class, and the
  time source (e.g. `GNSS`) on the matrix
- **Date** — show the date on the matrix (if several second-line options are
  enabled, the line alternates every 4 seconds)
- **Date format** — `DD.MM.YYYY`, ISO 8601, or `MM/DD/YYYY`. The browser
  clock follows the same clock lines as the LED matrix — labels appear
  between time and date, and pixel styles fall back to a digital rendering
- **PTP domain** — automatic detection (default) or a fixed domain number
  (0–255); the detected domain is shown in the status panel
- **Network interface** — selectable from the interfaces present on the
  system, applied without restart
- **Acceptable grandmasters** — optional comma-separated list of grandmaster
  identities (any separator style). If BMCA elects a grandmaster that is not
  on the list, a red `! UNACCEPTED GM !` error appears on the LED matrix and
  the browser clock, and the settings page shows a warning banner. Empty
  list = any grandmaster is fine
- **Grandmaster change notification** — when enabled, a grandmaster change
  shows `! NEW GM !` in red on the matrix (and on the browser clock) for
  10 seconds and triggers a browser notification / banner on the settings
  page:
- **Offset warning threshold** — optional limit in µs (0 = off). Whenever
  a sync deviates from the smoothed offset by more than the threshold —
  or, in grandmaster/passive mode, whenever the network master differs
  from GNSS by more than it — a red `! OFFSET +12.3us !` alert (with the
  actual value) shows on the LED matrix, the browser clock, and as a
  banner on the settings page for 10 seconds

<p align="center"><img src="./browser-clock2.png" alt="Browser clock showing the red NEW GM alert after a grandmaster change" width="800" height="auto"/></p>

The status panel shows live data decoded from the Announce messages
(grandmaster identity, priorities, clock class/accuracy/variance, steps
removed, time source), the grandmaster's vendor — resolved offline from the
OUI inside its identity via a curated list of common PTP/AV brands — the
TAI−UTC offset, and the measured mean path delay with a
Delay_Req/Delay_Resp counter. A separate table lists all masters
currently visible in the domain with the BMCA-elected one marked — handy for
watching a failover happen.

A **PTP analysis** section charts the last couple of minutes: sync offset
jitter (how far each Sync was off the smoothed estimate) together with the
raw path-delay samples, and the received message rates per second (Sync,
Follow_Up, Announce, Delay_Resp) in the active domain. The charts
auto-scale on percentiles, so the step of a master switch doesn't flatten
the interesting µs range. It also states whether the data comes from
hardware or software timestamps. Clicking a chart (or the *large view*
link) opens a fullscreen version at `/analysis`. And because the charts
are clock-line styles too (`PTP graph`, `PTP message rates`), they can be
put on the LED matrix itself — as an extra alternating line or as a
dedicated 128×32 jitter display.

> Note: browser push notifications require the page to be allowed to notify;
> on plain HTTP some browsers only show the in-page banner.

<p align="center"><img src="./web-settings.png" alt="Web interface with live PTP time, status panel, BMCA master list, and settings" width="560" height="auto"/></p>

## Configuration file

Settings are persisted as simple `key=value` pairs. The file is
`/var/lib/ptp-wallclock/ptp-wallclock.conf` when installed via `install.sh`,
otherwise `ptp-wallclock.conf` in the working directory (override with the
`PTP_WALLCLOCK_CONF` environment variable). Two settings are only
available in the file (restart required):

| Key         | Default | Meaning                                        |
|-------------|---------|------------------------------------------------|
| `http_port` | `8319`  | Port of the web interface                      |
| `hwts`      | `1`     | Try PTP hardware timestamping, `0` = never     |

The network interface (`iface`, default `auto`) can be changed in the web
interface without a restart. In `auto` mode the clock joins the PTP
multicast group on every interface that has an IPv4 address (re-checked
every 5 seconds, so interfaces that appear late — DHCP at boot, hotplug —
are picked up automatically). Pinning it to one interface name switches
the membership over immediately.

## GNSS grandmaster mode

With a GNSS receiver the wallclock can *be* the PTP grandmaster instead
of just displaying one. It needs two signals from the receiver:

- **NMEA** on a serial port (tells which second it is) — default
  `/dev/serial0`, 9600 baud
- **PPS** on a GPIO (tells exactly when the second starts) — default
  `/dev/pps0`

Example wiring for the Waveshare MAX-M8Q GNSS HAT (whose PPS is on
GPIO 18 out of the box; it stacks under the LED matrix HAT with a
stacking header and conflicts with none of its pins) — in
`/boot/firmware/config.txt`:

```
enable_uart=1
dtoverlay=pps-gpio,gpiopin=18
```

Then enable **PTP grandmaster (GNSS)** on the settings page. Activation
is deliberately two-staged:

1. **Use a GNSS receiver** — slave only (the default): the clock runs on
   GNSS time and measures the network grandmaster against it, but never
   transmits PTP itself. Safe on any network.
2. **Master mode** — a separate, explicitly-confirmed switch with a
   warning: only with it enabled does the clock join the BMCA and, if it
   wins, actively send Announce/Sync — other PTP devices may then
   synchronize to it.

What happens with both enabled:

- Each PPS pulse is paired with the following RMC sentence into a time
  sample; the display and web clock run directly on GNSS time
  (the analysis chart then shows the PPS jitter)
- The clock joins the BMCA with clockClass 6 (GNSS locked), accuracy per
  timestamping mode, timeSource GPS, and the configurable priorities. If
  it wins, it transmits Announce + two-step Sync/Follow_Up once per
  second and answers Delay_Req — with hardware TX timestamps on a Pi 5
- If a **better** grandmaster announces (lower priority1, etc.), the
  clock stays passive — and measures that master's Syncs against GNSS.
  The **network PTP vs GNSS** chart and status line show the offset of
  your grandmaster against GPS truth, per Sync, in µs (the path delay to
  it is measured with Delay_Req as usual and subtracted)
- Loses GNSS → clockClass 7 holdover for 5 minutes, then it returns to
  plain client mode; the GNSS status panel shows fix quality, satellites
  used/in view with per-satellite signal-strength bars, HDOP and PPS age
- clockClass 6 usually beats everything on a lab network. Give the clock
  priority1 > your real grandmaster's if you only want the measurement,
  or < if you want it to take over

TAI − UTC (37 s since 2017) is a setting: NMEA carries UTC, PTP runs on
TAI, and the offset is announced to clients. The devices are opened
while the service still runs as root; `install.sh` also installs a udev
rule + group so reopening after the privilege drop works (e.g. USB
receivers being replugged).

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

At startup the clock probes the network interface for PTP hardware
timestamping (`ETHTOOL_GET_TS_INFO`). If the NIC has a PHC — Raspberry
Pi 5 (RP1 Ethernet) and CM4 have one, Pi 3/4 do not — timestamping is
enabled in the NIC, Sync arrival (t2) is taken from the hardware RX
timestamp, the Delay_Req send time (t3) from the hardware TX timestamp on
the socket error queue, and the displayed time is derived from the PHC
itself, eliminating the user-space scheduling jitter from the
measurements. The status panel shows the active mode, e.g.
`hardware (eth0 via /dev/ptp0)`. Set `hwts=0` in the configuration file
to force software timestamps.

Without a PHC, packets are timestamped in user space and the achievable
accuracy is in the sub-millisecond range — plenty for a wall clock
display, but this is a visualization tool, not a reference clock. In both
modes Sync/Follow_Up and Delay_Resp measurements are smoothed with a
small exponential filter. Note that the LED refresh and the browser
rendering add their own few milliseconds — hardware timestamping makes
the *measurements* honest (visible in the PTP analysis charts), not the
photons faster.

## References

[Excuse me, what precise time is It?](https://media.ccc.de/v/39c3-excuse-me-what-precise-time-is-it).

## Open Issues

- PTPv1 (IEEE 1588-2002) is not supported. The implementation targets PTPv2
  (IEEE 1588-2008) only.

- Hardware timestamping is probed once at startup; after changing the
  interface setting, restart the service to re-probe.
