# ptp-wallclock
This is a simple Wallclock for Raspberry Pi with a LED Display.
I've used it to demonstrate that PTP is really distributing the Time at my Speech at Chaos Computer Club.
A lot of people asked for it, that's why im distributing the Code here.
I'm a rellay bad Coder and most of it is based on ChatGpt.<br>
What you need:<br>
--Raspberry 1 to 4 (5 not working at the moment) <br>
--Adafruit RGB Matrix HAT https://www.adafruit.com/product/2345 <br>
--2 x HUB75 LED Panel 32x64 Pixel (-->32 x 128 in Total)<br>
--Powersupply<br>

# Installation: 
### Easyway: <br>
You can Download my Raspberry Pi Image and just Flash the ISO with Rufus<br>
<br>
### Other way: <br>
Install the Demos and Display from here: 
https://github.com/hzeller/rpi-rgb-led-matrix
Download my Code (compiled or uncompiled) <br>
Compile it with "g++ -O2 -std=c++17 ptp-clock.cpp -o ptp-clock     -I./include -I./bindings -L./lib -lrgbmatrix -lpthread


