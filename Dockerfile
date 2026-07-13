# Headless PTP wallclock: no LED hardware, the clock display is served
# in the browser at http://<host>:8319/clock
#
# PTP multicast needs the host network stack:
#   docker build -t ptp-wallclock .
#   docker run -d --network host \
#       -v ptp-wallclock:/var/lib/ptp-wallclock \
#       --name ptp-wallclock ptp-wallclock
# The PTP interface is auto-detected; pin it with -e PTP_WALLCLOCK_IFACE=eth0

FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ libc6-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY ptp-clock.cpp .
RUN g++ -O2 -std=c++17 -DNO_MATRIX ptp-clock.cpp -o ptp-clock -lpthread

FROM debian:bookworm-slim
# tzdata for the local-time display mode
RUN apt-get update \
 && apt-get install -y --no-install-recommends tzdata \
 && rm -rf /var/lib/apt/lists/* \
 && mkdir -p /var/lib/ptp-wallclock
COPY --from=build /src/ptp-clock /usr/local/bin/ptp-clock
WORKDIR /var/lib/ptp-wallclock
EXPOSE 8319
CMD ["ptp-clock"]
