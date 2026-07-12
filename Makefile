# Manual build (the easy way is: sudo ./install.sh)
MATRIX_DIR ?= /opt/rpi-rgb-led-matrix

ptp-clock: ptp-clock.cpp
	g++ -O2 -std=c++17 $< -o $@ \
	    -I$(MATRIX_DIR)/include -L$(MATRIX_DIR)/lib -lrgbmatrix -lpthread

clean:
	rm -f ptp-clock

.PHONY: clean
