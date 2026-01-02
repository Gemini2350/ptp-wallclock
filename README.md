# ptp-wallclock

<p align="center"><img src="./clock2.png" alt="High-precision LED wall clock showing 19:05:37.23269755" width="800" height="auto"/></p>

This is a simple PTP 1588 Wallclock for Raspberry Pi with a LED Display.

I've used it to demonstrate that PTP is really distributing the Time at my Speech at Chaos Computer Club, [Excuse me, what precise time is It?](https://media.ccc.de/v/39c3-excuse-me-what-precise-time-is-it).

A lot of people asked for it, that's why I'm distributing the code here.

I'm a really bad coder and most of it is based on chatGPT.

It reads the PTPv2 Sync and Follow Up messages. It's not doing Delay Requests and not doing BMCA.

It shows TAI (= 37s off to UTC). I will make this adjustable as a setting.

I did not expect this interest in it, i will update it as soon as possible.

What you need:

  - Raspberry Pi 1 to 4 (5 not working at the moment)
  - [Adafruit RGB Matrix HAT](https://www.adafruit.com/product/2345)
  - 2 x [HUB75 LED Panel 32x64 Pixel](https://www.waveshare.com/RGB-Matrix-P3-64x32.htm) (32 x 128 total)
  - Power Supply

## Installation:

### Easy way:

You can Download my Raspberry Pi Image and just Flash the ISO with Rufus: [Releases](https://github.com/Gemini2350/ptp-wallclock/releases)

### Other way:

Install the Demos and Display from here: https://github.com/hzeller/rpi-rgb-led-matrix

Download my Code (compiled or uncompiled).

Compile it:

```
g++ -O2 -std=c++17 ptp-clock.cpp -o ptp-clock -I./include -I./bindings -L./lib -lrgbmatrix -lpthread
```

I havent found a better way that the Script can access ports below 1024, in order to do this you need to add:

```
sudo sysctl -w net.ipv4.ip_unprivileged_port_start=319
```
