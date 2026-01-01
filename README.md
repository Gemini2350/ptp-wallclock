# ptp-wallclock
<p align="center"> <img src="./images/clock.png" alt="High-precision LED wall clock showing 19:05:37.23269755" width="800"/> </p>
This is a simple Wallclock for Raspberry Pi with a LED Display.
I've used it to demonstrate that PTP is really distributing the Time at my Speech at Chaos Computer Club.
A lot of people asked for it, that's why im distributing the Code here.
I'm a realy bad Coder and most of it is based on ChatGpt.<br>
It ready the PTPv2 Sync and Follow Up Messages. It's not doing Delay Requests and not doing BMCA. <br>
It shows TAI (= 37s off to UTC). I will make this adjustable as a setting <br>
I did not expect this interes in it, i will update it as soon as possible.<br>
What you need:<br>
--Raspberry 1 to 4 (5 not working at the moment) <br>
--Adafruit RGB Matrix HAT https://www.adafruit.com/product/2345 <br>
--2 x HUB75 LED Panel 32x64 Pixel (-->32 x 128 in Total)<br>
--Powersupply<br>

# Installation: 
### Easyway: <br>
You can Download my Raspberry Pi Image and just Flash the ISO with Rufus --> Releases<br>
<br>
### Other way: <br>
Install the Demos and Display from here: 
https://github.com/hzeller/rpi-rgb-led-matrix
Download my Code (compiled or uncompiled) <br>
Compile it with "g++ -O2 -std=c++17 ptp-clock.cpp -o ptp-clock     -I./include -I./bindings -L./lib -lrgbmatrix -lpthread" <br>
I havent found a better way that the Script can access ports below 1024, in order to do this you need to add "sudo sysctl -w net.ipv4.ip_unprivileged_port_start=319"



