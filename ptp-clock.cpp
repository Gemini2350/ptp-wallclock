#include "led-matrix.h"
#include "graphics.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <cstring>
#include <iostream>
#include <csignal>
#include <ctime>
#include <map>

using namespace rgb_matrix;

volatile bool interrupt_received = false;
static void InterruptHandler(int signo) {
    interrupt_received = true;
}

struct PTPTimestamp {
    uint64_t seconds;
    uint32_t nanoseconds;
};

// ---- PTP Interpolations-Referenz ----
static uint64_t last_ptp_ns = 0;
static timespec last_mono{};
static bool have_ptp_ref = false;

// Hilfsfunktion: eth0 IP auslesen
uint32_t get_eth0_ip() {
    struct ifaddrs *ifaddr, *ifa;
    uint32_t ip = 0;

    if (getifaddrs(&ifaddr) == -1)
        return 0;

    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr)
            continue;

        if (ifa->ifa_addr->sa_family == AF_INET &&
            strcmp(ifa->ifa_name, "eth0") == 0) {

            ip = ((struct sockaddr_in*)ifa->ifa_addr)->sin_addr.s_addr;
            break;
        }
    }

    freeifaddrs(ifaddr);
    return ip;
}

int main(int argc, char **argv) {

    // --- Matrix Optionen ---
    RGBMatrix::Options matrix_options;
    RuntimeOptions runtime_opt;

    matrix_options.rows = 32;
    matrix_options.cols = 128;
    matrix_options.chain_length = 1;
    matrix_options.hardware_mapping = "adafruit-hat";
    runtime_opt.gpio_slowdown = 2;

    RGBMatrix *matrix = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
    if (!matrix) {
        std::cerr << "Failed to initialize RGB matrix\n";
        return 1;
    }

    FrameCanvas *offscreen = matrix->CreateFrameCanvas();

    // --- Font ---
    Font font;
    const char* font_path = "/usr/share/fonts/rpi-rgb-led-matrix/6x13B.bdf";
    if (!font.LoadFont(font_path)) {
        std::cerr << "Couldn't load font " << font_path << "\n";
        delete matrix;
        return 1;
    }

    signal(SIGINT, InterruptHandler);
    signal(SIGTERM, InterruptHandler);

    // --- Multicast-Sockets ---
    int sock_sync = socket(AF_INET, SOCK_DGRAM, 0);
    int sock_followup = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_sync < 0 || sock_followup < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in addr_sync{}, addr_followup{};
    addr_sync.sin_family = AF_INET;
    addr_sync.sin_port = htons(319);
    addr_sync.sin_addr.s_addr = htonl(INADDR_ANY);

    addr_followup.sin_family = AF_INET;
    addr_followup.sin_port = htons(320);
    addr_followup.sin_addr.s_addr = htonl(INADDR_ANY);

    bind(sock_sync, (struct sockaddr*)&addr_sync, sizeof(addr_sync));
    bind(sock_followup, (struct sockaddr*)&addr_followup, sizeof(addr_followup));

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr("224.0.1.129");
    mreq.imr_interface.s_addr = get_eth0_ip();
    if (mreq.imr_interface.s_addr == 0) {
        std::cerr << "eth0 IP not found\n";
        return 1;
    }

    setsockopt(sock_sync, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    setsockopt(sock_followup, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    std::cout << "Listening for PTP Messages...\n";

    // --- Hauptloop ---
    while (!interrupt_received) {

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock_sync, &fds);
        FD_SET(sock_followup, &fds);

        struct timeval tv{0, 20000}; // 20 ms
        select(std::max(sock_sync, sock_followup) + 1,
               &fds, nullptr, nullptr, &tv);

        // --- FOLLOW_UP ---
        if (FD_ISSET(sock_followup, &fds)) {
            uint8_t buf[128];
            if (recv(sock_followup, buf, sizeof(buf), 0) > 0) {

                if ((buf[0] & 0x0F) == 8) {

                    PTPTimestamp ts{};
                    for (int i = 0; i < 6; ++i)
                        ts.seconds = (ts.seconds << 8) | buf[34 + i];

                    ts.nanoseconds =
                        (buf[40] << 24) |
                        (buf[41] << 16) |
                        (buf[42] << 8)  |
                        buf[43];

                    last_ptp_ns =
                        ts.seconds * 1000000000ULL +
                        ts.nanoseconds;

                    clock_gettime(CLOCK_MONOTONIC_RAW, &last_mono);
                    have_ptp_ref = true;
                }
            }
        }

        if (!have_ptp_ref)
            continue;

        // --- fluessige Zeit ---
        timespec now;
        clock_gettime(CLOCK_MONOTONIC_RAW, &now);

        uint64_t delta_ns =
            (now.tv_sec - last_mono.tv_sec) * 1000000000ULL +
            (now.tv_nsec - last_mono.tv_nsec);

        uint64_t display_ns = last_ptp_ns + delta_ns;

        time_t sec = display_ns / 1000000000ULL;
        uint32_t nsec = display_ns % 1000000000ULL;

        struct tm tm_local;
        localtime_r(&sec, &tm_local);

        char time_buffer[64];
        snprintf(time_buffer, sizeof(time_buffer),
                 "%02d:%02d:%02d.%09u",
                 tm_local.tm_hour,
                 tm_local.tm_min,
                 tm_local.tm_sec,
                 nsec);

        offscreen->Fill(0,0,0);

        int x_center =
            (matrix_options.cols -
             font.CharacterWidth('0') * strlen(time_buffer)) / 2
            - 9;

        int y_center =
            (matrix_options.rows + font.baseline()) / 2;

        DrawText(offscreen, font,
                 x_center, y_center,
                 Color(255,255,0),
                 nullptr, time_buffer, 1);

        offscreen = matrix->SwapOnVSync(offscreen);
    }

    close(sock_sync);
    close(sock_followup);
    delete matrix;
    return 0;
}
