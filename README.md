# ptp-wallclock

[![Docker](https://github.com/Gemini2350/ptp-wallclock/actions/workflows/docker-publish.yml/badge.svg)](https://github.com/Gemini2350/ptp-wallclock/actions/workflows/docker-publish.yml)
[![Docker Hub](https://img.shields.io/docker/pulls/gemini2350/ptp-wallclock)](https://hub.docker.com/r/gemini2350/ptp-wallclock)

<p align="center"><img src="./clock2.png" alt="High-precision LED wall clock showing 19:05:37.23269755" width="800" height="auto"/></p>

A single-binary PTP (IEEE 1588) client for Raspberry Pi that makes time
synchronization *visible*: on an LED matrix wall clock, as a fullscreen
browser clock, and — with a GNSS receiver — as a grandmaster and
measurement instrument that checks your network's time against GPS
truth. Shown live at the Chaos Computer Club talk
[Excuse me, what precise time is It?](https://media.ccc.de/v/39c3-excuse-me-what-precise-time-is-it).

## The three builds

The same binary, three ways to run it — each step adds hardware:

| Build | Hardware | What you get |
|---|---|---|
| **Docker / headless** | none — any Linux box on the PTP network | fullscreen browser clock (`/clock`), full PTP analysis web UI: visible masters, offset/delay/PDV charts, protocol profiles. See [Docker](#docker--no-led-hardware-needed) |
| **LED wallclock** | Raspberry Pi + RGB LED matrix (see [Hardware](#hardware-for-the-led-wallclock)) | the wall clock itself: multi-timezone lines, display styles, GM alerts — plus everything above |
| **Measurement instrument** | CM5 on an IO board + GNSS HAT, one wire and one bridge | a GNSS-disciplined reference that measures your grandmaster against GPS truth to ±200 ns, and can itself be the grandmaster on all three profiles. See [GNSS grandmaster mode](#gnss-grandmaster-mode) and [The precision build](#the-precision-build-hardware-pps-capture-on-a-cm5) |

The measurement build works in stages: a GNSS HAT on any Pi gives the
grandmaster + time-error measurement at few-µs accuracy over GPIO (Pi 5
with hardware timestamps: ~±1–3 µs); the CM5 hardware capture removes
the last microseconds.

## Quick start

**LED clock on a Raspberry Pi** — one command builds the matrix library
and the clock, installs fonts and a systemd service:

```bash
git clone https://github.com/Gemini2350/ptp-wallclock.git
cd ptp-wallclock
sudo ./install.sh
```

Settings page: `http://<pi-address>:8319` · logs:
`journalctl -u ptp-wallclock -f`

**Docker / headless** — prebuilt multi-arch images, details
[below](#docker--no-led-hardware-needed):

```bash
docker compose up -d
```

**GNSS receiver attached?** Install with `sudo ./install.sh --gnss`
(reboots automatically when it changed the boot config) — see
[GNSS grandmaster mode](#gnss-grandmaster-mode).

**Manual build**: `make` (matrix in `/opt/rpi-rgb-led-matrix`, or pass
`MATRIX_DIR=`), or `make headless` for the no-LED binary; run with
`sudo ./ptp-clock`.

## Features

- **Real PTP client** — end-to-end delay mechanism with one-step and
  two-step masters, correction fields, BMCA election across all visible
  masters with automatic failover, automatic domain detection (or fixed
  0–255)
- **Three protocol profiles**, selectable live in the web UI: **PTPv2**
  (IEEE 1588 over UDP, default), **gPTP / IEEE 802.1AS** (AVB, Milan —
  raw Ethernet, peer delay, answers Pdelay_Req so bridges keep it
  asCapable) and **PTPv1** (IEEE 1588-2002, mapped onto the modern UI)
- **Hardware timestamping** when the NIC has a PHC (Pi 5, CM4/CM5,
  Intel i210/i225…): packets stamped by the NIC, display driven by the
  PTP hardware clock, transparent software fallback everywhere else
- **LED display** — a configurable list of clock lines: any IANA time
  zone plus UTC/TAI (world clock), styles from digital 24h/12h over
  Unix timestamp, binary BCD and flip clock to a DCF77 telegram and a
  live PTP-stats view, all with nine fractional digits; a second line
  shows date, grandmaster or clock quality; red alerts for GM changes,
  unaccepted GMs and offset violations
- **Web interface** on port 8319 — live ticking PTP clock, all
  settings, decoded grandmaster data, a table of all visible masters,
  and 5-minute analysis charts (sync PDV, path delay, message rates,
  GNSS time error)
- **GNSS grandmaster mode** — slave-only by default (disciplines the
  clock from GPS and *measures* the network grandmaster against it);
  an explicitly confirmed master mode transmits on all three profiles
- Runs entirely in user space; one-step installer, Docker images for
  amd64 / arm64 / arm-v7

## Hardware for the LED wallclock

Only the physical LED clock needs hardware — the
[Docker version](#docker--no-led-hardware-needed) runs anywhere Docker
runs, on any network that carries PTP.

- Raspberry Pi (tested on 3/4/5; Pi 5 works since `rpi-rgb-led-matrix`
  gained its RP1 backend — `install.sh` always builds the current
  library)
- RGB LED matrix compatible with `rpi-rgb-led-matrix`:
  - [Adafruit RGB Matrix HAT](https://www.adafruit.com/product/2345)
  - 2 × [HUB75 LED panel 32×64](https://www.waveshare.com/RGB-Matrix-P3-64x32.htm) (32×128 total)
- Ethernet on a PTP-capable network (grandmaster or PTP-enabled switch)

## Docker — no LED hardware needed

The clock runs headless in a container: same PTP client, same web
interface, and the LED panel is replaced by a fullscreen browser clock
at `http://<host>:8319/clock` — glowing digits in the configured color,
date, grandmaster status line and the GM alerts. Click the page for
fullscreen; brightness and blackout from the settings page apply here
too.

<p align="center"><img src="./browser-clock.png" alt="Fullscreen browser clock showing 20:53:54.812497625 with grandmaster status line" width="800" height="auto"/></p>

Images are on Docker Hub as
[`gemini2350/ptp-wallclock`](https://hub.docker.com/r/gemini2350/ptp-wallclock)
(built by CI from this repo):

```bash
docker run -d --network host --restart unless-stopped \
    -v ptp-wallclock:/var/lib/ptp-wallclock \
    --name ptp-wallclock gemini2350/ptp-wallclock
```

- `--network host` is required for the PTP multicast on UDP 319/320
  (works on Linux hosts; Docker Desktop on macOS/Windows does not pass
  host multicast through)
- interface auto-detection joins every IPv4 interface; pin one with
  `-e PTP_WALLCLOCK_IFACE=eth0` or in the web UI (the volume keeps
  settings)
- hardware timestamping inside the container needs
  `--cap-add NET_ADMIN --device /dev/ptp0` (commented lines in
  `docker-compose.yml`); GNSS mode additionally
  `--device /dev/serial0 --device /dev/pps0`
- build it yourself with `docker build -t ptp-wallclock .`

## Web Interface

<p align="center"><img src="./web-settings.png" alt="Web interface with live PTP time, status panel, BMCA master list, and settings" width="560" height="auto"/></p>

The settings page shows the current PTP time as a live, smoothly
ticking clock with all nine fractional digits. The fractional digits —
LED and browser alike — use a per-position speed ladder: the tenths
digit is real, and every further digit visibly changes faster than the
one before. The true values change far too fast for any display, so the
fast digits are synthesized; what you see is ordered acceleration
instead of uniform flicker.

**Display settings** — color picker, brightness slider (instant),
one-click blackout (PTP keeps running), 180° rotation (live), and the
**clock lines** list: per line a time zone (UTC, TAI, or any IANA zone —
a world clock like `New York / Zurich / Tokyo` is just three lines), a
style (digital 24h/12h, Unix timestamp, binary BCD, flip clock, DCF77
telegram, live PTP stats) and an optional label (`%Z` = zone
abbreviation). One line is static, several alternate every 4 seconds,
PTP-second aligned. Optional second line: date (`DD.MM.YYYY`, ISO 8601
or `MM/DD/YYYY`), grandmaster ID, priorities & clock quality. The
browser clock follows the same list (pixel styles fall back to
digital).

**PTP settings** — protocol profile (v2 / gPTP / v1, applied live),
domain (auto-detect or fixed), network interface (auto = all IPv4
interfaces, hotplug-aware; or pinned), and three watchdogs:

- **Acceptable grandmasters** — comma-separated identity list; an
  elected GM not on the list raises a persistent red
  `! UNACCEPTED GM !` on matrix, browser clock and settings page
  (empty = any GM is fine)
- **Grandmaster change notification** — `! NEW GM !` in red for 10 s
  plus a browser notification (needs notification permission; on plain
  HTTP some browsers only show the in-page banner)
- **Offset warning threshold (ns)** — a sync deviating more than this
  from the smoothed offset (or, in GNSS mode, the network master
  deviating from GNSS) raises `! OFFSET +12.3us !` for 10 s

**Status panel** — everything decoded from the Announce messages
(identity, priorities, class/accuracy/variance, steps removed, time
source), the grandmaster vendor resolved offline from its OUI, TAI−UTC,
measured mean path delay, the active timestamping mode and the running
build revision. A separate table lists every master visible in the
domain with the elected one marked — handy for watching a failover.

**PTP analysis** — charts of the last 5 minutes on a true time axis:
sync PDV, path-delay samples, message rates per second, and (with GNSS)
the time error against GPS. Percentile autoscaling keeps the µs range
readable across master switches; ⓘ buttons explain each chart with
reference values. Click a chart (or *large view*) for the fullscreen
`/analysis` page; *reset* clears the history. The same live figures are
available on the matrix as the `PTP stats` clock-line style.

<p align="center"><img src="./browser-clock2.png" alt="Browser clock showing the red NEW GM alert after a grandmaster change" width="800" height="auto"/></p>

## Configuration file

Settings persist as `key=value` pairs in
`/var/lib/ptp-wallclock/ptp-wallclock.conf` (installed) or
`ptp-wallclock.conf` in the working directory (override with
`PTP_WALLCLOCK_CONF`). Two keys are file-only (restart required):

| Key         | Default | Meaning                                    |
|-------------|---------|--------------------------------------------|
| `http_port` | `8319`  | Port of the web interface                  |
| `hwts`      | `1`     | Try PTP hardware timestamping, `0` = never |

## GNSS grandmaster mode

With a GNSS receiver the wallclock can *be* the PTP grandmaster instead
of just displaying one — and measure your existing grandmaster against
GPS. It needs two signals:

- **NMEA** on a serial port (which second it is) — default
  `/dev/serial0`, 9600 baud
- **PPS** (exactly when the second starts) — default `/dev/pps0` via
  GPIO. GPIO timestamps carry the kernel's interrupt latency
  (µs-class); the clock compensates with a kernel-measured
  PHC↔REALTIME translation and a spike gate. On boards whose NIC
  exposes the SYNC_IN pin (CM4/CM5 on an IO board — the Pi 5 Model B
  does not), pick `phc` as the PPS device instead: the pulse is then
  hardware-timestamped by the PHC itself — see
  [the precision build](#the-precision-build-hardware-pps-capture-on-a-cm5)

Example receiver: the Waveshare MAX-M8Q GNSS HAT (PPS on GPIO 18 out of
the box, stacks under the LED matrix HAT without pin conflicts). Both
device fields in the UI are dropdowns listing what exists on the
system. OS preparation is one command, then a reboot (automatic when
something changed):

```bash
sudo ./install.sh --gnss
```

It enables the serial port on the good PL011 UART (`disable-bt`), loads
the `pps-gpio` overlay (GPIO 18, override with `GNSS_PPS_GPIO=nn`),
removes the serial login console, and installs `pps-tools` — all
idempotent with `.wallclock.bak` backups. Verify with
`sudo ppstest /dev/pps0` and `timeout 3 cat /dev/serial0`.

For the last microsecond on the GPIO path, `sudo ./install.sh
--gnss-tune` installs a boot-time service that sets the `performance`
governor and pins the PPS interrupt to a quiet CPU — and re-check the
**GNSS PPS offset** calibration after tuning changes, the latency floor
moves with it. Calibration is one click: **Set from current time
error** folds the measured mean time error into the offset and
re-centers the chart on zero (needs GNSS lock and a comparison master
with ≥30 Syncs).

Activation is deliberately two-staged:

1. **Use a GNSS receiver** — slave only (default): the clock runs on
   GNSS time and *measures* the network grandmaster against it, but
   never transmits PTP. Safe on any network.
2. **Master mode** — a separate, explicitly confirmed switch: only with
   it does the clock join the BMCA and, if it wins, send Announce/Sync
   — other PTP devices may then synchronize to it.

What you get:

- Each PPS pulse is paired with its RMC sentence into a time sample
  (u-blox qErr sawtooth correction applied automatically when
  available); the display runs directly on GNSS time, and the GNSS
  panel shows fix, satellites with per-satellite signal bars, HDOP,
  PPS age and the live **servo residual** — how tightly the clock
  follows GPS
- If a **better** grandmaster announces, the clock stays passive and
  measures it: the **Time Error** chart shows your grandmaster against
  GPS truth per Sync (path delay subtracted). The **GNSS PPS offset**
  (ns) calibrates out antenna cable delay (≈5 ns/m) and receiver bias
- If it wins the election, it transmits on the selected profile:
  v2 Announce + two-step Sync at 1 Hz with Delay_Resp; gPTP Announce
  with path-trace TLV and Sync every 125 ms; v1 two-step Sync carrying
  the dataset (stratum 1, `GPS`). clockClass 6 when locked, 5 minutes
  of clockClass 7 holdover after losing GNSS
- clockClass 6 usually beats everything on a lab network: set
  priority1 higher than your real grandmaster's if you only want the
  measurement, lower if you want it to take over

TAI − UTC (37 s since 2017) is a setting: NMEA carries UTC, PTP runs on
TAI, and the offset is announced to clients. `install.sh` installs a
udev rule + group so device reopening works after the privilege drop.

### The precision build: hardware PPS capture on a CM5

The GPIO PPS path is limited by interrupt latency (µs-class, and on a
CM5 additionally by slow MDIO clock translation). This build removes
all of it: the GNSS pulse goes into the Ethernet PHY's SYNC pin and is
captured by the **same hardware clock that timestamps the PTP
packets** — no interrupt, no clock translation. Measured on the
reference build: time-error band ≈ ±200 ns against a commercial
grandmaster (servo residual ≈ ±200 ns RMS, qErr-corrected).

**Parts**

| Part | Note |
|---|---|
| Raspberry Pi Compute Module 5 | any variant (Lite boots from microSD). The CM5's BCM54210PE PHY carries the IEEE-1588 engine with the SYNC pin — the Pi 5 Model B does **not** expose that pin; no soldering helps there |
| Waveshare CM5-IO-BASE-B | carrier with NIC; exposes the CM5's `Ethernet_SYNC_OUT` (edge pin 18) on solder pad row H3. Alternatives: CM5-NANO-A (pad H1-1), CM5-NANO-B (H5-1), official CM5IO board (J2 pin 6 — its silkscreen wrongly says 9) |
| Waveshare MAX-M8Q GNSS HAT | NMEA on serial0, PPS on GPIO 18, stacking header, backup battery |
| Active GPS antenna (SMA) | powered by the HAT through the coax; a window sill is usually enough |
| ~10 cm thin wire | one signal wire; ground is shared through the 40-pin stack |
| optional: the LED matrix HAT | the measurement variant also works headless via the web UI |

**The solder bridge (on the GNSS HAT)**

The HAT's edge header P1 (`3V3 VCC 5V 5V GND RXD TXD INT SDA SCL PPS`)
carries the timepulse on its **PPS** pin — but behind a TXS0108 level
shifter powered from the **VCC** pin, which expects an external host
to supply its logic voltage. Stacked on a Pi, nothing does. So:

- bridge **3V3 ↔ VCC** (the two adjacent pins at the end of P1)
- careful: VCC's *other* neighbour is 5V — bridging that one instead
  pushes 5 V pulses toward the PHY. Verify with a meter afterwards:
  VCC against GND must read 3.3 V, and the PPS pin must hop 0↔3.3 V
  once a second (only with a GNSS fix — no fix, no pulse, no PPS LED)

**The wire**

- from P1 **PPS** on the GNSS HAT
- to the **left pad of the H3 row** on the underside of the
  CM5-IO-BASE-B: the three-pad row inside the "IO-VREF" silkscreen
  box, labelled `RUN … GL-EN`. Left pad (next to "RUN") = SYNC input,
  middle pad = GND
- **stay away from the right pad (GL-EN)** — shorting it to ground
  powers the whole module off
- keep the wire short (<15 cm); no extra ground wire needed

**Solder-free variant** — with the official Raspberry Pi CM5 IO Board
instead of the Waveshare carrier, nothing needs soldering:

- the SYNC signal sits on the **populated J2 header, pin 6** (count the
  pins — the numbers printed on the silkscreen next to J2 are
  misleading; the perout test below confirms the right pin before you
  connect anything): a female-female Dupont wire from the GNSS HAT's
  P1 **PPS** pin is all the wiring
- the HAT-side bridge is still required — it powers the HAT's level
  shifter and is independent of the carrier board. If your HAT's P1
  header is populated, a standard 2.54 mm **jumper cap across
  3V3↔VCC** replaces the solder bridge; if P1 is bare pads, it is the
  one solder joint of the build
- the official board is larger and pricier; electrically the result is
  identical (same CM5 PHY, same pin)

**Verify before soldering** that the pad really is the PHY's 1588 pin.
The CM5 has *two* PTP clocks (RP1 MAC + PHY); the PHY is the one named
`bcm_phy_ptp` (usually `/dev/ptp0`, and `ethtool -T eth0` should
already name its index as the timestamp provider). `testptp` comes
from the kernel tree:

```bash
curl -sL https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/tools/testing/selftests/ptp/testptp.c -o testptp.c && gcc -O2 -o testptp testptp.c
```

```bash
grep . /sys/class/ptp/ptp*/clock_name
sudo ./testptp -d /dev/ptp0 -L 0,2 && sudo ./testptp -d /dev/ptp0 -p 1000000000
```

The pad now toggles once per second (multimeter against the middle GND
pad). Stop with `sudo ./testptp -d /dev/ptp0 -p 0`.

**Verify after soldering** that pulses arrive in the PHC (stop the
wallclock service first — it holds the pin):

```bash
sudo systemctl stop ptp-wallclock
sudo ./testptp -d /dev/ptp0 -L 0,1 && sudo ./testptp -d /dev/ptp0 -e 5
sudo systemctl start ptp-wallclock
```

Five events, exactly one second apart. Then pick `phc` as the **PPS
device** on the settings page and restart the service: the GNSS status
line shows "PPS hardware-stamped by the PHC" plus the live servo
residual, and the u-blox qErr sawtooth correction engages
automatically. Finally set the **GNSS PPS offset** to 0 and, once the
time-error chart has settled, click **Set from current time error**.

## Accuracy

At startup the clock probes the interface for PTP hardware timestamping
(`ETHTOOL_GET_TS_INFO`). With a PHC — Pi 5 (RP1) and CM4/CM5 have one,
Pi 3/4 do not — Sync arrival (t2) comes from the hardware RX timestamp,
the Delay_Req send time (t3) from the hardware TX timestamp on the
error queue, and the displayed time is derived from the PHC itself,
eliminating user-space scheduling jitter from the measurements. The
status panel shows the active mode, e.g. `hardware (eth0 via
/dev/ptp0)`. Without a PHC, packets are timestamped in user space and
accuracy is sub-millisecond — plenty for a wall clock, but then it is a
visualization, not a reference.

The clock is disciplined by a small **PI servo** that estimates offset
*and* the oscillator's frequency error (shown as "Clock drift"): a
crystal's ppm error no longer causes a standing lag, and displayed time
runs frequency-corrected between samples. Path-delay measurements are
computed leg by leg against the same servo model, so oscillator drift
between Sync and Delay exchange cancels instead of appearing as delay
variation. Note that LED refresh and browser rendering add their own
few milliseconds — hardware timestamping makes the *measurements*
honest, not the photons faster.

## Protocol profiles

The **Protocol profile** setting selects what the clock listens to —
switching applies immediately (fresh election, no restart):

- **PTPv2** (default): IEEE 1588 over UDP multicast, end-to-end delay
  mechanism
- **gPTP — IEEE 802.1AS / AVB / Milan**: the same v2 messages in raw
  Ethernet frames (EtherType 0x88F7, transportSpecific 1) with the
  peer-delay mechanism; the clock answers incoming Pdelay_Req — a
  silent neighbor would be declared !asCapable and cut off by 802.1AS
  bridges. Linux only (raw socket); works in Docker with
  `--network host`
- **PTPv1 — IEEE 1588-2002**: the historic format on the same UDP
  ports. No Announce in v1 — the Sync carries the dataset, mapped onto
  the modern UI (stratum → clockClass, `GPS`/`ATOM` → time source);
  the UTC timescale is converted to TAI

With master mode enabled, the grandmaster transmits on whichever
profile is selected: v2 Announce + two-step Sync at 1 Hz; gPTP Announce
with path-trace TLV at 1 Hz and Sync/Follow_Up every 125 ms (the
802.1AS default — AVB endpoints time out on slower Syncs); v1 two-step
Sync/Follow_Up at 1 Hz with the dataset in the Sync. Delay/Pdelay
requests are answered on all profiles.

## Privileged ports (why sudo?)

PTP uses UDP ports 319/320, which are privileged on Linux. The systemd
service starts as root, binds the ports and initializes the GPIO, then
the matrix library drops privileges to `daemon` by itself. For manual
runs use `sudo ./ptp-clock`, or grant the capability once:

```bash
sudo setcap cap_net_bind_service+ep ./ptp-clock
./ptp-clock
```

If binding fails, the program exits with exactly this hint.

## Open Issues

- Hardware timestamping is probed once at startup; after changing the
  interface setting, restart the service to re-probe.
