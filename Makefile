# Manual build (the easy way is: sudo ./install.sh)
MATRIX_DIR ?= /opt/rpi-rgb-led-matrix
REV := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

ptp-clock: ptp-clock.cpp
	g++ -O2 -std=c++17 -DPTP_WALLCLOCK_REV='"$(REV)"' $< -o $@ \
	    -I$(MATRIX_DIR)/include -L$(MATRIX_DIR)/lib -lrgbmatrix -lpthread

# No LED hardware: PTP client + web interface with the /clock browser display
headless: ptp-clock.cpp
	g++ -O2 -std=c++17 -DNO_MATRIX -DPTP_WALLCLOCK_REV='"$(REV)"' $< -o ptp-clock -lpthread

clean:
	rm -f ptp-clock

.PHONY: headless clean
