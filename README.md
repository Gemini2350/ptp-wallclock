# ptp-wallclock

<p align="center"><img src="./clock2.png" alt="High-precision LED wall clock showing 19:05:37.23269755" width="800" height="auto"/></p>

# ptp-wallclock

`ptp-wallclock` is a simple C++ application for Raspberry Pi that listens for
IEEE 1588 Precision Time Protocol (PTPv2) messages and displays the synchronized
wall-clock time on an attached LED matrix display. 

The project is intended as a lightweight, hardware-based visualization of PTP
time synchronization, useful for experiments, demos, and educational purposes.
I've used it to demonstrate that PTP is really distributing the Time at my Speech at Chaos Computer Club, 
[Excuse me, what precise time is It?](https://media.ccc.de/v/39c3-excuse-me-what-precise-time-is-it).

---

## Features

- Listens for PTPv2 (IEEE 1588) Sync / Follow_Up messages
- Computes synchronized wall-clock time
- Displays time on an RGB LED matrix
- Runs entirely in user space
- Designed for Raspberry Pi

---

## Hardware Requirements

- Raspberry Pi (tested on Raspberry Pi 3/4 - 5 not working at the moment)
- RGB LED matrix compatible with the `rpi-rgb-led-matrix` library 
-     [Adafruit RGB Matrix HAT](https://www.adafruit.com/product/2345)
-     2 x [HUB75 LED Panel 32x64 Pixel](https://www.waveshare.com/RGB-Matrix-P3-64x32.htm) (32 x 128 total)
- Network interface receiving PTP packets (typically Ethernet)

---

## Software Requirements

- Linux-based Raspberry Pi OS
- C++17-compatible compiler (e.g. `g++`)
- `rpi-rgb-led-matrix` library
- PTP-capable network environment (PTP grandmaster or PTP-enabled switch)

---

## Build Instructions

Clone the repository:

```bash
git clone https://github.com/Gemini2350/ptp-wallclock.git
cd ptp-wallclock
```

## Installation:

### Easy way:

You can Download my Raspberry Pi Image and just Flash the ISO with Rufus: [Releases](https://github.com/Gemini2350/ptp-wallclock/releases)

### Other way:

Install the Demos and Display from here: https://github.com/hzeller/rpi-rgb-led-matrix

Download my Code (compiled or uncompiled).

Compile it:

```
g++ -O2 -std=c++17 ptp-clock.cpp -o ptp-clock \
    -I./include -I./bindings \
    -L./lib -lrgbmatrix -lpthread
```

Running the Application

PTP uses UDP ports 319 and 320, which are considered privileged ports on
Linux systems. By default, binding to these ports requires root privileges.

To allow binding to these ports without running the application as root, adjust
the unprivileged port range:

```
sudo sysctl -w net.ipv4.ip_unprivileged_port_start=319
```

You can then run the application as a normal user:

```
./ptp-clock
```

Note:
The 6x13B font is not installed by default. To make it available system-wide,
copy it from the rpi-rgb-led-matrix repository:

```bash
sudo mkdir -p /usr/share/fonts/rpi-rgb-led-matrix
sudo cp fonts/6x13B.bdf /usr/share/fonts/rpi-rgb-led-matrix/
```

## References
[Excuse me, what precise time is It?](https://media.ccc.de/v/39c3-excuse-me-what-precise-time-is-it).

## Open Issues

- IGMP membership reports for the PTP multicast address `224.0.1.129` are not
  explicitly generated. Operation may depend on network multicast behavior.

- PTPv1 (IEEE 1588-2002) is not supported. The implementation targets PTPv2
  (IEEE 1588-2008) only.

- Best Master Clock Algorithm (BMCA) is not implemented. The clock passively
  listens for Sync (and Follow_Up) messages only.

- The network interface is fixed to `eth0`.




