// Build with -DNO_MATRIX for a headless PTP clock (e.g. in Docker) that
// serves only the web interface and the /clock browser display.
#ifndef NO_MATRIX
#include "led-matrix.h"
#include "graphics.h"
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <poll.h>
#include <fcntl.h>
#include <algorithm>
#include <termios.h>
#ifdef __linux__
#include <linux/pps.h>
#endif
#ifdef __linux__
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>
#include <linux/errqueue.h>
#endif
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <csignal>
#include <ctime>
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <sstream>
#include <fstream>
#include <map>
#include <vector>
#include <cmath>

#ifndef NO_MATRIX
using namespace rgb_matrix;

// Draws through to the real canvas, optionally rotated by 180 degrees —
// for LED panels that are mounted upside down. A live web setting, so the
// rotation happens here at draw time instead of in a pixel mapper, which
// the library only applies at matrix-creation time.
class FlipCanvas : public Canvas {
public:
    Canvas *inner = nullptr;
    bool flip = false;
    int width() const override { return inner->width(); }
    int height() const override { return inner->height(); }
    void SetPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) override {
        if (flip)
            inner->SetPixel(inner->width() - 1 - x,
                            inner->height() - 1 - y, r, g, b);
        else
            inner->SetPixel(x, y, r, g, b);
    }
    void Clear() override { inner->Clear(); }
    void Fill(uint8_t r, uint8_t g, uint8_t b) override { inner->Fill(r, g, b); }
};

// Draws through to another canvas but discards pixels outside a vertical
// window — used for the flip-clock roll animation.
class ClipCanvas : public Canvas {
public:
    Canvas *inner = nullptr;
    int y0 = 0, y1 = 31;
    int width() const override { return inner->width(); }
    int height() const override { return inner->height(); }
    void SetPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) override {
        if (y >= y0 && y <= y1)
            inner->SetPixel(x, y, r, g, b);
    }
    void Clear() override { inner->Clear(); }
    void Fill(uint8_t r, uint8_t g, uint8_t b) override { inner->Fill(r, g, b); }
};

// Fractional digits with a per-position speed ladder: the tenths digit is
// the true value; every further digit gets its own visible update rate
// (~10/s at the front, geometrically faster towards the back). The true
// values change far too fast for any display — sample-and-hold with a
// deterministic hash makes the eye see an ordered acceleration instead of
// uniform flicker.
static void format_fraction(char out[10], uint64_t t_ns) {
    static const uint64_t kHold[9] = {
        100000000ULL, 66000000ULL, 43600000ULL, 28700000ULL, 19000000ULL,
        12500000ULL, 8300000ULL, 5500000ULL, 3600000ULL};
    out[0] = (char)('0' + (t_ns / 100000000ULL) % 10);
    for (int k = 1; k < 9; ++k) {
        uint32_t idx = (uint32_t)(t_ns / kHold[k]);
        uint32_t v = idx * 2654435761u + (uint32_t)(k + 1) * 40503u;
        out[k] = (char)('0' + (v >> 16) % 10);
    }
    out[9] = 0;
}

// Short time-source label for the matrix details line (IEEE 1588 table 7)
static const char *time_source_short(uint8_t ts) {
    switch (ts) {
        case 0x10: return "ATOM";
        case 0x20: return "GNSS";
        case 0x30: return "RADIO";
        case 0x40: return "PTP";
        case 0x50: return "NTP";
        case 0x60: return "HAND";
        case 0x90: return "OTHER";
        case 0xA0: return "OSC";
    }
    return nullptr;
}
#endif

volatile bool interrupt_received = false;
static void InterruptHandler(int signo) {
    (void)signo;
    interrupt_received = true;
}

// ---- Settings (served / edited via the web interface) ----
enum TimeMode { MODE_UTC = 0, MODE_TAI = 1, MODE_LOCAL = 2, MODE_CYCLE = 3 };
enum DateFormat { DATE_DMY = 0, DATE_ISO = 1, DATE_MDY = 2 };

// One clock line on the LED display. The display shows a configurable
// list of these: a single entry is static, several alternate every 4 s.
struct ClockEntry {
    std::string zone = "UTC";             // "UTC", "TAI", or an IANA zone
                                          // name like "Europe/Zurich"
    std::string style = "24h";            // rendering style, see kStyles
    std::string name;                     // label; "" = none, "%Z" = zone
};

static const char *kStyles[] = {"24h", "12h", "unix", "bcd", "flip",
                                "dcf77", "graph", "rates"};

static bool style_valid(const std::string &st) {
    for (const char *k : kStyles)
        if (st == k)
            return true;
    return false;
}

// Serialized form: "zone,style,name;zone,style,name" — the name is
// everything after the second comma, so commas inside names are fine.
// The legacy scale tokens utc/tai/local are still understood; "local"
// maps to the configured legacy time zone (see clocks_parse's caller).
static std::string clocks_serialize(const std::vector<ClockEntry> &v) {
    std::string out;
    for (const auto &e : v) {
        if (!out.empty())
            out += ";";
        out += e.zone;
        out += ",";
        out += e.style;
        out += ",";
        out += e.name;
    }
    return out;
}

static std::vector<ClockEntry> clocks_parse(const std::string &in,
                                            const std::string &legacy_tz) {
    std::vector<ClockEntry> out;
    std::stringstream ss(in);
    std::string item;
    while (std::getline(ss, item, ';')) {
        size_t c1 = item.find(',');
        if (c1 == std::string::npos)
            continue;
        size_t c2 = item.find(',', c1 + 1);
        if (c2 == std::string::npos)
            continue;
        ClockEntry e;
        e.zone = item.substr(0, c1);
        if (e.zone == "utc")
            e.zone = "UTC";
        else if (e.zone == "tai")
            e.zone = "TAI";
        else if (e.zone == "local")
            e.zone = legacy_tz.empty() ? "UTC" : legacy_tz;
        if (e.zone.empty() || e.zone.size() > 48)
            e.zone = "UTC";
        e.style = item.substr(c1 + 1, c2 - c1 - 1);
        if (!style_valid(e.style))
            e.style = "24h";
        e.name = item.substr(c2 + 1);
        if (e.name.size() > 24)
            e.name.resize(24);
        out.push_back(e);
        if (out.size() >= 12)
            break;
    }
    return out;
}

struct Settings {
    uint8_t r = 255, g = 255, b = 0;      // display color
    int brightness = 100;                 // display brightness in percent
    bool blackout = false;                // display temporarily off
    bool rotate180 = false;               // LED matrix mounted upside down
    bool show_gm = false;                 // show grandmaster ID on the matrix
    bool show_gm_details = false;         // show priorities / clock quality
    bool show_date = false;               // show the date on the matrix
    int mode = MODE_UTC;                  // UTC / TAI / local time
    std::string timezone = "Europe/Berlin";
    std::string tz_label;                 // custom label for the local time
                                          // scale; empty = %Z abbreviation
    std::string utc_label;                // custom label for UTC; empty = UTC
    std::string tai_label;                // custom label for TAI; empty = TAI
    bool show_zone = false;               // show the scale label also in
                                          // the fixed (non-cycle) modes
    std::vector<ClockEntry> clocks;       // LED display clock lines
    int time_format = 24;                 // 24 or 12 (AM/PM)
    int date_format = DATE_DMY;           // DD.MM.YYYY / ISO / MM/DD/YYYY
    bool notify_gm_change = false;        // notify on grandmaster change
    int http_port = 8319;
    bool hwts = true;                     // try PTP hardware timestamping
    bool gm_enable = false;               // act as GNSS grandmaster
    std::string gm_serial = "/dev/serial0";  // NMEA source
    std::string gm_pps = "/dev/pps0";     // PPS source (pps-gpio)
    int gm_prio1 = 128, gm_prio2 = 128;   // our announce priorities
    int gm_utc_offset = 37;               // TAI - UTC for the announce
    int domain = -1;                      // PTP domain, -1 = auto detect
    std::string iface = "auto";           // interface, "auto" = all of them
    std::string acceptable_gms;           // comma-separated GM identities;
                                          // empty = any GM is acceptable
};

static Settings g_settings;
static std::mutex g_mutex;                // guards g_settings and GM state
static std::string g_config_path = "ptp-wallclock.conf";
static std::string g_joined_names;        // joined ifaces, guarded by g_mutex

// ---- Grandmaster info from Announce messages ----
struct GMInfo {
    uint8_t id[8] = {0};
    uint8_t priority1 = 0, priority2 = 0;
    uint8_t clock_class = 0, clock_accuracy = 0;
    uint16_t variance = 0;
    uint16_t steps_removed = 0;
    uint8_t time_source = 0;
};
static GMInfo g_gm;                       // elected master, guarded by g_mutex
static bool g_have_gm = false;
static uint32_t g_gm_changes = 0;
static timespec g_last_gm_change{};       // CLOCK_MONOTONIC_RAW

// ---- BMCA: all masters seen announcing in the domain ----
struct ForeignMaster {
    uint8_t sender[10];                   // sourcePortIdentity of the Announce
    GMInfo gm;                            // advertised grandmaster dataset
    uint64_t last_seen = 0;               // mono ns
    uint64_t timeout_ns = 3000000000ULL;  // 3 x announce interval
    bool elected = false;
};
static std::vector<ForeignMaster> g_masters;   // guarded by g_mutex
static bool g_have_master = false;             // main thread
static uint8_t g_elected_sender[10] = {0};     // main thread

// ---- PTP client state (main thread only) ----
struct PendingSync {
    bool valid = false;
    uint16_t seq = 0;
    uint8_t src[10];                      // sourcePortIdentity of the Sync
    uint64_t t2_mono = 0;                 // local arrival time
    int64_t corr_ns = 0;                  // Sync correctionField
};
static PendingSync g_pending_sync;

static bool g_have_pair = false;          // valid (t1, t2) from last Sync
static int64_t g_last_t1 = 0;             // master send time (TAI ns, corrected)
static int64_t g_last_t2 = 0;             // local arrival time (mono ns)

struct PendingDelayReq {
    bool valid = false;
    uint16_t seq = 0;
    uint64_t t3_mono = 0;                 // local send time
};
static PendingDelayReq g_pending_dreq;
static uint16_t g_dreq_seq = 0;

static bool g_have_mpd = false;
static int64_t g_mpd_ns = 0;              // mean path delay (EMA)
static bool g_have_offset = false;
static int64_t g_offset_ns = 0;           // local(mono) - master(TAI), EMA

static uint8_t g_clock_id[8] = {0};       // our clockIdentity (EUI-64 from MAC)

// ---- Shared with the HTTP thread ----
static std::atomic<bool> have_ptp_ref{false};
static std::atomic<int16_t> current_utc_offset{0};
static std::atomic<long long> g_offset_atomic{0};  // mirror of g_offset_ns
static std::atomic<long long> g_path_delay_ns{0};
static std::atomic<unsigned long long> g_last_sync_mono_ns{0};
static std::atomic<uint32_t> g_dreq_sent{0};
static std::atomic<uint32_t> g_dresp_received{0};
static std::atomic<int> g_domain{-1};          // configured, -1 = auto
static std::atomic<int> g_active_domain{-1};   // detected in auto mode
static std::atomic<unsigned long long> g_last_announce_mono_ns{0};
static std::atomic<bool> g_reset_ptp{false};   // web thread requests state reset
static std::atomic<bool> g_iface_changed{false};  // web thread changed iface
static std::atomic<bool> g_iface_up{false};    // multicast currently joined

// ---- PTP analysis history (rings guarded by g_mutex) ----
static const int kHistN = 150;
static int32_t g_hist_off[kHistN];             // offset deviation per Sync, ns
static int g_hist_off_n = 0, g_hist_off_i = 0;
static int32_t g_hist_del[kHistN];             // path delay samples, ns
static int g_hist_del_n = 0, g_hist_del_i = 0;
static int32_t g_hist_cmp[kHistN];             // wire PTP - GNSS, ns
static int g_hist_cmp_n = 0, g_hist_cmp_i = 0;
struct RateSample { uint16_t sync, fup, ann, dresp; };
static RateSample g_hist_rate[kHistN];         // received messages per second
static int g_hist_rate_n = 0, g_hist_rate_i = 0;
// per-second counters (domain-filtered wire view), reset every second
static std::atomic<uint32_t> g_cnt_sync{0}, g_cnt_fup{0};
static std::atomic<uint32_t> g_cnt_ann{0}, g_cnt_dresp{0};

// ---- PTP hardware timestamping (Linux, optional) ----
// When available, the NIC's PHC becomes the local timing clock: Sync
// arrival (t2) comes from the hardware RX timestamp, the Delay_Req send
// time (t3) from the TX error queue, and the display reads the PHC.
static bool g_hwts = false;
static int g_phc_fd = -1;
static std::string g_hwts_desc;           // e.g. "eth0 via /dev/ptp0"
static std::atomic<unsigned long long> g_last_sync_age_ns{0};  // mono, for UI

// ---- PTP grandmaster mode (GNSS-disciplined) ----
// Role: CLIENT = plain client; GM_WAIT = grandmaster mode on but no
// usable GNSS yet (behaves like a client); GM_PASSIVE = GNSS drives the
// clock but a better master won the BMCA; GM_ACTIVE = we are the elected
// grandmaster and transmit Announce/Sync.
enum { ROLE_CLIENT = 0, ROLE_GM_WAIT = 1, ROLE_GM_PASSIVE = 2,
       ROLE_GM_ACTIVE = 3 };
static std::atomic<int> g_role{ROLE_CLIENT};
static std::atomic<bool> g_gnss_lock{false};
static std::atomic<int> g_gnss_sats_used{-1};       // GGA: satellites used
static std::atomic<int> g_gnss_sats_view{-1};       // GSV: in view
static std::atomic<int> g_gnss_fixq{0};             // GGA fix quality
static std::atomic<int> g_gnss_hdop10{-1};          // HDOP * 10
static std::atomic<unsigned long long> g_gnss_last_pps{0};     // mono
static std::atomic<unsigned long long> g_gnss_last_sample{0};  // mono
static std::atomic<uint32_t> g_gnss_sample_count{0};
static std::atomic<bool> g_gnss_serial_ok{false};
static std::atomic<bool> g_gnss_pps_ok{false};
static std::atomic<int> g_gnss_serial_fd{-1};       // opened while root
static std::atomic<int> g_gnss_pps_fd{-1};

// Satellites in view (from GSV) and the pending time sample handed from
// the GNSS thread to the main loop — guarded by g_gnss_mutex. Lock order:
// g_mutex may be taken first, never the other way around.
struct SatInfo { char talker[3]; int prn; int snr; uint64_t seen; };
static std::mutex g_gnss_mutex;
static std::vector<SatInfo> g_gnss_sats;
static bool g_gnss_sample_valid = false;
static int64_t g_gnss_t1 = 0, g_gnss_t2 = 0;

// Wire-PTP vs GNSS comparison (clock GNSS-driven, network Syncs measured
// against it; positive = the network master is ahead of GNSS)
static uint8_t g_cmp_target[10] = {0};    // main thread: who we compare to
static std::atomic<bool> g_cmp_target_valid{false};
static bool g_cmp_have = false;           // main thread (EMA state)
static int64_t g_cmp_ns = 0;
static std::atomic<long long> g_cmp_atomic{0};
static std::atomic<long long> g_cmp_last{0};
static std::atomic<uint32_t> g_cmp_count{0};
static char g_cmp_gm_str[32] = "";        // guarded by g_mutex

static uint16_t g_ann_seq = 0, g_sync_seq = 0;      // main thread

static void hist_push(int32_t *buf, int &n, int &i, int64_t v) {
    if (v > 2000000000LL) v = 2000000000LL;
    if (v < -2000000000LL) v = -2000000000LL;
    buf[i] = (int32_t)v;
    i = (i + 1) % kHistN;
    if (n < kHistN) n++;
}

// ---- Helpers ----
static uint64_t mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// The local timing clock: the NIC's PHC when hardware timestamping is
// active (packet timestamps come from the same clock), else monotonic.
static uint64_t local_clock_ns() {
#ifdef __linux__
    if (g_hwts) {
        timespec ts;
        clockid_t cid = (clockid_t)((~(clockid_t)g_phc_fd) << 3) | 3;
        if (clock_gettime(cid, &ts) == 0)
            return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    }
#endif
    return mono_ns();
}

static uint64_t be_bytes(const uint8_t *p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i)
        v = (v << 8) | p[i];
    return v;
}

// PTP 10-byte timestamp (48-bit seconds + 32-bit nanoseconds) -> ns
static uint64_t ts_to_ns(const uint8_t *p) {
    return be_bytes(p, 6) * 1000000000ULL + be_bytes(p + 6, 4);
}

// correctionField (int64, ns * 2^16) -> ns
static int64_t corr_to_ns(const uint8_t *p) {
    return (int64_t)be_bytes(p, 8) >> 16;
}

// ---- OUI → vendor lookup ----
// The grandmaster identity is an EUI-64 derived from the MAC address, so
// its first three bytes are the vendor OUI. Curated offline subset of
// brands commonly seen as PTP grandmasters (dedicated GMs, NICs running
// linuxptp, AV gear) — extend here as needed.
struct OuiEntry { uint8_t o[3]; const char *vendor; };
static const OuiEntry kOuiTable[] = {
    {{0xEC, 0x46, 0x70}, "Meinberg"},
    {{0x20, 0xB0, 0xF7}, "Mobatime (Enclustra SoM)"},
    {{0x00, 0x1D, 0xC1}, "Audinate (Dante)"},
    {{0x00, 0x15, 0x17}, "Intel"},
    {{0x00, 0x1B, 0x21}, "Intel"},
    {{0x68, 0x05, 0xCA}, "Intel"},
    {{0xA0, 0x36, 0x9F}, "Intel"},
    {{0x00, 0x02, 0xC9}, "Mellanox/NVIDIA"},
    {{0xEC, 0x0D, 0x9A}, "Mellanox/NVIDIA"},
    {{0xB8, 0x59, 0x9F}, "Mellanox/NVIDIA"},
    {{0x00, 0x0F, 0x53}, "Solarflare/AMD"},
    {{0x00, 0x0A, 0x35}, "Xilinx/AMD"},
    {{0x00, 0x1C, 0x73}, "Arista"},
    {{0x00, 0x00, 0x0C}, "Cisco"},
    {{0x00, 0x05, 0x85}, "Juniper"},
    {{0x00, 0x04, 0x96}, "Extreme Networks"},
    {{0xF0, 0x9F, 0xC2}, "Ubiquiti"},
    {{0x00, 0x90, 0xE8}, "Moxa"},
    {{0x08, 0x00, 0x11}, "Tektronix"},
    {{0x00, 0xA0, 0xDE}, "Yamaha"},
    {{0x00, 0x04, 0xA3}, "Microchip"},
    {{0x7C, 0x2E, 0x0D}, "Blackmagic Design"},
    {{0xB8, 0x27, 0xEB}, "Raspberry Pi"},
    {{0xDC, 0xA6, 0x32}, "Raspberry Pi"},
    {{0xE4, 0x5F, 0x01}, "Raspberry Pi"},
    {{0xD8, 0x3A, 0xDD}, "Raspberry Pi"},
    {{0x2C, 0xCF, 0x67}, "Raspberry Pi"},
    {{0x28, 0xCD, 0xC1}, "Raspberry Pi"},
    {{0x8C, 0x1F, 0x64}, "IEEE RA (shared block)"},
};

// Vendor for a grandmaster identity, "" when the OUI is not in the table
static const char *lookup_vendor(const uint8_t id[8]) {
    for (const auto &e : kOuiTable)
        if (e.o[0] == id[0] && e.o[1] == id[1] && e.o[2] == id[2])
            return e.vendor;
    if (id[0] & 0x02)
        return "locally administered";   // random/software MAC
    return "";
}

static std::string format_gm(const uint8_t id[8]) {
    char buf[32];
    snprintf(buf, sizeof(buf),
             "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
             id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
    return buf;
}

static void apply_timezone(const std::string &tz) {
    setenv("TZ", tz.c_str(), 1);
    tzset();
}

// Per-clock-line zone switching for the LED renderer (main thread only).
// glibc caches the parsed zone, so this is cheap while the zone is stable.
static std::string g_render_tz;
static void ensure_tz(const std::string &tz) {
    if (tz == g_render_tz)
        return;
    setenv("TZ", tz.c_str(), 1);
    tzset();
    g_render_tz = tz;
}

// ---- Config persistence (key=value lines) ----
static void resolve_config_path() {
    const char *env = getenv("PTP_WALLCLOCK_CONF");
    if (env && *env) {
        g_config_path = env;
        return;
    }
    struct stat st;
    if (stat("/var/lib/ptp-wallclock", &st) == 0 && S_ISDIR(st.st_mode)) {
        g_config_path = "/var/lib/ptp-wallclock/ptp-wallclock.conf";
        return;
    }
    g_config_path = "ptp-wallclock.conf";
}

static void save_settings_locked() {
    std::ofstream f(g_config_path);
    if (!f) {
        std::cerr << "Could not write " << g_config_path << "\n";
        return;
    }
    f << "color=" << (int)g_settings.r << "," << (int)g_settings.g << ","
      << (int)g_settings.b << "\n";
    f << "brightness=" << g_settings.brightness << "\n";
    f << "blackout=" << (g_settings.blackout ? 1 : 0) << "\n";
    f << "rotate180=" << (g_settings.rotate180 ? 1 : 0) << "\n";
    f << "show_gm=" << (g_settings.show_gm ? 1 : 0) << "\n";
    f << "show_gm_details=" << (g_settings.show_gm_details ? 1 : 0) << "\n";
    f << "show_date=" << (g_settings.show_date ? 1 : 0) << "\n";
    f << "mode=" << g_settings.mode << "\n";
    f << "timezone=" << g_settings.timezone << "\n";
    f << "tz_label=" << g_settings.tz_label << "\n";
    f << "utc_label=" << g_settings.utc_label << "\n";
    f << "tai_label=" << g_settings.tai_label << "\n";
    f << "show_zone=" << (g_settings.show_zone ? 1 : 0) << "\n";
    f << "clocks=" << clocks_serialize(g_settings.clocks) << "\n";
    f << "time_format=" << g_settings.time_format << "\n";
    f << "date_format=" << (g_settings.date_format == DATE_ISO ? "iso" :
                            g_settings.date_format == DATE_MDY ? "mdy" :
                            "dmy") << "\n";
    f << "notify_gm_change=" << (g_settings.notify_gm_change ? 1 : 0) << "\n";
    f << "http_port=" << g_settings.http_port << "\n";
    if (g_settings.domain < 0)
        f << "domain=auto\n";
    else
        f << "domain=" << g_settings.domain << "\n";
    f << "iface=" << g_settings.iface << "\n";
    f << "hwts=" << (g_settings.hwts ? 1 : 0) << "\n";
    f << "gm_enable=" << (g_settings.gm_enable ? 1 : 0) << "\n";
    f << "gm_serial=" << g_settings.gm_serial << "\n";
    f << "gm_pps=" << g_settings.gm_pps << "\n";
    f << "gm_prio1=" << g_settings.gm_prio1 << "\n";
    f << "gm_prio2=" << g_settings.gm_prio2 << "\n";
    f << "gm_utc_offset=" << g_settings.gm_utc_offset << "\n";
    f << "acceptable_gms=" << g_settings.acceptable_gms << "\n";
}

static void load_settings() {
    std::ifstream f(g_config_path);
    std::string line;
    while (f && std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "color") {
            int r, g, b;
            if (sscanf(val.c_str(), "%d,%d,%d", &r, &g, &b) == 3) {
                g_settings.r = r; g_settings.g = g; g_settings.b = b;
            }
        } else if (key == "brightness") {
            int v = atoi(val.c_str());
            if (v >= 1 && v <= 100)
                g_settings.brightness = v;
        } else if (key == "blackout") {
            g_settings.blackout = (val == "1");
        } else if (key == "rotate180") {
            g_settings.rotate180 = (val == "1");
        } else if (key == "show_gm") {
            g_settings.show_gm = (val == "1");
        } else if (key == "show_gm_details") {
            g_settings.show_gm_details = (val == "1");
        } else if (key == "show_date") {
            g_settings.show_date = (val == "1");
        } else if (key == "mode") {
            int m = atoi(val.c_str());
            if (m >= MODE_UTC && m <= MODE_CYCLE)
                g_settings.mode = m;
        } else if (key == "timezone") {
            if (!val.empty())
                g_settings.timezone = val;
        } else if (key == "tz_label") {
            g_settings.tz_label = val;
        } else if (key == "utc_label") {
            g_settings.utc_label = val;
        } else if (key == "tai_label") {
            g_settings.tai_label = val;
        } else if (key == "show_zone") {
            g_settings.show_zone = (val == "1");
        } else if (key == "clocks") {
            g_settings.clocks = clocks_parse(val, g_settings.timezone);
        } else if (key == "time_format") {
            if (val == "12" || val == "24")
                g_settings.time_format = atoi(val.c_str());
        } else if (key == "date_format") {
            if (val == "iso") g_settings.date_format = DATE_ISO;
            else if (val == "mdy") g_settings.date_format = DATE_MDY;
            else if (val == "dmy") g_settings.date_format = DATE_DMY;
        } else if (key == "notify_gm_change") {
            g_settings.notify_gm_change = (val == "1");
        } else if (key == "http_port") {
            int p = atoi(val.c_str());
            if (p > 0 && p < 65536)
                g_settings.http_port = p;
        } else if (key == "domain") {
            if (val == "auto") {
                g_settings.domain = -1;
            } else {
                int d = atoi(val.c_str());
                if (d >= 0 && d <= 255)
                    g_settings.domain = d;
            }
        } else if (key == "iface") {
            if (!val.empty())
                g_settings.iface = val;
        } else if (key == "hwts") {
            g_settings.hwts = (val != "0");
        } else if (key == "gm_enable") {
            g_settings.gm_enable = (val == "1");
        } else if (key == "gm_serial") {
            if (!val.empty())
                g_settings.gm_serial = val;
        } else if (key == "gm_pps") {
            if (!val.empty())
                g_settings.gm_pps = val;
        } else if (key == "gm_prio1") {
            int v = atoi(val.c_str());
            if (v >= 0 && v <= 255)
                g_settings.gm_prio1 = v;
        } else if (key == "gm_prio2") {
            int v = atoi(val.c_str());
            if (v >= 0 && v <= 255)
                g_settings.gm_prio2 = v;
        } else if (key == "gm_utc_offset") {
            int v = atoi(val.c_str());
            if (v >= 0 && v <= 99)
                g_settings.gm_utc_offset = v;
        } else if (key == "acceptable_gms") {
            g_settings.acceptable_gms = val;
        }
    }
    g_domain = g_settings.domain;

    // No clock list yet (fresh install or pre-list config): build one from
    // the classic mode/format/label settings so behavior stays the same.
    if (g_settings.clocks.empty()) {
        std::string style = g_settings.time_format == 12 ? "12h" : "24h";
        std::string local_zone = g_settings.timezone.empty()
            ? "UTC" : g_settings.timezone;
        auto zone_for = [&](int scale) -> std::string {
            if (scale == MODE_UTC) return "UTC";
            if (scale == MODE_TAI) return "TAI";
            return local_zone;
        };
        auto label_for = [&](int scale) -> std::string {
            if (scale == MODE_UTC)
                return g_settings.utc_label.empty() ? "UTC"
                                                    : g_settings.utc_label;
            if (scale == MODE_TAI)
                return g_settings.tai_label.empty() ? "TAI"
                                                    : g_settings.tai_label;
            return g_settings.tz_label.empty() ? "%Z" : g_settings.tz_label;
        };
        if (g_settings.mode == MODE_CYCLE) {
            for (int sc : {MODE_UTC, MODE_TAI, MODE_LOCAL})
                g_settings.clocks.push_back(
                    {zone_for(sc), style, label_for(sc)});
        } else {
            g_settings.clocks.push_back(
                {zone_for(g_settings.mode), style,
                 g_settings.show_zone ? label_for(g_settings.mode)
                                      : std::string()});
        }
    }
}

// ---- PTP processing ----
static void reset_ptp_state() {
    g_pending_sync.valid = false;
    g_pending_dreq.valid = false;
    g_have_pair = false;
    g_have_mpd = false;
    g_mpd_ns = 0;
    g_have_offset = false;
    have_ptp_ref = false;
    g_path_delay_ns = 0;
    g_last_sync_mono_ns = 0;
    g_last_sync_age_ns = 0;
    g_cmp_target_valid = false;
    g_cmp_have = false;
    g_cmp_count = 0;
    g_active_domain = -1;
    g_last_announce_mono_ns = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_have_gm = false;
    g_masters.clear();
    g_have_master = false;
}

// Domain filter. In auto mode (-1) the first Announce locks the domain.
static bool domain_ok(uint8_t pkt_domain, bool is_announce) {
    int cfg = g_domain.load();
    if (cfg >= 0)
        return pkt_domain == (uint8_t)cfg;
    int active = g_active_domain.load();
    if (active >= 0)
        return pkt_domain == (uint8_t)active;
    if (is_announce) {
        g_active_domain = pkt_domain;
        std::cout << "PTP domain " << (int)pkt_domain
                  << " detected automatically\n";
        return true;
    }
    return false;
}

// Domain to stamp into outgoing Delay_Req messages
static uint8_t effective_domain() {
    int cfg = g_domain.load();
    if (cfg >= 0)
        return (uint8_t)cfg;
    int active = g_active_domain.load();
    return active >= 0 ? (uint8_t)active : 0;
}

// Normalize a user-entered grandmaster identity to the canonical
// "aa:bb:cc:ff:fe:dd:ee:ff" form. Accepts any separator style and case;
// returns "" when the input does not contain exactly 16 hex digits.
static std::string normalize_gm_str(const std::string &in) {
    std::string hex;
    for (char c : in)
        if (isxdigit((unsigned char)c))
            hex += (char)tolower(c);
    if (hex.size() != 16)
        return "";
    std::string out;
    for (int i = 0; i < 8; ++i) {
        if (i)
            out += ":";
        out += hex.substr(i * 2, 2);
    }
    return out;
}

// True when the elected grandmaster is on the acceptable list — or when
// the list is empty/has no valid entries, or no grandmaster is elected.
// Call with g_mutex held.
static bool gm_acceptable_locked() {
    if (!g_have_gm)
        return true;
    std::string cur = format_gm(g_gm.id);
    bool any_valid = false;
    std::stringstream ss(g_settings.acceptable_gms);
    std::string item;
    while (std::getline(ss, item, ',')) {
        std::string n = normalize_gm_str(item);
        if (n.empty())
            continue;
        any_valid = true;
        if (n == cur)
            return true;
    }
    return !any_valid;
}

// BMCA dataset comparison (IEEE 1588-2008 9.3.4, single-port slave).
// Negative: a is better. Zero: tie (caller breaks it by sender identity).
static int compare_datasets(const GMInfo &a, const GMInfo &b) {
    if (memcmp(a.id, b.id, 8) != 0) {
        if (a.priority1 != b.priority1)
            return (int)a.priority1 - (int)b.priority1;
        if (a.clock_class != b.clock_class)
            return (int)a.clock_class - (int)b.clock_class;
        if (a.clock_accuracy != b.clock_accuracy)
            return (int)a.clock_accuracy - (int)b.clock_accuracy;
        if (a.variance != b.variance)
            return (int)a.variance - (int)b.variance;
        if (a.priority2 != b.priority2)
            return (int)a.priority2 - (int)b.priority2;
        return memcmp(a.id, b.id, 8);
    }
    // Same grandmaster via different paths: prefer fewer hops
    return (int)a.steps_removed - (int)b.steps_removed;
}

// Prune masters whose announces timed out, elect the best remaining one,
// and handle the bookkeeping when the election result changes.
// Call with g_mutex held, from the main thread only.
static void bmca_elect_locked(uint64_t now) {
    for (auto it = g_masters.begin(); it != g_masters.end();) {
        if (now - it->last_seen > it->timeout_ns)
            it = g_masters.erase(it);
        else
            ++it;
    }

    int best = -1;
    for (size_t i = 0; i < g_masters.size(); ++i) {
        if (best < 0) {
            best = (int)i;
            continue;
        }
        int c = compare_datasets(g_masters[i].gm, g_masters[best].gm);
        if (c < 0 || (c == 0 && memcmp(g_masters[i].sender,
                                       g_masters[best].sender, 10) < 0))
            best = (int)i;
    }
    for (size_t i = 0; i < g_masters.size(); ++i)
        g_masters[i].elected = ((int)i == best);

    if (best < 0) {
        if (g_have_master) {
            g_have_master = false;
            g_have_gm = false;
            g_pending_sync.valid = false;
            std::cout << "PTP master lost (announce timeout)\n";
        }
        return;
    }

    const ForeignMaster &m = g_masters[best];
    if (g_have_master && memcmp(g_elected_sender, m.sender, 10) == 0)
        return;                           // no change

    bool gm_changed = g_have_gm && memcmp(g_gm.id, m.gm.id, 8) != 0;
    memcpy(g_elected_sender, m.sender, 10);
    g_have_master = true;
    if (gm_changed) {
        std::string old_gm = format_gm(g_gm.id);
        g_gm_changes++;
        clock_gettime(CLOCK_MONOTONIC_RAW, &g_last_gm_change);
        std::cout << "Grandmaster change: " << old_gm
                  << " -> " << format_gm(m.gm.id) << "\n";
    } else if (!g_have_gm) {
        std::cout << "Grandmaster: " << format_gm(m.gm.id) << "\n";
    } else {
        std::cout << "PTP parent changed (same grandmaster)\n";
    }
    g_gm = m.gm;
    g_have_gm = true;
    if (!gm_acceptable_locked())
        std::cout << "WARNING: grandmaster " << format_gm(m.gm.id)
                  << " is not on the acceptable list!\n";

    // New sync path: restart pending state and re-measure the path delay
    g_pending_sync.valid = false;
    g_pending_dreq.valid = false;
    g_have_mpd = false;
    g_path_delay_ns = 0;
}

// A (t1, t2) pair is complete: update the offset estimate.
// local = master + offset  =>  offset = t2 - t1 - meanPathDelay
// wire=false marks a GNSS sample (grandmaster mode). While GNSS drives
// the clock, wire pairs are not used for discipline — they are measured
// AGAINST the GNSS reference instead (positive = network master ahead).
static void complete_sync_pair(int64_t t1, int64_t t2, bool wire = true) {
    g_last_t1 = t1;
    g_last_t2 = t2;
    g_have_pair = true;

    // A GNSS sample has no network path, so no mean path delay applies
    int64_t sample = t2 - t1 - ((wire && g_have_mpd) ? g_mpd_ns : 0);
    if (wire && g_role.load() >= ROLE_GM_PASSIVE) {
        if (!g_have_offset)
            return;                       // no GNSS reference yet
        int64_t d = g_offset_ns - sample; // wire master - GNSS, ns
        if (!g_cmp_have) {
            g_cmp_ns = d;
            g_cmp_have = true;
        } else {
            g_cmp_ns += (d - g_cmp_ns) / 8;
        }
        g_cmp_atomic = g_cmp_ns;
        g_cmp_last = d;
        g_cmp_count++;
        std::lock_guard<std::mutex> lock(g_mutex);
        hist_push(g_hist_cmp, g_hist_cmp_n, g_hist_cmp_i, d);
        return;
    }

    // Analysis history: how far this sync was off the smoothed estimate
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        hist_push(g_hist_off, g_hist_off_n, g_hist_off_i,
                  g_have_offset ? sample - g_offset_ns : 0);
    }
    if (!g_have_offset || llabs(sample - g_offset_ns) > 100000000LL) {
        g_offset_ns = sample;             // first sample or step: jump
        g_have_offset = true;
    } else {
        g_offset_ns += (sample - g_offset_ns) / 8;
    }
    g_offset_atomic = g_offset_ns;
    g_last_sync_mono_ns = (uint64_t)t2;
    g_last_sync_age_ns = mono_ns();       // housekeeping clock, not t2:
    have_ptp_ref = true;                  // t2 may be PHC time (hw mode)
}

// Port 319 (event): Sync
static void process_event_packet(const uint8_t *buf, ssize_t len,
                                 uint64_t now_mono) {
    if (len < 44 || (buf[1] & 0x0F) != 2 || !domain_ok(buf[4], false))
        return;
    uint8_t msg_type = buf[0] & 0x0F;
    if (msg_type != 0x00)                 // only Sync
        return;
    g_cnt_sync++;

    // Hardware mode: a Sync without a hardware timestamp (arrived on a
    // non-PHC interface) is useless for timing
    if (now_mono == 0)
        return;

    // GNSS-driven (grandmaster/passive): Syncs from the comparison
    // target are measured against GNSS, everything else is ignored.
    // Otherwise BMCA rules: only the elected master disciplines us.
    if (g_role.load() >= ROLE_GM_PASSIVE) {
        if (!g_cmp_target_valid.load() ||
            memcmp(buf + 20, g_cmp_target, 10) != 0)
            return;
    } else if (!g_have_master ||
               memcmp(buf + 20, g_elected_sender, 10) != 0) {
        return;
    }

    uint16_t seq = (buf[30] << 8) | buf[31];
    bool two_step = buf[6] & 0x02;
    int64_t corr = corr_to_ns(buf + 8);

    if (two_step) {
        g_pending_sync.valid = true;
        g_pending_sync.seq = seq;
        memcpy(g_pending_sync.src, buf + 20, 10);
        g_pending_sync.t2_mono = now_mono;
        g_pending_sync.corr_ns = corr;
    } else {
        complete_sync_pair((int64_t)ts_to_ns(buf + 34) + corr,
                           (int64_t)now_mono);
    }
}

// Port 320 (general): Announce, Follow_Up, Delay_Resp
static void process_general_packet(const uint8_t *buf, ssize_t len,
                                   uint64_t now_mono) {
    if (len < 44 || (buf[1] & 0x0F) != 2)
        return;
    uint8_t msg_type = buf[0] & 0x0F;
    if (!domain_ok(buf[4], msg_type == 0x0B))
        return;

    // --- Announce (0x0B): feed the BMCA master list ---
    if (msg_type == 0x0B && len >= 64) {
        g_cnt_ann++;
        g_last_announce_mono_ns = now_mono;

        GMInfo gm;
        memcpy(gm.id, buf + 53, 8);
        gm.priority1 = buf[47];
        gm.clock_class = buf[48];
        gm.clock_accuracy = buf[49];
        gm.variance = (buf[50] << 8) | buf[51];
        gm.priority2 = buf[52];
        gm.steps_removed = (buf[61] << 8) | buf[62];
        gm.time_source = buf[63];

        // Announce receipt timeout: 3 x announce interval (from the
        // logMessageInterval field), clamped to [1 s, 30 s]
        int8_t log_itv = (int8_t)buf[33];
        if (log_itv < -4) log_itv = -4;
        if (log_itv > 6) log_itv = 6;
        uint64_t timeout = (uint64_t)(3e9 * ldexp(1.0, log_itv));
        if (timeout < 1000000000ULL) timeout = 1000000000ULL;
        if (timeout > 30000000000ULL) timeout = 30000000000ULL;

        std::lock_guard<std::mutex> lock(g_mutex);
        ForeignMaster *fm = nullptr;
        for (auto &m : g_masters) {
            if (memcmp(m.sender, buf + 20, 10) == 0) {
                fm = &m;
                break;
            }
        }
        if (!fm) {
            g_masters.push_back(ForeignMaster{});
            fm = &g_masters.back();
            memcpy(fm->sender, buf + 20, 10);
        }
        fm->gm = gm;
        fm->last_seen = now_mono;
        fm->timeout_ns = timeout;

        bmca_elect_locked(now_mono);

        // Data from the elected master only
        if (g_have_master && memcmp(g_elected_sender, buf + 20, 10) == 0) {
            g_gm = gm;
            uint16_t flags = (buf[6] << 8) | buf[7];
            if ((flags & 0x0008) && (flags & 0x0004))
                current_utc_offset = (int16_t)((buf[44] << 8) | buf[45]);
        }
    }
    // --- Follow_Up (0x08): precise origin timestamp (t1) ---
    else if (msg_type == 0x08) {
        g_cnt_fup++;
        uint16_t seq = (buf[30] << 8) | buf[31];
        if (g_pending_sync.valid && g_pending_sync.seq == seq &&
            memcmp(g_pending_sync.src, buf + 20, 10) == 0) {
            int64_t t1 = (int64_t)ts_to_ns(buf + 34) +
                         g_pending_sync.corr_ns + corr_to_ns(buf + 8);
            complete_sync_pair(t1, (int64_t)g_pending_sync.t2_mono);
            g_pending_sync.valid = false;
        }
    }
    // --- Delay_Resp (0x09): t4 for our Delay_Req ---
    else if (msg_type == 0x09 && len >= 54) {
        g_cnt_dresp++;
        if (!g_pending_dreq.valid || !g_have_pair)
            return;
        if (memcmp(buf + 44, g_clock_id, 8) != 0 ||
            buf[52] != 0 || buf[53] != 1)
            return;                       // not addressed to us
        uint16_t seq = (buf[30] << 8) | buf[31];
        if (seq != g_pending_dreq.seq)
            return;

        int64_t t4 = (int64_t)ts_to_ns(buf + 34);
        int64_t t3 = (int64_t)g_pending_dreq.t3_mono;
        g_pending_dreq.valid = false;

        // meanPathDelay = ((t2 - t1) + (t4 - t3)) / 2 - correction
        int64_t sample = ((g_last_t2 - g_last_t1) + (t4 - t3)) / 2
                         - corr_to_ns(buf + 8);
        if (sample < 0)
            sample = 0;
        if (sample > 1000000000LL)        // > 1 s: bogus, discard
            return;
        if (!g_have_mpd) {
            g_mpd_ns = sample;
            g_have_mpd = true;
        } else {
            g_mpd_ns += (sample - g_mpd_ns) / 8;
        }
        g_path_delay_ns = g_mpd_ns;
        g_dresp_received++;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            hist_push(g_hist_del, g_hist_del_n, g_hist_del_i, sample);
        }
    }
}

// Build a 44-byte Delay_Req message
static void build_delay_req(uint8_t *buf, uint16_t seq) {
    memset(buf, 0, 44);
    buf[0] = 0x01;                        // messageType Delay_Req
    buf[1] = 0x02;                        // versionPTP 2
    buf[2] = 0; buf[3] = 44;              // messageLength
    buf[4] = effective_domain();          // domainNumber
    memcpy(buf + 20, g_clock_id, 8);      // sourcePortIdentity.clockIdentity
    buf[28] = 0; buf[29] = 1;             // sourcePortIdentity.portNumber
    buf[30] = seq >> 8; buf[31] = seq & 0xFF;
    buf[32] = 1;                          // controlField Delay_Req
    buf[33] = 0x7F;                       // logMessageInterval
    // originTimestamp (34..43) may be zero
}

// ---- PTP master TX (grandmaster mode) ----
static void ptp_master_header(uint8_t *buf, uint8_t type, uint16_t len,
                              uint16_t flags, uint16_t seq, uint8_t control,
                              int8_t log_itv) {
    memset(buf, 0, len);
    buf[0] = type;
    buf[1] = 0x02;                        // versionPTP 2
    buf[2] = (uint8_t)(len >> 8);
    buf[3] = (uint8_t)(len & 0xFF);
    buf[4] = effective_domain();
    buf[6] = (uint8_t)(flags >> 8);
    buf[7] = (uint8_t)(flags & 0xFF);
    memcpy(buf + 20, g_clock_id, 8);
    buf[28] = 0; buf[29] = 1;             // portNumber 1
    buf[30] = (uint8_t)(seq >> 8);
    buf[31] = (uint8_t)(seq & 0xFF);
    buf[32] = control;
    buf[33] = (uint8_t)log_itv;
}

static void put_ptp_ts(uint8_t *p, int64_t tai_ns) {
    uint64_t s = (uint64_t)(tai_ns / 1000000000LL);
    uint32_t n = (uint32_t)(tai_ns % 1000000000LL);
    for (int i = 0; i < 6; ++i)
        p[i] = (uint8_t)(s >> (8 * (5 - i)));
    for (int i = 0; i < 4; ++i)
        p[6 + i] = (uint8_t)(n >> (8 * (3 - i)));
}

// Announce: our grandmaster dataset (same offsets the client parses)
static void build_announce(uint8_t *buf, uint16_t seq, const GMInfo &self,
                           int16_t utc_off, bool traceable) {
    uint16_t flags = 0x0008 | 0x0004;     // ptpTimescale + utcOffsetValid
    if (traceable)
        flags |= 0x0010 | 0x0020;         // time + frequency traceable
    ptp_master_header(buf, 0x0B, 64, flags, seq, 5, 0);
    buf[44] = (uint8_t)(utc_off >> 8);
    buf[45] = (uint8_t)(utc_off & 0xFF);
    buf[47] = self.priority1;
    buf[48] = self.clock_class;
    buf[49] = self.clock_accuracy;
    buf[50] = (uint8_t)(self.variance >> 8);
    buf[51] = (uint8_t)(self.variance & 0xFF);
    buf[52] = self.priority2;
    memcpy(buf + 53, self.id, 8);
    buf[61] = 0; buf[62] = 0;             // stepsRemoved
    buf[63] = self.time_source;
}

static void build_sync(uint8_t *buf, uint16_t seq, int64_t approx_tai_ns) {
    ptp_master_header(buf, 0x00, 44, 0x0200, seq, 0, 0);  // two-step
    put_ptp_ts(buf + 34, approx_tai_ns);
}

static void build_follow_up(uint8_t *buf, uint16_t seq, int64_t t1_tai_ns) {
    ptp_master_header(buf, 0x08, 44, 0, seq, 2, 0);
    put_ptp_ts(buf + 34, t1_tai_ns);
}

// Delay_Resp for a received Delay_Req (echoes seq + correctionField)
static void build_delay_resp(uint8_t *buf, const uint8_t *req,
                             int64_t t4_tai_ns) {
    ptp_master_header(buf, 0x09, 54, 0, 0, 3, 0);
    memcpy(buf + 8, req + 8, 8);          // correctionField
    memcpy(buf + 30, req + 30, 2);        // sequenceId
    put_ptp_ts(buf + 34, t4_tai_ns);
    memcpy(buf + 44, req + 20, 10);       // requestingPortIdentity
}

// ---- GNSS receiver (NMEA + PPS) for the grandmaster mode ----

static bool nmea_checksum_ok(const char *line) {
    if (line[0] != '$')
        return false;
    uint8_t cs = 0;
    const char *p = line + 1;
    while (*p && *p != '*')
        cs ^= (uint8_t)*p++;
    if (*p != '*' || !isxdigit((unsigned char)p[1]) ||
        !isxdigit((unsigned char)p[2]))
        return false;
    return cs == (uint8_t)strtol(p + 1, nullptr, 16);
}

// Parse one NMEA sentence. GGA/GSV update the GNSS status; a valid RMC
// fix sets *utc_out (UTC seconds since the epoch) and returns true.
static bool nmea_parse_line(const char *line, time_t *utc_out) {
    if (!nmea_checksum_ok(line))
        return false;
    const char *star = strchr(line, '*');
    std::string body(line, star ? (size_t)(star - line) : strlen(line));
    std::vector<std::string> f;
    std::stringstream ss(body);
    std::string item;
    while (std::getline(ss, item, ','))
        f.push_back(item);
    if (f.empty() || f[0].size() < 6)
        return false;
    std::string talker = f[0].substr(1, 2);
    std::string type = f[0].substr(3);

    if (type == "GGA" && f.size() > 8) {
        g_gnss_fixq = atoi(f[6].c_str());
        g_gnss_sats_used = f[7].empty() ? -1 : atoi(f[7].c_str());
        g_gnss_hdop10 = f[8].empty() ? -1 : (int)(atof(f[8].c_str()) * 10);
    } else if (type == "GSV" && f.size() > 3) {
        uint64_t now = mono_ns();
        std::lock_guard<std::mutex> lk(g_gnss_mutex);
        for (size_t i = 4; i + 3 < f.size(); i += 4) {
            if (f[i].empty())
                continue;
            int prn = atoi(f[i].c_str());
            int snr = f[i + 3].empty() ? -1 : atoi(f[i + 3].c_str());
            bool found = false;
            for (auto &s : g_gnss_sats) {
                if (s.prn == prn && talker == s.talker) {
                    s.snr = snr;
                    s.seen = now;
                    found = true;
                    break;
                }
            }
            if (!found) {
                SatInfo si{};
                snprintf(si.talker, sizeof(si.talker), "%s", talker.c_str());
                si.prn = prn;
                si.snr = snr;
                si.seen = now;
                g_gnss_sats.push_back(si);
            }
        }
        g_gnss_sats.erase(
            std::remove_if(g_gnss_sats.begin(), g_gnss_sats.end(),
                           [&](const SatInfo &s) {
                               return now - s.seen > 15000000000ULL;
                           }),
            g_gnss_sats.end());
        g_gnss_sats_view = (int)g_gnss_sats.size();
    } else if (type == "RMC" && f.size() > 9) {
        if (f[2] != "A" || f[1].size() < 6 || f[9].size() < 6)
            return false;
        struct tm tmv{};
        tmv.tm_hour = (f[1][0] - '0') * 10 + (f[1][1] - '0');
        tmv.tm_min = (f[1][2] - '0') * 10 + (f[1][3] - '0');
        tmv.tm_sec = (f[1][4] - '0') * 10 + (f[1][5] - '0');
        tmv.tm_mday = (f[9][0] - '0') * 10 + (f[9][1] - '0');
        tmv.tm_mon = (f[9][2] - '0') * 10 + (f[9][3] - '0') - 1;
        tmv.tm_year = (f[9][4] - '0') * 10 + (f[9][5] - '0') + 100;
        time_t t = timegm(&tmv);
        if (t <= 0)
            return false;
        if (f[1].size() > 6 && atof(f[1].c_str() + 6) >= 0.5)
            t += 1;                       // fix epoch in the 2nd half
        *utc_out = t;
        return true;
    }
    return false;
}

static int gnss_open_serial(const char *dev) {
    int fd = open(dev, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    struct termios tio{};
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        tio.c_cflag |= CLOCAL | CREAD;
        cfsetispeed(&tio, B9600);
        cfsetospeed(&tio, B9600);
        tcsetattr(fd, TCSANOW, &tio);
    }
    return fd;
}

#ifdef __linux__
static int gnss_open_pps(const char *dev) {
    int fd = open(dev, O_RDWR);
    if (fd < 0)
        return -1;
    struct pps_kparams params{};
    if (ioctl(fd, PPS_GETPARAMS, &params) == 0) {
        params.mode |= PPS_CAPTUREASSERT;
        ioctl(fd, PPS_SETPARAMS, &params);
    }
    return fd;
}
#endif

// Open the GNSS devices while still privileged (called from main before
// the matrix library drops to the daemon user)
static void gnss_open_initial() {
    int s = gnss_open_serial(g_settings.gm_serial.c_str());
    g_gnss_serial_fd = s;
    if (s >= 0)
        std::cout << "GNSS: serial " << g_settings.gm_serial << " open\n";
    else
        std::cerr << "GNSS: cannot open serial " << g_settings.gm_serial
                  << "\n";
#ifdef __linux__
    int p = gnss_open_pps(g_settings.gm_pps.c_str());
    g_gnss_pps_fd = p;
    if (p >= 0)
        std::cout << "GNSS: PPS " << g_settings.gm_pps << " open\n";
    else
        std::cerr << "GNSS: cannot open PPS " << g_settings.gm_pps << "\n";
#endif
}

// GNSS thread: pairs each PPS pulse (the exact top of second) with the
// following RMC sentence (which second it was) and hands (TAI, local)
// samples to the main loop. Reopens devices as needed — note that after
// the matrix privilege drop reopening may need the udev rule/group set
// up by install.sh.
static void gnss_thread() {
    char acc[1024];
    size_t fill = 0;
    uint32_t last_seq = 0;
    bool have_seq = false;
    uint64_t pulse_local = 0, pulse_mono = 0;
    uint64_t next_reopen = 0;
    std::string ser_dev, pps_dev;

    for (;;) {
        bool enabled;
        std::string cfg_ser, cfg_pps;
        int utc_off;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            enabled = g_settings.gm_enable;
            cfg_ser = g_settings.gm_serial;
            cfg_pps = g_settings.gm_pps;
            utc_off = g_settings.gm_utc_offset;
        }
        if (!enabled) {
            usleep(500000);
            continue;
        }
        if (ser_dev.empty())
            ser_dev = cfg_ser;
        if (pps_dev.empty())
            pps_dev = cfg_pps;

        int sfd = g_gnss_serial_fd.load();
        int pfd = g_gnss_pps_fd.load();
        if (cfg_ser != ser_dev) {         // device changed in the web UI
            if (sfd >= 0)
                close(sfd);
            g_gnss_serial_fd = sfd = -1;
            ser_dev = cfg_ser;
        }
        if (cfg_pps != pps_dev) {
            if (pfd >= 0)
                close(pfd);
            g_gnss_pps_fd = pfd = -1;
            pps_dev = cfg_pps;
        }
        uint64_t now = mono_ns();
        if ((sfd < 0 || pfd < 0) && now >= next_reopen) {
            next_reopen = now + 5000000000ULL;
            if (sfd < 0) {
                sfd = gnss_open_serial(ser_dev.c_str());
                g_gnss_serial_fd = sfd;
            }
#ifdef __linux__
            if (pfd < 0) {
                pfd = gnss_open_pps(pps_dev.c_str());
                g_gnss_pps_fd = pfd;
            }
#endif
        }
        g_gnss_serial_ok = sfd >= 0;
        g_gnss_pps_ok = pfd >= 0;

        // --- PPS pulse (blocks up to 200 ms) ---
#ifdef __linux__
        if (pfd >= 0) {
            struct pps_fdata fdata{};
            fdata.timeout.sec = 0;
            fdata.timeout.nsec = 200000000;
            fdata.timeout.flags = ~PPS_TIME_INVALID;
            if (ioctl(pfd, PPS_FETCH, &fdata) == 0) {
                if (!have_seq || fdata.info.assert_sequence != last_seq) {
                    have_seq = true;
                    last_seq = fdata.info.assert_sequence;
                    // The pulse is stamped with CLOCK_REALTIME; translate
                    // it into the local timing clock
                    timespec rt;
                    clock_gettime(CLOCK_REALTIME, &rt);
                    uint64_t loc = local_clock_ns();
                    int64_t rt_now =
                        (int64_t)rt.tv_sec * 1000000000LL + rt.tv_nsec;
                    int64_t pulse_rt =
                        (int64_t)fdata.info.assert_tu.sec * 1000000000LL +
                        fdata.info.assert_tu.nsec;
                    pulse_local =
                        (uint64_t)((int64_t)loc - (rt_now - pulse_rt));
                    pulse_mono = mono_ns();
                    g_gnss_last_pps = pulse_mono;
                }
            } else if (errno != ETIMEDOUT && errno != EINTR) {
                close(pfd);
                g_gnss_pps_fd = -1;
            }
        } else
#endif
        {
            usleep(200000);
        }

        // --- NMEA sentences ---
        if (sfd >= 0) {
            char rb[256];
            ssize_t r;
            while ((r = read(sfd, rb, sizeof(rb))) > 0) {
                for (ssize_t i = 0; i < r; ++i) {
                    char c = rb[i];
                    if (c != '\n' && fill < sizeof(acc) - 1) {
                        acc[fill++] = c;
                        continue;
                    }
                    acc[fill] = 0;
                    while (fill && acc[fill - 1] == '\r')
                        acc[--fill] = 0;
                    if (fill) {
                        time_t utc = 0;
                        if (nmea_parse_line(acc, &utc)) {
                            uint64_t n2 = mono_ns();
                            // The RMC names the second of the pulse that
                            // just preceded it
                            if (pulse_mono &&
                                n2 - pulse_mono < 900000000ULL) {
                                std::lock_guard<std::mutex> lk(g_gnss_mutex);
                                g_gnss_t1 =
                                    ((int64_t)utc + utc_off) * 1000000000LL;
                                g_gnss_t2 = (int64_t)pulse_local;
                                g_gnss_sample_valid = true;
                                g_gnss_last_sample = n2;
                                g_gnss_sample_count++;
                            }
                        }
                    }
                    fill = 0;
                }
            }
            if (r == 0) {                 // EOF: USB receiver unplugged
                close(sfd);
                g_gnss_serial_fd = -1;
            } else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                       errno != EINTR) {
                close(sfd);
                g_gnss_serial_fd = -1;
            }
        }
        g_gnss_lock = g_gnss_last_sample.load() != 0 &&
                      mono_ns() - g_gnss_last_sample.load() < 10000000000ULL;
    }
}

// ---- Network interface helpers ----
static uint32_t get_iface_ip(const std::string &name) {
    struct ifaddrs *ifaddr, *ifa;
    uint32_t ip = 0;

    if (getifaddrs(&ifaddr) == -1)
        return 0;

    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr)
            continue;
        if (ifa->ifa_addr->sa_family == AF_INET &&
            name == ifa->ifa_name) {
            ip = ((struct sockaddr_in*)ifa->ifa_addr)->sin_addr.s_addr;
            break;
        }
    }

    freeifaddrs(ifaddr);
    return ip;
}

// All interfaces with an IPv4 address (except loopback), for the web UI
static std::vector<std::string> list_ifaces() {
    std::vector<std::string> out;
    struct ifaddrs *ifaddr, *ifa;

    if (getifaddrs(&ifaddr) == -1)
        return out;

    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (ifa->ifa_flags & IFF_LOOPBACK)
            continue;
        std::string name = ifa->ifa_name;
        bool dup = false;
        for (const auto &n : out)
            if (n == name)
                dup = true;
        if (!dup)
            out.push_back(name);
    }

    freeifaddrs(ifaddr);
    return out;
}

// Our clockIdentity: EUI-64 derived from the interface MAC address
static void init_clock_identity(int sock, const std::string &iface) {
#ifdef __linux__
    struct ifreq ifr{};
    strncpy(ifr.ifr_name, iface.c_str(), sizeof(ifr.ifr_name) - 1);
    if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
        const uint8_t *mac = (const uint8_t*)ifr.ifr_hwaddr.sa_data;
        g_clock_id[0] = mac[0]; g_clock_id[1] = mac[1]; g_clock_id[2] = mac[2];
        g_clock_id[3] = 0xFF;   g_clock_id[4] = 0xFE;
        g_clock_id[5] = mac[3]; g_clock_id[6] = mac[4]; g_clock_id[7] = mac[5];
        return;
    }
#else
    (void)sock; (void)iface;
#endif
    // Fallback: locally administered pseudo identity
    uint32_t pid = (uint32_t)getpid();
    g_clock_id[0] = 0x02; g_clock_id[1] = 0x00; g_clock_id[2] = 0x00;
    g_clock_id[3] = 0xFF; g_clock_id[4] = 0xFE;
    g_clock_id[5] = (pid >> 16) & 0xFF;
    g_clock_id[6] = (pid >> 8) & 0xFF;
    g_clock_id[7] = pid & 0xFF;
}

// Probe the interface(s) for PTP hardware timestamping and enable it if
// available. Must run while still root (SIOCSHWTSTAMP and /dev/ptpN need
// privileges; the socket option and the kept-open PHC fd survive the
// later privilege drop). Falls back to software timestamps silently.
static void try_enable_hw_timestamping(int sock_event) {
#ifdef __linux__
    std::vector<std::string> cand;
    if (g_settings.iface == "auto")
        cand = list_ifaces();
    else
        cand.push_back(g_settings.iface);
    for (const auto &name : cand) {
        struct ifreq ifr{};
        strncpy(ifr.ifr_name, name.c_str(), sizeof(ifr.ifr_name) - 1);
        struct ethtool_ts_info info{};
        info.cmd = ETHTOOL_GET_TS_INFO;
        ifr.ifr_data = (char *)&info;
        if (ioctl(sock_event, SIOCETHTOOL, &ifr) < 0)
            continue;
        unsigned need = SOF_TIMESTAMPING_RX_HARDWARE |
                        SOF_TIMESTAMPING_TX_HARDWARE |
                        SOF_TIMESTAMPING_RAW_HARDWARE;
        if ((info.so_timestamping & need) != need || info.phc_index < 0)
            continue;

        // Turn on timestamping in the NIC; filter support varies
        static const int kFilters[] = {HWTSTAMP_FILTER_ALL,
                                       HWTSTAMP_FILTER_PTP_V2_L4_EVENT,
                                       HWTSTAMP_FILTER_PTP_V2_EVENT};
        struct hwtstamp_config cfg{};
        cfg.tx_type = HWTSTAMP_TX_ON;
        bool nic_ok = false;
        for (int filt : kFilters) {
            cfg.rx_filter = filt;
            struct ifreq ifr2{};
            strncpy(ifr2.ifr_name, name.c_str(), sizeof(ifr2.ifr_name) - 1);
            ifr2.ifr_data = (char *)&cfg;
            if (ioctl(sock_event, SIOCSHWTSTAMP, &ifr2) == 0) {
                nic_ok = true;
                break;
            }
        }
        if (!nic_ok)
            continue;

        char path[32];
        snprintf(path, sizeof(path), "/dev/ptp%d", info.phc_index);
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            std::cerr << "Hardware timestamping: cannot open " << path
                      << ", staying on software timestamps\n";
            continue;
        }

        int flags = SOF_TIMESTAMPING_RX_HARDWARE |
                    SOF_TIMESTAMPING_RAW_HARDWARE |
                    SOF_TIMESTAMPING_TX_HARDWARE |
                    SOF_TIMESTAMPING_OPT_TSONLY;
        if (setsockopt(sock_event, SOL_SOCKET, SO_TIMESTAMPING,
                       &flags, sizeof(flags)) < 0) {
            close(fd);
            continue;
        }

        g_phc_fd = fd;
        g_hwts_desc = name + " via " + path;
        g_hwts = true;
        std::cout << "PTP hardware timestamping enabled on " << g_hwts_desc
                  << "\n";
        return;
    }
    std::cout << "PTP hardware timestamping not available, "
              << "using software timestamps\n";
#else
    (void)sock_event;
#endif
}

// Receive one packet from the event socket and return the local timestamp
// to pair with it: the hardware (PHC) RX timestamp in hw mode, else
// CLOCK_MONOTONIC_RAW. Returns 0 when no usable timestamp exists (hw mode
// and the packet arrived on a different interface).
static uint64_t recv_event_packet(int sock, uint8_t *buf, size_t buflen,
                                  ssize_t *len_out) {
#ifdef __linux__
    struct iovec iov{buf, buflen};
    union { char buf[256]; struct cmsghdr align; } ctrl;
    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl.buf;
    msg.msg_controllen = sizeof(ctrl.buf);
    ssize_t len = recvmsg(sock, &msg, 0);
    *len_out = len;
    if (len <= 0)
        return 0;
    if (!g_hwts)
        return mono_ns();
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c;
         c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPING) {
            struct scm_timestamping tss;
            memcpy(&tss, CMSG_DATA(c), sizeof(tss));
            if (tss.ts[2].tv_sec || tss.ts[2].tv_nsec)   // [2] = raw hardware
                return (uint64_t)tss.ts[2].tv_sec * 1000000000ULL +
                       tss.ts[2].tv_nsec;
        }
    }
    return 0;
#else
    ssize_t len = recv(sock, buf, buflen, 0);
    *len_out = len;
    return len > 0 ? mono_ns() : 0;
#endif
}

#ifdef __linux__
// Discard leftover error-queue entries (a TX timestamp that arrived
// after its fetch window would otherwise be taken as the NEXT Delay_Req's
// send time — about a second off)
static void drain_errqueue(int sock) {
    union { char buf[256]; struct cmsghdr align; } ctrl;
    char dbuf[64];
    struct iovec iov{dbuf, sizeof(dbuf)};
    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    do {
        msg.msg_control = ctrl.buf;
        msg.msg_controllen = sizeof(ctrl.buf);
    } while (recvmsg(sock, &msg, MSG_ERRQUEUE) >= 0);
}

// Hardware TX timestamp of the just-sent Delay_Req, looped back by the
// kernel on the socket's error queue. Usually there within microseconds;
// give up after ~4 ms (that delay round is then skipped).
static uint64_t fetch_tx_timestamp(int sock) {
    for (int tries = 0; tries < 20; ++tries) {
        union { char buf[256]; struct cmsghdr align; } ctrl;
        char dbuf[64];
        struct iovec iov{dbuf, sizeof(dbuf)};
        struct msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl.buf;
        msg.msg_controllen = sizeof(ctrl.buf);
        ssize_t r = recvmsg(sock, &msg, MSG_ERRQUEUE);
        if (r < 0) {
            usleep(200);
            continue;
        }
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c;
             c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET &&
                c->cmsg_type == SCM_TIMESTAMPING) {
                struct scm_timestamping tss;
                memcpy(&tss, CMSG_DATA(c), sizeof(tss));
                if (tss.ts[2].tv_sec || tss.ts[2].tv_nsec)
                    return (uint64_t)tss.ts[2].tv_sec * 1000000000ULL +
                           tss.ts[2].tv_nsec;
            }
        }
    }
    return 0;
}
#endif

// Add/drop the PTP multicast membership on one interface (both sockets).
// op is IP_ADD_MEMBERSHIP or IP_DROP_MEMBERSHIP.
static void membership(int sock_event, int sock_gen, uint32_t ip, int op) {
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr("224.0.1.129");
    mreq.imr_interface.s_addr = ip;
    setsockopt(sock_event, IPPROTO_IP, op, &mreq, sizeof(mreq));
    setsockopt(sock_gen, IPPROTO_IP, op, &mreq, sizeof(mreq));
}

// ---- Minimal HTTP server ----
static std::string url_decode(const std::string &in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size()) {
            int hi = isdigit(in[i+1]) ? in[i+1]-'0' : (tolower(in[i+1])-'a'+10);
            int lo = isdigit(in[i+2]) ? in[i+2]-'0' : (tolower(in[i+2])-'a'+10);
            out += (char)((hi << 4) | lo);
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}

static std::map<std::string, std::string> parse_form(const std::string &body) {
    std::map<std::string, std::string> kv;
    std::stringstream ss(body);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        size_t eq = pair.find('=');
        if (eq == std::string::npos)
            continue;
        kv[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
    }
    return kv;
}

static std::string json_escape(const std::string &in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

static std::string settings_json() {
    std::lock_guard<std::mutex> lock(g_mutex);
    char color[8];
    snprintf(color, sizeof(color), "#%02x%02x%02x",
             g_settings.r, g_settings.g, g_settings.b);
    std::ostringstream j;
    j << "{\"color\":\"" << color << "\","
      << "\"brightness\":" << g_settings.brightness << ","
      << "\"blackout\":" << (g_settings.blackout ? "true" : "false") << ","
      << "\"rotate180\":" << (g_settings.rotate180 ? "true" : "false") << ","
      << "\"show_gm\":" << (g_settings.show_gm ? "true" : "false") << ","
      << "\"show_gm_details\":" << (g_settings.show_gm_details ? "true" : "false") << ","
      << "\"show_date\":" << (g_settings.show_date ? "true" : "false") << ","
      << "\"mode\":\"" << (g_settings.mode == MODE_TAI ? "tai" :
                           g_settings.mode == MODE_LOCAL ? "local" :
                           g_settings.mode == MODE_CYCLE ? "cycle" : "utc") << "\","
      << "\"timezone\":\"" << json_escape(g_settings.timezone) << "\","
      << "\"tz_label\":\"" << json_escape(g_settings.tz_label) << "\","
      << "\"utc_label\":\"" << json_escape(g_settings.utc_label) << "\","
      << "\"tai_label\":\"" << json_escape(g_settings.tai_label) << "\","
      << "\"show_zone\":" << (g_settings.show_zone ? "true" : "false") << ","
      << "\"clocks\":[";
    for (size_t i = 0; i < g_settings.clocks.size(); ++i) {
        const ClockEntry &e = g_settings.clocks[i];
        j << (i ? "," : "") << "{\"zone\":\"" << json_escape(e.zone)
          << "\",\"style\":\"" << e.style << "\",\"name\":\""
          << json_escape(e.name) << "\"}";
    }
    j << "],"
      << "\"time_format\":\"" << g_settings.time_format << "\","
      << "\"date_format\":\"" << (g_settings.date_format == DATE_ISO ? "iso" :
                                  g_settings.date_format == DATE_MDY ? "mdy" :
                                  "dmy") << "\","
      << "\"notify_gm_change\":" << (g_settings.notify_gm_change ? "true" : "false") << ","
      << "\"domain\":" << g_settings.domain << ","
      << "\"acceptable_gms\":\"" << json_escape(g_settings.acceptable_gms) << "\","
      << "\"gm_enable\":" << (g_settings.gm_enable ? "true" : "false") << ","
      << "\"gm_serial\":\"" << json_escape(g_settings.gm_serial) << "\","
      << "\"gm_pps\":\"" << json_escape(g_settings.gm_pps) << "\","
      << "\"gm_prio1\":" << g_settings.gm_prio1 << ","
      << "\"gm_prio2\":" << g_settings.gm_prio2 << ","
      << "\"gm_utc_offset\":" << g_settings.gm_utc_offset << ","
      << "\"iface\":\"" << json_escape(g_settings.iface) << "\","
      << "\"ifaces\":[";
    std::vector<std::string> ifs = list_ifaces();
    for (size_t i = 0; i < ifs.size(); ++i)
        j << (i ? "," : "") << "\"" << json_escape(ifs[i]) << "\"";
    j << "]}";
    return j.str();
}

// Analysis history for the settings-page charts (oldest first)
static std::string history_json() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::ostringstream j;
    auto ring = [&](const int32_t *buf, int n, int i) {
        for (int k = 0; k < n; ++k) {
            int idx = (i - n + k + 2 * kHistN) % kHistN;
            j << (k ? "," : "") << buf[idx];
        }
    };
    j << "{\"offset\":[";
    ring(g_hist_off, g_hist_off_n, g_hist_off_i);
    j << "],\"delay\":[";
    ring(g_hist_del, g_hist_del_n, g_hist_del_i);
    j << "],\"cmp\":[";
    ring(g_hist_cmp, g_hist_cmp_n, g_hist_cmp_i);
    j << "],\"rates\":{";
    const char *names[4] = {"sync", "fup", "ann", "dresp"};
    for (int f = 0; f < 4; ++f) {
        j << (f ? "," : "") << "\"" << names[f] << "\":[";
        for (int k = 0; k < g_hist_rate_n; ++k) {
            int idx = (g_hist_rate_i - g_hist_rate_n + k + 2 * kHistN)
                      % kHistN;
            const RateSample &r = g_hist_rate[idx];
            uint16_t v = f == 0 ? r.sync : f == 1 ? r.fup
                       : f == 2 ? r.ann : r.dresp;
            j << (k ? "," : "") << v;
        }
        j << "]";
    }
    j << "}}";
    return j.str();
}

// Favicon: an LED-style clock face (served at /favicon.svg)
static const char *kFaviconSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">
 <rect width="64" height="64" rx="14" fill="#0b0b0b"/>
 <circle cx="32" cy="32" r="23" fill="none" stroke="#ffc400" stroke-width="4"/>
 <g stroke="#ffc400" stroke-width="3" stroke-linecap="round" opacity="0.55">
  <line x1="32" y1="13" x2="32" y2="17"/>
  <line x1="32" y1="47" x2="32" y2="51"/>
  <line x1="13" y1="32" x2="17" y2="32"/>
  <line x1="47" y1="32" x2="51" y2="32"/>
 </g>
 <g stroke="#ffc400" stroke-linecap="round" stroke-width="4.5">
  <line x1="32" y1="32" x2="24" y2="24"/>
  <line x1="32" y1="32" x2="42" y2="21"/>
 </g>
 <circle cx="32" cy="32" r="3.2" fill="#ffc400"/>
</svg>
)SVG";

static std::string status_json() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::ostringstream j;
    long since_change = -1;
    timespec now;
    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    if (g_gm_changes > 0)
        since_change = now.tv_sec - g_last_gm_change.tv_sec;

    double sync_age = -1;
    unsigned long long last_sync = g_last_sync_age_ns.load();
    if (last_sync > 0) {
        uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
        sync_age = (double)(now_ns - last_sync) / 1e9;
    }

    // Current PTP time (TAI), split so the values stay exact in JS doubles
    long long tai_sec = -1, tai_nsec = 0;
    if (have_ptp_ref) {
        long long tai = (long long)local_clock_ns() - g_offset_atomic.load();
        if (tai > 0) {
            tai_sec = tai / 1000000000LL;
            tai_nsec = tai % 1000000000LL;
        }
    }

    j << "{\"have_ptp\":" << (have_ptp_ref ? "true" : "false") << ","
      << "\"tai_sec\":" << tai_sec << ","
      << "\"tai_nsec\":" << tai_nsec << ","
      << "\"blackout\":" << (g_settings.blackout ? "true" : "false") << ","
      << "\"brightness\":" << g_settings.brightness << ","
      << "\"sync_age\":" << sync_age << ","
      << "\"domain\":" << g_domain.load() << ","
      << "\"active_domain\":" << g_active_domain.load() << ","
      << "\"iface\":\"" << json_escape(g_settings.iface) << "\","
      << "\"iface_active\":\"" << json_escape(g_joined_names) << "\","
      << "\"iface_up\":" << (g_iface_up ? "true" : "false") << ","
      << "\"hwts\":" << (g_hwts ? "true" : "false") << ","
      << "\"hwts_desc\":\"" << json_escape(g_hwts_desc) << "\","
      << "\"role\":" << g_role.load() << ","
      << "\"gm_enable\":" << (g_settings.gm_enable ? "true" : "false") << ","
      << "\"gnss_lock\":" << (g_gnss_lock ? "true" : "false") << ","
      << "\"gnss_serial_ok\":" << (g_gnss_serial_ok ? "true" : "false") << ","
      << "\"gnss_pps_ok\":" << (g_gnss_pps_ok ? "true" : "false") << ","
      << "\"gnss_sats_used\":" << g_gnss_sats_used.load() << ","
      << "\"gnss_sats_view\":" << g_gnss_sats_view.load() << ","
      << "\"gnss_fixq\":" << g_gnss_fixq.load() << ","
      << "\"gnss_hdop10\":" << g_gnss_hdop10.load() << ","
      << "\"gnss_pps_age\":"
      << (g_gnss_last_pps.load()
              ? (double)(mono_ns() - g_gnss_last_pps.load()) / 1e9
              : -1.0) << ","
      << "\"gnss_samples\":" << g_gnss_sample_count.load() << ","
      << "\"cmp_valid\":"
      << (g_cmp_target_valid.load() && g_cmp_count.load() > 0 ? "true"
                                                              : "false")
      << ","
      << "\"cmp_ns\":" << g_cmp_atomic.load() << ","
      << "\"cmp_last_ns\":" << g_cmp_last.load() << ","
      << "\"cmp_count\":" << g_cmp_count.load() << ","
      << "\"cmp_gm\":\"" << g_cmp_gm_str << "\","
      << "\"gm_id\":\"" << (g_have_gm ? format_gm(g_gm.id) : "") << "\","
      << "\"gm_vendor\":\"" << (g_have_gm ? lookup_vendor(g_gm.id) : "") << "\","
      << "\"gm_accepted\":" << (gm_acceptable_locked() ? "true" : "false") << ","
      << "\"priority1\":" << (int)g_gm.priority1 << ","
      << "\"priority2\":" << (int)g_gm.priority2 << ","
      << "\"clock_class\":" << (int)g_gm.clock_class << ","
      << "\"clock_accuracy\":" << (int)g_gm.clock_accuracy << ","
      << "\"variance\":" << g_gm.variance << ","
      << "\"steps_removed\":" << g_gm.steps_removed << ","
      << "\"time_source\":" << (int)g_gm.time_source << ","
      << "\"utc_offset\":" << current_utc_offset << ","
      << "\"path_delay_ns\":" << g_path_delay_ns.load() << ","
      << "\"dreq_sent\":" << g_dreq_sent.load() << ","
      << "\"dresp_received\":" << g_dresp_received.load() << ","
      << "\"gm_changes\":" << g_gm_changes << ","
      << "\"seconds_since_change\":" << since_change << ","
      << "\"masters\":[";
    for (size_t i = 0; i < g_masters.size(); ++i) {
        const ForeignMaster &m = g_masters[i];
        j << (i ? "," : "")
          << "{\"gm_id\":\"" << format_gm(m.gm.id) << "\","
          << "\"vendor\":\"" << lookup_vendor(m.gm.id) << "\","
          << "\"priority1\":" << (int)m.gm.priority1 << ","
          << "\"priority2\":" << (int)m.gm.priority2 << ","
          << "\"clock_class\":" << (int)m.gm.clock_class << ","
          << "\"steps_removed\":" << m.gm.steps_removed << ","
          << "\"self\":"
          << (memcmp(m.sender, g_clock_id, 8) == 0 ? "true" : "false") << ","
          << "\"elected\":" << (m.elected ? "true" : "false") << "}";
    }
    j << "],\"gnss_sats\":[";
    {
        std::lock_guard<std::mutex> lk(g_gnss_mutex);
        for (size_t i = 0; i < g_gnss_sats.size(); ++i) {
            const SatInfo &s = g_gnss_sats[i];
            j << (i ? "," : "") << "{\"sys\":\"" << s.talker[0]
              << s.talker[1] << "\",\"prn\":" << s.prn
              << ",\"snr\":" << s.snr << "}";
        }
    }
    j << "]}";
    return j.str();
}

static const char *kIndexHtml = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PTP Wallclock</title>
<link rel="icon" href="/favicon.svg" type="image/svg+xml">
<style>
 body { font-family: sans-serif; background: #1a1a1a; color: #eee;
        max-width: 520px; margin: 2em auto; padding: 0 1em; }
 h1 { font-size: 1.3em; }
 fieldset { border: 1px solid #444; border-radius: 6px; margin-bottom: 1em; }
 legend { padding: 0 0.4em; color: #aaa; }
 label { display: block; margin: 0.6em 0; }
 select, input[type=text], input[type=number] { padding: 0.3em;
        background: #2a2a2a; color: #eee; border: 1px solid #555; }
 input[type=text] { width: 100%; }
 input[type=number] { width: 6em; }
 input[type=color] { width: 4em; height: 2em; vertical-align: middle; }
 button { padding: 0.5em 1.5em; background: #2d6cdf; color: #fff;
        border: none; border-radius: 4px; cursor: pointer; }
 button:hover { background: #3d7cef; }
 button.blackout-on { background: #b33; }
 button.blackout-on:hover { background: #c44; }
 input[type=range] { width: 100%; }
 table.status { width: 100%; font-size: 0.9em; border-collapse: collapse; }
 table.status td { padding: 0.15em 0.3em; color: #aaa; }
 table.status td + td { color: #eee; text-align: right; font-family: monospace; }
 table.masters { width: 100%; font-size: 0.85em; border-collapse: collapse;
        font-family: monospace; }
 table.masters th { color: #888; font-weight: normal; text-align: right;
        padding: 0.15em 0.3em; }
 table.masters td { color: #eee; text-align: right; padding: 0.15em 0.3em; }
 table.masters th:first-child, table.masters td:first-child { text-align: left; }
 table.masters tr.elected td { color: #6c6; }
 #banner, #gmwarn { display: none; background: #b33; color: #fff;
        padding: 0.6em; border-radius: 4px; margin-bottom: 1em; }
 #saved { color: #6c6; visibility: hidden; margin-left: 1em; }
 .hint { color: #888; font-size: 0.85em; margin: 0.8em 0 0.2em; }
 canvas.chart { width: 100%; height: 110px; background: #111;
        border: 1px solid #333; border-radius: 4px; display: block;
        cursor: zoom-in; }
 .chart-title { color: #aaa; font-size: 0.85em; margin: 0.7em 0 0.2em; }
 .chart-legend { color: #888; font-size: 0.8em; margin: 0.2em 0 0.4em;
        font-family: monospace; }
 .snrwrap { display: flex; align-items: flex-end; gap: 3px; height: 52px;
        margin: 0.3em 0 1.4em; }
 .snrbar { width: 11px; border-radius: 2px 2px 0 0; position: relative; }
 .snrbar span { position: absolute; top: 100%; left: 50%;
        transform: translateX(-50%); font-size: 0.6em; color: #777;
        font-family: monospace; }
 .crow { display: flex; gap: 4px; margin: 0.3em 0; }
 .crow select, .crow input { padding: 0.25em; background: #2a2a2a;
        color: #eee; border: 1px solid #555; }
 .crow input { flex: 1; min-width: 0; }
 .crow .c_del { padding: 0.1em 0.55em; background: #733; }
 #add_clock { padding: 0.25em 0.8em; background: #364; }
 #clock { font-family: monospace; text-align: center; color: #fd0;
        font-size: clamp(1em, 4.4vw, 1.8em); white-space: nowrap; }
 a { color: #7ab; }
 #clockzone { font-family: monospace; text-align: center; color: #db0;
        letter-spacing: 0.2em; margin-top: 0.15em; }
 #clockdate { font-family: monospace; text-align: center; color: #aaa;
        margin-top: 0.1em; }
</style>
</head>
<body>
<h1>PTP Wallclock &ndash; Settings</h1>
<div id="banner"></div>
<div id="gmwarn"></div>

<fieldset>
<legend>PTP time &nbsp;<a href="/clock" target="_blank" rel="noopener">fullscreen clock &rarr;</a></legend>
<div id="clock">--:--:--</div>
<div id="clockzone"></div>
<div id="clockdate"></div>
</fieldset>

<fieldset>
<legend>Status</legend>
<table class="status">
 <tr><td>PTP</td><td id="s_ptp">&ndash;</td></tr>
 <tr><td>Role</td><td id="s_role">&ndash;</td></tr>
 <tr><td>Interface</td><td id="s_iface">&ndash;</td></tr>
 <tr><td>Timestamping</td><td id="s_hwts">&ndash;</td></tr>
 <tr><td>Domain</td><td id="s_domain">&ndash;</td></tr>
 <tr><td>Grandmaster</td><td id="s_gm">&ndash;</td></tr>
 <tr><td>Vendor</td><td id="s_vendor">&ndash;</td></tr>
 <tr><td>Priority 1 / 2</td><td id="s_prio">&ndash;</td></tr>
 <tr><td>Clock class</td><td id="s_class">&ndash;</td></tr>
 <tr><td>Accuracy</td><td id="s_acc">&ndash;</td></tr>
 <tr><td>Variance</td><td id="s_var">&ndash;</td></tr>
 <tr><td>Steps removed</td><td id="s_steps">&ndash;</td></tr>
 <tr><td>Time source</td><td id="s_src">&ndash;</td></tr>
 <tr><td>TAI&minus;UTC offset</td><td id="s_off">&ndash;</td></tr>
 <tr><td>Path delay</td><td id="s_delay">&ndash;</td></tr>
 <tr><td>GM changes</td><td id="s_changes">0</td></tr>
</table>
</fieldset>

<fieldset>
<legend>Visible masters (BMCA)</legend>
<table class="masters" id="masters"><tr><td>none</td></tr></table>
</fieldset>

<fieldset>
<legend>PTP analysis &nbsp;<a href="/analysis" target="_blank"
 rel="noopener">large view &rarr;</a></legend>
<div class="chart-legend" id="ts_mode" style="margin:0 0 0.5em">&ndash;</div>
<div class="chart-title">Sync offset jitter &amp; path delay</div>
<canvas class="chart" id="ch_off"></canvas>
<div class="chart-legend"><span style="color:#fd0">&#9632;</span> offset
deviation per Sync &nbsp;<span style="color:#6cf">&#9632;</span> path delay
samples</div>
<div class="chart-title">Received message rates (per second, this domain)</div>
<canvas class="chart" id="ch_rate"></canvas>
<div class="chart-legend"><span style="color:#6c6">&#9632;</span> Sync
&nbsp;<span style="color:#6cf">&#9632;</span> Follow_Up
&nbsp;<span style="color:#fd0">&#9632;</span> Announce
&nbsp;<span style="color:#f6c">&#9632;</span> Delay_Resp
&nbsp;&nbsp;<span id="rate_now"></span></div>
<div id="cmp_wrap" style="display:none">
<div class="chart-title">Network PTP vs GNSS (per Sync of the network
master)</div>
<canvas class="chart" id="ch_cmp"></canvas>
<div class="chart-legend"><span style="color:#f96">&#9632;</span>
<span id="cmp_info">network master &minus; GNSS</span></div>
</div>
</fieldset>

<form id="form">
<fieldset>
<legend>Display</legend>
<p style="margin: 0.6em 0">
 <button type="button" id="blackout_btn">Blackout &ndash; turn display off</button>
</p>
<label>Display color:
 <input type="color" id="color" value="#ffff00">
</label>
<label>Brightness: <span id="bval">100</span> %
 <input type="range" id="brightness" min="1" max="100" value="100">
</label>
<p class="hint">Clock lines — one line is static, several alternate every
4 seconds. The name is shown as the small label (blank = no label,
<code>%Z</code> = zone abbreviation). The <a href="/clock">browser
clock</a> follows the same lines (pixel styles fall back to digital):</p>
<div id="clock_rows"></div>
<p style="margin: 0.3em 0">
 <button type="button" id="add_clock">+ add clock line</button>
</p>
<label>
 <input type="checkbox" id="rotate180"> Rotate display 180&deg; (LED matrix mounted upside down)
</label>
<p class="hint">The 2nd-line options below only affect the physical LED
matrix:</p>
<label>
 <input type="checkbox" id="show_gm"> Show grandmaster ID (2nd line)
</label>
<label>
 <input type="checkbox" id="show_gm_details"> Show priorities &amp; clock quality (2nd line)
</label>
<label>
 <input type="checkbox" id="show_date"> Show date (2nd line)
</label>
</fieldset>

<fieldset>
<legend>Date</legend>
<label>Date format:
 <select id="date_format">
  <option value="dmy">31.12.2026 (DD.MM.YYYY)</option>
  <option value="iso">2026-12-31 (ISO 8601)</option>
  <option value="mdy">12/31/2026 (MM/DD/YYYY)</option>
 </select>
</label>
</fieldset>

<fieldset>
<legend>PTP</legend>
<label>
 <input type="checkbox" id="domain_auto" checked> Detect domain automatically
</label>
<label>Domain:
 <input type="number" id="domain" min="0" max="255" value="0" disabled>
</label>
<label>Network interface:
 <input type="text" id="iface" list="iflist" value="eth0">
 <datalist id="iflist"></datalist>
</label>
<label>Acceptable grandmasters (comma-separated, empty = any):
 <input type="text" id="acceptable_gms"
        placeholder="00:1d:c1:ff:fe:12:34:56, 00:1d:c1:ff:fe:aa:bb:cc">
</label>
</fieldset>

<fieldset>
<legend>PTP grandmaster (GNSS)</legend>
<label>
 <input type="checkbox" id="gm_enable"> Act as PTP grandmaster when a
 GNSS receiver provides time (NMEA + PPS)
</label>
<p class="hint">Needs a GNSS module with its PPS pulse on a GPIO
(<code>dtoverlay=pps-gpio,gpiopin=18</code>) and NMEA on a serial port —
see the README. With GNSS lock the clock announces itself with
clockClass&nbsp;6 / GPS; a better grandmaster on the network still wins
the BMCA, and its Syncs are then measured against GNSS (chart above).</p>
<label>NMEA serial device:
 <input type="text" id="gm_serial" list="serlist" value="/dev/serial0">
 <datalist id="serlist"><option value="/dev/serial0">
 <option value="/dev/ttyAMA0"><option value="/dev/ttyS0">
 <option value="/dev/ttyACM0"><option value="/dev/ttyUSB0"></datalist>
</label>
<label>PPS device:
 <input type="text" id="gm_pps" value="/dev/pps0">
</label>
<label>Priority 1:
 <input type="number" id="gm_prio1" min="0" max="255" value="128">
</label>
<label>Priority 2:
 <input type="number" id="gm_prio2" min="0" max="255" value="128">
</label>
<label>TAI &minus; UTC:
 <input type="number" id="gm_utc" min="0" max="99" value="37">
</label>
<div id="gnss_box" style="display:none">
 <div class="chart-title">GNSS receiver</div>
 <div class="chart-legend" id="gnss_sum">&ndash;</div>
 <div class="snrwrap" id="gnss_bars"></div>
 <div class="chart-legend" id="gnss_cmp">&ndash;</div>
</div>
</fieldset>

<fieldset>
<legend>Notification</legend>
<label>
 <input type="checkbox" id="notify"> Notify on grandmaster change
</label>
</fieldset>

<button type="submit">Save</button><span id="saved">Saved</span>
</form>

<script>
const ACCURACY = {0x20:'25 ns',0x21:'100 ns',0x22:'250 ns',0x23:'1 µs',
 0x24:'2.5 µs',0x25:'10 µs',0x26:'25 µs',0x27:'100 µs',
 0x28:'250 µs',0x29:'1 ms',0x2A:'2.5 ms',0x2B:'10 ms',0x2C:'25 ms',
 0x2D:'100 ms',0x2E:'250 ms',0x2F:'1 s',0x30:'10 s',0x31:'>10 s',
 0xFE:'unknown'};
const CLOCK_CLASS = {6:'GNSS locked',7:'holdover',13:'application specific',
 14:'application holdover',52:'degraded A',58:'degraded A (holdover)',
 187:'degraded B',193:'degraded B (holdover)',248:'default',
 255:'slave-only'};
const TIME_SOURCE = {0x10:'atomic clock',0x20:'GPS/GNSS',0x30:'terrestrial radio',
 0x40:'PTP',0x50:'NTP',0x60:'hand set',0x90:'other',
 0xA0:'internal oscillator'};
const hex = (v, w) => '0x' + v.toString(16).toUpperCase().padStart(w, '0');

function syncDomainInput() {
  document.getElementById('domain').disabled =
      document.getElementById('domain_auto').checked;
}
document.getElementById('domain_auto').addEventListener('change', syncDomainInput);

// --- LED clock-line list editor ---
const STYLES = [['24h', 'digital 24h'], ['12h', 'digital 12h (AM/PM)'],
  ['unix', 'Unix timestamp'], ['bcd', 'binary (BCD)'],
  ['flip', 'flip clock'], ['dcf77', 'DCF77 telegram'],
  ['graph', 'PTP graph (jitter/delay)'], ['rates', 'PTP message rates']];
const ZONES = ['UTC', 'TAI', 'Europe/Berlin', 'Europe/Zurich',
  'Europe/Vienna', 'Europe/London', 'Europe/Paris', 'Europe/Moscow',
  'America/New_York', 'America/Chicago', 'America/Denver',
  'America/Los_Angeles', 'America/Sao_Paulo', 'Asia/Dubai',
  'Asia/Kolkata', 'Asia/Shanghai', 'Asia/Tokyo', 'Asia/Singapore',
  'Australia/Sydney', 'Pacific/Auckland'];
function clockRow(e) {
  const div = document.createElement('div');
  div.className = 'crow';
  const zones = ZONES.includes(e.zone) || !e.zone
      ? ZONES : [e.zone].concat(ZONES);
  div.innerHTML =
    '<select class="c_zone">' + zones.map(v =>
      '<option value="' + v + '"' + (v === e.zone ? ' selected' : '') +
      '>' + v + '</option>').join('') + '</select>' +
    '<select class="c_style">' + STYLES.map(p =>
      '<option value="' + p[0] + '"' + (p[0] === e.style ? ' selected' : '') +
      '>' + p[1] + '</option>').join('') + '</select>' +
    '<input class="c_name" maxlength="24" placeholder="label (blank = none)">' +
    '<button type="button" class="c_del" title="remove">&times;</button>';
  div.querySelector('.c_name').value = e.name || '';
  div.querySelector('.c_del').addEventListener('click', () => {
    if (document.querySelectorAll('#clock_rows .crow').length > 1)
      div.remove();
  });
  return div;
}
function setClockRows(list) {
  const c = document.getElementById('clock_rows');
  c.innerHTML = '';
  (list && list.length ? list : [{zone: 'UTC', style: '24h', name: ''}])
      .forEach(e => c.appendChild(clockRow(e)));
}
document.getElementById('add_clock').addEventListener('click', () => {
  document.getElementById('clock_rows').appendChild(
      clockRow({zone: 'UTC', style: '24h', name: ''}));
});
function clocksSerialized() {
  return Array.from(document.querySelectorAll('#clock_rows .crow')).map(r =>
    r.querySelector('.c_zone').value + ',' +
    r.querySelector('.c_style').value + ',' +
    r.querySelector('.c_name').value.replace(/;/g, ' ').trim()
  ).join(';');
}

// Blackout toggle and brightness slider apply immediately (no Save needed)
let blackout = false;
function renderBlackout() {
  const b = document.getElementById('blackout_btn');
  b.textContent = blackout
      ? 'Blackout active – turn display on'
      : 'Blackout – turn display off';
  b.className = blackout ? 'blackout-on' : '';
}
document.getElementById('blackout_btn').addEventListener('click', async () => {
  blackout = !blackout;
  renderBlackout();
  await fetch('/api/settings', { method: 'POST',
      body: new URLSearchParams({ blackout: blackout ? 1 : 0 }) });
});
document.getElementById('brightness').addEventListener('input', (e) => {
  document.getElementById('bval').textContent = e.target.value;
});
document.getElementById('brightness').addEventListener('change', async (e) => {
  await fetch('/api/settings', { method: 'POST',
      body: new URLSearchParams({ brightness: e.target.value }) });
});

async function loadSettings() {
  const s = await fetch('/api/settings').then(r => r.json());
  document.getElementById('color').value = s.color;
  document.getElementById('brightness').value = s.brightness;
  document.getElementById('bval').textContent = s.brightness;
  document.getElementById('rotate180').checked = s.rotate180;
  setClockRows(s.clocks);
  blackout = s.blackout;
  renderBlackout();
  document.getElementById('show_gm').checked = s.show_gm;
  document.getElementById('show_gm_details').checked = s.show_gm_details;
  document.getElementById('show_date').checked = s.show_date;
  document.getElementById('notify').checked = s.notify_gm_change;
  document.getElementById('domain_auto').checked = (s.domain === -1);
  document.getElementById('domain').value = (s.domain === -1) ? 0 : s.domain;
  syncDomainInput();
  document.getElementById('acceptable_gms').value = s.acceptable_gms;
  document.getElementById('gm_enable').checked = s.gm_enable;
  document.getElementById('gm_serial').value = s.gm_serial;
  document.getElementById('gm_pps').value = s.gm_pps;
  document.getElementById('gm_prio1').value = s.gm_prio1;
  document.getElementById('gm_prio2').value = s.gm_prio2;
  document.getElementById('gm_utc').value = s.gm_utc_offset;
  document.getElementById('iface').value = s.iface;
  document.getElementById('iflist').innerHTML =
      ['auto'].concat(s.ifaces).map(i => '<option value="' + i + '">').join('');
  document.getElementById('date_format').value = s.date_format;
}

document.getElementById('form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const body = new URLSearchParams({
    color: document.getElementById('color').value,
    brightness: document.getElementById('brightness').value,
    rotate180: document.getElementById('rotate180').checked ? 1 : 0,
    clocks: clocksSerialized(),
    show_gm: document.getElementById('show_gm').checked ? 1 : 0,
    show_gm_details: document.getElementById('show_gm_details').checked ? 1 : 0,
    show_date: document.getElementById('show_date').checked ? 1 : 0,
    date_format: document.getElementById('date_format').value,
    domain: document.getElementById('domain_auto').checked
        ? -1 : document.getElementById('domain').value,
    acceptable_gms: document.getElementById('acceptable_gms').value,
    gm_enable: document.getElementById('gm_enable').checked ? 1 : 0,
    gm_serial: document.getElementById('gm_serial').value,
    gm_pps: document.getElementById('gm_pps').value,
    gm_prio1: document.getElementById('gm_prio1').value,
    gm_prio2: document.getElementById('gm_prio2').value,
    gm_utc_offset: document.getElementById('gm_utc').value,
    iface: document.getElementById('iface').value,
    notify: document.getElementById('notify').checked ? 1 : 0
  });
  await fetch('/api/settings', { method: 'POST', body });
  const saved = document.getElementById('saved');
  saved.style.visibility = 'visible';
  setTimeout(() => saved.style.visibility = 'hidden', 1500);
  if (document.getElementById('notify').checked &&
      'Notification' in window && Notification.permission === 'default') {
    Notification.requestPermission();
  }
});

// Live PTP clock: the server sends TAI at poll time, the browser
// extrapolates in between. Fractional digits use a per-position speed
// ladder (same as the LED): the tenths digit is real, every further
// digit visibly changes faster than the one before it — sample-and-hold
// with a deterministic hash, since the true values change far too fast
// for any display anyway.
const FRAC_HOLD = [1e8, 6.6e7, 4.36e7, 2.87e7, 1.9e7,
                   1.25e7, 8.3e6, 5.5e6, 3.6e6];
function fracDigits(tNs) {
  let s = String(Math.floor(tNs / 1e8) % 10);
  for (let k = 1; k < 9; k++) {
    const idx = Math.floor(tNs / FRAC_HOLD[k]) >>> 0;
    const v = (Math.imul(idx, 2654435761) + (k + 1) * 40503) >>> 0;
    s += (v >>> 16) % 10;
  }
  return s;
}

let clockBase = null;   // {sec, nsec, perf, off}
function renderClock() {
  const el = document.getElementById('clock');
  const ez = document.getElementById('clockzone');
  const ed = document.getElementById('clockdate');
  if (!clockBase) {
    el.textContent = '--:--:--';
    ez.textContent = '';
    ed.textContent = '';
    return;
  }
  const df = document.getElementById('date_format').value;

  const elapsed = performance.now() - clockBase.perf;
  const totalNs = clockBase.nsec + elapsed * 1e6;
  let sec = clockBase.sec + Math.floor(totalNs / 1e9);
  // Reduced TAI ns (mod ~1 day) keeps the math exact in doubles
  const fr9 = fracDigits((clockBase.sec % 100000) * 1e9 + totalNs);

  // Active clock line straight from the editor rows (live preview),
  // same PTP-second-aligned rotation as the LED display
  const rows = document.querySelectorAll('#clock_rows .crow');
  let zone = 'UTC', style = '24h', name = '';
  if (rows.length) {
    const r = rows[rows.length > 1 ? Math.floor(sec / 4) % rows.length : 0];
    zone = r.querySelector('.c_zone').value;
    style = r.querySelector('.c_style').value;
    name = r.querySelector('.c_name').value.trim();
  }
  const tz = (zone === 'UTC' || zone === 'TAI') ? 'UTC' : zone;
  if (zone !== 'TAI') sec -= clockBase.off;      // TAI -> UTC

  const d = new Date(sec * 1000);
  let p;
  try {
    p = new Intl.DateTimeFormat('en-GB', { timeZone: tz, hourCycle: 'h23',
        hour: '2-digit', minute: '2-digit', second: '2-digit',
        weekday: 'short', day: '2-digit', month: '2-digit', year: 'numeric',
        timeZoneName: 'short'
      }).formatToParts(d).reduce((a, x) => (a[x.type] = x.value, a), {});
  } catch (e) {                                  // unknown time zone
    el.textContent = '--:--:--';
    ez.textContent = '';
    ed.textContent = 'unknown time zone: ' + tz;
    return;
  }
  // Body text per style (pixel styles fall back to digital 24h)
  let body;
  if (style === 'unix') {
    body = sec + '.' + fr9;
  } else if (style === '12h') {
    let h = parseInt(p.hour, 10);
    const am = h >= 12 ? ' PM' : ' AM';
    h = h % 12 || 12;
    body = String(h).padStart(2, '0') + ':' + p.minute + ':' +
        p.second + '.' + fr9 + am;
  } else {
    body = p.hour + ':' + p.minute + ':' + p.second + '.' + fr9;
  }
  el.textContent = body;
  ez.textContent = name === '%Z'
      ? (zone === 'TAI' ? 'TAI' : (p.timeZoneName || '')) : name;
  const wd = p.weekday.toUpperCase();
  ed.textContent = df === 'iso'
      ? wd + ' ' + p.year + '-' + p.month + '-' + p.day
      : df === 'mdy'
        ? wd + ' ' + p.month + '/' + p.day + '/' + p.year
        : wd + ' ' + p.day + '.' + p.month + '.' + p.year;
}
(function clockTick() {
  renderClock();
  requestAnimationFrame(clockTick);
})();

// --- Analysis charts (plain canvas, no libraries) ---
function drawChart(id, series, fmt, includeZero) {
  const cv = document.getElementById(id);
  const w = cv.width = cv.clientWidth * 2;      // 2x for crisp lines
  const h = cv.height = 220;
  const g = cv.getContext('2d');
  g.clearRect(0, 0, w, h);
  let all = [];
  series.forEach(s => { all = all.concat(s.data); });
  if (!all.length) {
    g.fillStyle = '#666';
    g.font = '20px sans-serif';
    g.fillText('collecting data...', 10, 30);
    return;
  }
  // Robust autoscale: percentiles keep step outliers (master switches)
  // from flattening the interesting range; outliers clip at the edge
  const sorted = all.slice().sort((a, b) => a - b);
  const q = f => sorted[Math.max(0, Math.min(sorted.length - 1,
      Math.floor(f * (sorted.length - 1))))];
  let mn = q(0.03), mx = q(0.97);
  if (includeZero) { mn = Math.min(mn, 0); mx = Math.max(mx, 0); }
  if (mx === mn) { mx += 1; mn -= 1; }
  const pad = (mx - mn) * 0.12;
  const lo = mn - pad, hi = mx + pad;
  const y = v => Math.max(1, Math.min(h - 1,
      h - (v - lo) / (hi - lo) * h));
  if (includeZero && mn <= 0 && mx >= 0) {
    g.strokeStyle = '#333';
    g.beginPath(); g.moveTo(0, y(0)); g.lineTo(w, y(0)); g.stroke();
  }
  series.forEach(s => {
    if (!s.data.length) return;
    g.strokeStyle = s.color;
    g.lineWidth = 2;
    g.beginPath();
    s.data.forEach((v, k) => {
      const x = k / Math.max(1, s.data.length - 1) * w;
      k ? g.lineTo(x, y(v)) : g.moveTo(x, y(v));
    });
    g.stroke();
  });
  g.fillStyle = '#888';
  g.font = '18px monospace';
  g.fillText(fmt(mx), 6, 20);
  g.fillText(fmt(mn), 6, h - 8);
}

async function pollHistory() {
  try {
    const hh = await fetch('/api/history').then(r => r.json());
    drawChart('ch_off', [
      {data: hh.offset.map(v => v / 1000), color: '#fd0'},
      {data: hh.delay.map(v => v / 1000), color: '#6cf'}
    ], v => v.toFixed(1) + ' µs', true);
    drawChart('ch_rate', [
      {data: hh.rates.sync, color: '#6c6'},
      {data: hh.rates.fup, color: '#6cf'},
      {data: hh.rates.ann, color: '#fd0'},
      {data: hh.rates.dresp, color: '#f6c'}
    ], v => Math.round(v) + '/s', true);
    const last = a => a.length ? a[a.length - 1] : 0;
    document.getElementById('rate_now').textContent =
        'now: ' + last(hh.rates.sync) + '/' + last(hh.rates.fup) + '/' +
        last(hh.rates.ann) + '/' + last(hh.rates.dresp);
    const cw = document.getElementById('cmp_wrap');
    if (hh.cmp && hh.cmp.length) {
      cw.style.display = '';
      drawChart('ch_cmp', [{data: hh.cmp.map(v => v / 1000), color: '#f96'}],
                v => v.toFixed(1) + ' µs', true);
    } else {
      cw.style.display = 'none';
    }
  } catch (e) {}
}
pollHistory();
setInterval(pollHistory, 2000);
for (const id of ['ch_off', 'ch_rate', 'ch_cmp'])
  document.getElementById(id).onclick =
      () => window.open('/analysis', '_blank');

const set = (id, text) => document.getElementById(id).textContent = text;
let lastChanges = null;
async function poll() {
  try {
    const s = await fetch('/api/status').then(r => r.json());
    if (s.tai_sec < 0) {
      clockBase = null;
    } else {
      // Client-side delays only make the clock late, never early: keep
      // the base implying the latest time, force-refresh every 30 s.
      const perf = performance.now();
      const cand = { sec: s.tai_sec, nsec: s.tai_nsec, perf,
                     off: s.utc_offset,
                     val: s.tai_sec * 1000 + s.tai_nsec / 1e6 - perf };
      if (!clockBase || cand.val > clockBase.val ||
          perf - clockBase.perf > 30000) {
        clockBase = cand;
      } else {
        clockBase.off = s.utc_offset;
      }
    }
    set('s_ptp', s.have_ptp
        ? 'synchronized (sync ' + s.sync_age.toFixed(1) + ' s ago)'
        : 'waiting for PTP...');
    set('s_iface', s.iface === 'auto'
        ? (s.iface_active ? 'auto (' + s.iface_active + ')'
                          : 'auto (searching...)')
        : s.iface + (s.iface_up ? '' : ' (not connected)'));
    set('s_hwts', s.hwts ? 'hardware (' + s.hwts_desc + ')' : 'software');
    set('s_role', !s.gm_enable ? 'client'
        : s.role === 3 ? 'GRANDMASTER (GNSS)'
        : s.role === 2 ? 'passive — better grandmaster on the network'
        : 'client (waiting for GNSS)');
    const gb = document.getElementById('gnss_box');
    gb.style.display = s.gm_enable ? '' : 'none';
    if (s.gm_enable) {
      let sum;
      if (!s.gnss_serial_ok && !s.gnss_pps_ok)
        sum = 'no receiver (serial and PPS device closed)';
      else {
        sum = (s.gnss_lock ? 'locked' :
               s.gnss_fixq > 0 ? 'fix, waiting for PPS pairing' : 'no fix');
        if (s.gnss_sats_used >= 0)
          sum += ' — ' + s.gnss_sats_used + ' sats used';
        if (s.gnss_sats_view >= 0)
          sum += ', ' + s.gnss_sats_view + ' in view';
        if (s.gnss_hdop10 >= 0)
          sum += ', HDOP ' + (s.gnss_hdop10 / 10).toFixed(1);
        if (s.gnss_pps_age >= 0 && s.gnss_pps_age < 100)
          sum += ', PPS ' + s.gnss_pps_age.toFixed(1) + ' s ago';
        else if (s.gnss_pps_ok)
          sum += s.gnss_pps_age >= 0 ? ', PPS lost!' : ', no PPS pulse yet';
        if (!s.gnss_serial_ok) sum += ' — serial closed!';
        if (!s.gnss_pps_ok) sum += ' — PPS closed!';
        sum += ' (' + s.gnss_samples + ' samples)';
      }
      document.getElementById('gnss_sum').textContent = sum;
      const snrCol = v => v >= 35 ? '#6c6' : v >= 20 ? '#fd0' : '#f66';
      document.getElementById('gnss_bars').innerHTML =
        (s.gnss_sats || []).slice().sort((a, b) =>
            a.sys === b.sys ? a.prn - b.prn : (a.sys < b.sys ? -1 : 1))
          .map(t => {
            const v = Math.max(0, t.snr);
            const h = 4 + Math.min(46, v);
            return '<div class="snrbar" title="' + t.sys + ' ' + t.prn +
                   ': ' + (t.snr < 0 ? 'searching' : t.snr + ' dB-Hz') +
                   '" style="height:' + h + 'px;background:' +
                   (t.snr < 0 ? '#444' : snrCol(v)) +
                   '"><span>' + t.prn + '</span></div>';
          }).join('');
      document.getElementById('gnss_cmp').textContent = s.cmp_valid
        ? 'network PTP vs GNSS: ' + (s.cmp_ns / 1000).toFixed(1) +
          ' µs (last ' + (s.cmp_last_ns / 1000).toFixed(1) + ' µs, ' +
          s.cmp_count + ' Syncs, vs ' + s.cmp_gm + ')'
        : 'network PTP vs GNSS: no comparison master';
    }
    document.getElementById('ts_mode').innerHTML = s.hwts
        ? '<span style="color:#6c6">&#9679;</span> hardware timestamping ('
          + s.hwts_desc + ') &mdash; t2/t3 stamped by the NIC, clock reads '
          + 'the PHC'
        : '<span style="color:#fd0">&#9679;</span> software timestamping '
          + '&mdash; measurements include OS scheduling jitter';
    set('s_domain', s.domain === -1
        ? (s.active_domain >= 0
           ? s.active_domain + ' (auto-detected)'
           : 'scanning... (auto)')
        : s.domain);
    const gm = s.gm_id !== '';
    const badGm = gm && s.gm_accepted === false;
    set('s_gm', gm ? s.gm_id + (badGm ? ' — NOT ACCEPTED' : '') : 'unknown');
    document.getElementById('s_gm').style.color = badGm ? '#f66' : '';
    set('s_vendor', gm ? (s.gm_vendor || 'unknown (OUI not in list)') : '–');
    const w = document.getElementById('gmwarn');
    w.style.display = badGm ? 'block' : 'none';
    if (badGm)
      w.textContent = 'Unaccepted grandmaster: ' + s.gm_id +
          ' is not on the acceptable list!';
    set('s_prio', gm ? s.priority1 + ' / ' + s.priority2 : '–');
    set('s_class', gm ? s.clock_class + ' (' +
        (CLOCK_CLASS[s.clock_class] || 'reserved') + ')' : '–');
    set('s_acc', gm ? (ACCURACY[s.clock_accuracy] || 'reserved') +
        ' (' + hex(s.clock_accuracy, 2) + ')' : '–');
    set('s_var', gm ? hex(s.variance, 4) : '–');
    set('s_steps', gm ? s.steps_removed : '–');
    set('s_src', gm ? (TIME_SOURCE[s.time_source] || 'reserved') +
        ' (' + hex(s.time_source, 2) + ')' : '–');
    set('s_off', s.utc_offset + ' s');
    set('s_delay', s.path_delay_ns > 0
        ? (s.path_delay_ns / 1000).toFixed(1) + ' µs (' +
          s.dresp_received + '/' + s.dreq_sent + ' responses)'
        : s.dreq_sent > 0
          ? 'no response (' + s.dreq_sent + ' requests)'
          : '–');
    set('s_changes', s.gm_changes);
    if (s.blackout !== blackout) {
      blackout = s.blackout;             // changed from another browser
      renderBlackout();
    }
    document.getElementById('masters').innerHTML = s.masters.length
      ? '<tr><th>Grandmaster</th><th>P1</th><th>P2</th><th>Class</th>' +
        '<th>Steps</th><th></th></tr>' +
        s.masters.map(m =>
          '<tr' + (m.elected ? ' class="elected"' : '') + '><td title="' +
          (m.vendor || '') + '">' + m.gm_id +
          (m.self ? ' (this clock)' : '') +
          '</td><td>' + m.priority1 + '</td><td>' + m.priority2 +
          '</td><td>' + m.clock_class + '</td><td>' + m.steps_removed +
          '</td><td>' + (m.elected ? '&#9733;' : '') + '</td></tr>').join('')
      : '<tr><td>none</td></tr>';
    if (lastChanges !== null && s.gm_changes > lastChanges &&
        document.getElementById('notify').checked) {
      const msg = 'PTP grandmaster change! New GM: ' + s.gm_id;
      const banner = document.getElementById('banner');
      banner.textContent = msg;
      banner.style.display = 'block';
      setTimeout(() => banner.style.display = 'none', 30000);
      if ('Notification' in window && Notification.permission === 'granted') {
        new Notification('PTP Wallclock', { body: msg });
      }
    }
    lastChanges = s.gm_changes;
  } catch (e) { /* server gone, keep polling */ }
}
loadSettings();
poll();
setInterval(poll, 2000);
</script>
</body>
</html>
)HTML";

// Fullscreen browser clock ("the display" for headless installations)
// Fullscreen version of the analysis charts (linked from the settings
// page, or click one of the small charts)
static const char *kAnalysisHtml = R"ANA(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PTP analysis</title>
<link rel="icon" type="image/svg+xml" href="/favicon.svg">
<style>
 * { box-sizing: border-box; }
 body { background: #000; color: #ccc; font-family: monospace;
        margin: 0; padding: 1em 1.4em; }
 h1 { font-size: 1.05em; color: #999; font-weight: normal;
      margin: 0 0 0.2em; }
 h1 a { color: #58a6ff; text-decoration: none; font-size: 0.9em; }
 #ts_mode { color: #888; font-size: 0.9em; margin: 0 0 0.8em; }
 .chart-title { color: #aaa; margin: 0.5em 0 0.3em; }
 canvas { width: 100%; height: 34vh; display: block; background: #0a0a0a;
          border: 1px solid #222; border-radius: 4px; }
 .chart-legend { color: #888; font-size: 0.9em; margin: 0.3em 0 0.6em; }
</style>
</head>
<body>
<h1>PTP analysis &nbsp;<a href="/">&larr; settings</a></h1>
<div id="ts_mode">&ndash;</div>
<div class="chart-title">Sync offset jitter &amp; path delay</div>
<canvas id="ch_off"></canvas>
<div class="chart-legend"><span style="color:#fd0">&#9632;</span> offset
deviation per Sync &nbsp;<span style="color:#6cf">&#9632;</span> path delay
samples</div>
<div class="chart-title">Received message rates (per second, this domain)</div>
<canvas id="ch_rate"></canvas>
<div class="chart-legend"><span style="color:#6c6">&#9632;</span> Sync
&nbsp;<span style="color:#6cf">&#9632;</span> Follow_Up
&nbsp;<span style="color:#fd0">&#9632;</span> Announce
&nbsp;<span style="color:#f6c">&#9632;</span> Delay_Resp
&nbsp;&nbsp;<span id="rate_now"></span></div>
<div id="cmp_wrap" style="display:none">
<div class="chart-title">Network PTP vs GNSS</div>
<canvas id="ch_cmp"></canvas>
<div class="chart-legend"><span style="color:#f96">&#9632;</span>
network master &minus; GNSS, per Sync &nbsp;<span id="cmp_now"></span></div>
</div>
<script>
function drawChart(id, series, fmt, includeZero) {
  const cv = document.getElementById(id);
  const w = cv.width = cv.clientWidth * 2;      // 2x for crisp lines
  const h = cv.height = cv.clientHeight * 2;
  const g = cv.getContext('2d');
  g.clearRect(0, 0, w, h);
  let all = [];
  series.forEach(s => { all = all.concat(s.data); });
  if (!all.length) {
    g.fillStyle = '#666';
    g.font = '28px sans-serif';
    g.fillText('collecting data...', 14, 40);
    return;
  }
  const sorted = all.slice().sort((a, b) => a - b);
  const q = f => sorted[Math.max(0, Math.min(sorted.length - 1,
      Math.floor(f * (sorted.length - 1))))];
  let mn = q(0.03), mx = q(0.97);
  if (includeZero) { mn = Math.min(mn, 0); mx = Math.max(mx, 0); }
  if (mx === mn) { mx += 1; mn -= 1; }
  const pad = (mx - mn) * 0.12;
  const lo = mn - pad, hi = mx + pad;
  const y = v => Math.max(1, Math.min(h - 1,
      h - (v - lo) / (hi - lo) * h));
  if (includeZero && mn <= 0 && mx >= 0) {
    g.strokeStyle = '#333';
    g.beginPath(); g.moveTo(0, y(0)); g.lineTo(w, y(0)); g.stroke();
  }
  series.forEach(s => {
    if (!s.data.length) return;
    g.strokeStyle = s.color;
    g.lineWidth = 3;
    g.beginPath();
    s.data.forEach((v, k) => {
      const x = k / Math.max(1, s.data.length - 1) * w;
      k ? g.lineTo(x, y(v)) : g.moveTo(x, y(v));
    });
    g.stroke();
  });
  g.fillStyle = '#888';
  g.font = '24px monospace';
  g.fillText(fmt(mx), 8, 28);
  g.fillText(fmt(mn), 8, h - 10);
}
async function poll() {
  try {
    const hh = await fetch('/api/history').then(r => r.json());
    drawChart('ch_off', [
      {data: hh.offset.map(v => v / 1000), color: '#fd0'},
      {data: hh.delay.map(v => v / 1000), color: '#6cf'}
    ], v => v.toFixed(1) + ' µs', true);
    drawChart('ch_rate', [
      {data: hh.rates.sync, color: '#6c6'},
      {data: hh.rates.fup, color: '#6cf'},
      {data: hh.rates.ann, color: '#fd0'},
      {data: hh.rates.dresp, color: '#f6c'}
    ], v => Math.round(v) + '/s', true);
    const last = a => a.length ? a[a.length - 1] : 0;
    document.getElementById('rate_now').textContent =
        'now: ' + last(hh.rates.sync) + '/' + last(hh.rates.fup) + '/' +
        last(hh.rates.ann) + '/' + last(hh.rates.dresp);
    const s = await fetch('/api/status').then(r => r.json());
    const cw = document.getElementById('cmp_wrap');
    if (hh.cmp && hh.cmp.length) {
      cw.style.display = '';
      drawChart('ch_cmp', [{data: hh.cmp.map(v => v / 1000), color: '#f96'}],
                v => v.toFixed(1) + ' µs', true);
      document.getElementById('cmp_now').textContent = s.cmp_valid
          ? 'mean: ' + (s.cmp_ns / 1000).toFixed(1) + ' µs vs ' + s.cmp_gm
          : '';
    } else {
      cw.style.display = 'none';
    }
    document.getElementById('ts_mode').innerHTML = s.hwts
        ? '<span style="color:#6c6">&#9679;</span> hardware timestamping ('
          + s.hwts_desc + ') &mdash; t2/t3 stamped by the NIC, clock reads '
          + 'the PHC'
        : '<span style="color:#fd0">&#9679;</span> software timestamping '
          + '&mdash; measurements include OS scheduling jitter';
  } catch (e) {}
}
poll();
setInterval(poll, 2000);
window.addEventListener('resize', poll);
</script>
</body>
</html>
)ANA";

static const char *kClockHtml = R"CLOCK(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PTP Clock</title>
<link rel="icon" href="/favicon.svg" type="image/svg+xml">
<style>
 :root { --c: #ffff00; }
 html, body { height: 100%; margin: 0; background: #000; }
 body { display: flex; align-items: center; justify-content: center;
        overflow: hidden; cursor: pointer; user-select: none;
        -webkit-user-select: none;
        font-family: ui-monospace, "SF Mono", Menlo, Consolas,
                     "DejaVu Sans Mono", monospace; }
 #main { text-align: center; transition: opacity 0.6s; }
 #time { font-size: min(8.5vw, 55vh); font-weight: 700; line-height: 1.1;
         white-space: nowrap; color: var(--c);
         text-shadow: 0 0 0.08em var(--c), 0 0 0.45em var(--c); }
 #time .sup { font-size: 0.3em; vertical-align: 0.4em; margin-left: 0.4em;
         opacity: 0.85; }
 #zone { margin-top: 1.2vw; font-size: min(2.6vw, 7.5vh);
         letter-spacing: 0.3em; color: var(--c); opacity: 0.75; }
 #date { margin-top: 1vw; font-size: min(2.4vw, 7vh);
         letter-spacing: 0.25em; color: var(--c); opacity: 0.55; }
 #alert { display: none; margin-top: 2.2vw; font-size: min(2.8vw, 8vh);
         font-weight: 700; color: #f43;
         text-shadow: 0 0 0.5em #f43; animation: pulse 1s infinite; }
 @keyframes pulse { 50% { opacity: 0.35; } }
 #footer { position: fixed; bottom: 2.5vh; left: 0; right: 0;
         text-align: center; font-size: min(1.15vw, 3.6vh);
         letter-spacing: 0.03em; color: #555; }
 #bo { display: none; position: fixed; bottom: 2.5vh; right: 2vw;
         color: #222; font-size: 1.4vw; }
 #cfg { position: fixed; top: 2.5vh; right: 2vw; color: #2a2a2a;
         font-size: 1.6em; text-decoration: none; }
 #cfg:hover { color: #9ab; }
 /* Small windows: hide date + status line and let the time take the room */
 @media (max-width: 480px), (max-height: 300px) {
   #zone, #date, #footer { display: none; }
   #time { font-size: min(8.5vw, 72vh); }
 }
</style>
</head>
<body>
<div id="main">
 <div id="time">--:--:--</div>
 <div id="zone"></div>
 <div id="date"></div>
 <div id="alert"></div>
</div>
<div id="footer"></div>
<div id="bo">blackout</div>
<a id="cfg" href="/" title="Settings">&#9881;</a>

<script>
const CLS = {6:'GNSS locked',7:'holdover',13:'application specific',
 52:'degraded A',187:'degraded B',248:'default',255:'slave-only'};
const ACC = {0x20:'25 ns',0x21:'100 ns',0x22:'250 ns',0x23:'1 µs',
 0x24:'2.5 µs',0x25:'10 µs',0x26:'25 µs',0x27:'100 µs',0x28:'250 µs',
 0x29:'1 ms',0x2A:'2.5 ms',0x2B:'10 ms',0x2C:'25 ms',0x2D:'100 ms',
 0x2E:'250 ms',0x2F:'1 s',0x30:'10 s',0x31:'>10 s'};
const TSRC = {0x10:'atomic clock',0x20:'GPS/GNSS',0x30:'terrestrial radio',
 0x40:'PTP',0x50:'NTP',0x60:'hand set',0x90:'other',
 0xA0:'internal oscillator'};

// Fractional digits with a per-position speed ladder (same as the LED):
// the tenths digit is real, every further digit visibly changes faster
// than the one before it — sample-and-hold with a deterministic hash.
const FRAC_HOLD = [1e8, 6.6e7, 4.36e7, 2.87e7, 1.9e7,
                   1.25e7, 8.3e6, 5.5e6, 3.6e6];
function fracDigits(tNs) {
  let s = String(Math.floor(tNs / 1e8) % 10);
  for (let k = 1; k < 9; k++) {
    const idx = Math.floor(tNs / FRAC_HOLD[k]) >>> 0;
    const v = (Math.imul(idx, 2654435761) + (k + 1) * 40503) >>> 0;
    s += (v >>> 16) % 10;
  }
  return s;
}

let S = null;          // settings (mode, formats, color, notify)
let st = null;         // last status
let base = null;       // extrapolation base

async function loadSettings() {
  try {
    S = await fetch('/api/settings').then(r => r.json());
    document.documentElement.style.setProperty('--c', S.color);
  } catch (e) {}
}

function updateFooter(s) {
  const a = document.getElementById('alert');
  // Persistent error when the elected GM is not on the acceptable list;
  // the transient NEW GM alert comes second.
  if (s.gm_id && s.gm_accepted === false) {
    a.style.display = 'block';
    a.textContent = '! UNACCEPTED GM !  ' + s.gm_id;
  } else {
    const showAlert = S && S.notify_gm_change &&
        s.seconds_since_change >= 0 && s.seconds_since_change < 10;
    a.style.display = showAlert ? 'block' : 'none';
    if (showAlert) a.textContent = '! NEW GM !  ' + s.gm_id;
  }

  const f = document.getElementById('footer');
  if (!s.gm_id) {
    f.textContent = 'waiting for a PTP master...';
    return;
  }
  const dom = s.domain === -1
      ? (s.active_domain >= 0 ? s.active_domain + ' (auto)' : 'auto')
      : s.domain;
  f.textContent = 'GM ' + s.gm_id +
      ' · priority ' + s.priority1 + '/' + s.priority2 +
      ' · class ' + s.clock_class +
      (CLS[s.clock_class] ? ' (' + CLS[s.clock_class] + ')' : '') +
      ' · accuracy ' + (ACC[s.clock_accuracy] || 'unknown') +
      ' · source ' + (TSRC[s.time_source] || 'unknown') +
      ' · domain ' + dom;
}

async function poll() {
  try {
    const s = await fetch('/api/status').then(r => r.json());
    st = s;
    if (s.tai_sec < 0) {
      base = null;
    } else {
      const perf = performance.now();
      const cand = { sec: s.tai_sec, nsec: s.tai_nsec, perf,
                     off: s.utc_offset,
                     val: s.tai_sec * 1000 + s.tai_nsec / 1e6 - perf };
      if (!base || cand.val > base.val || perf - base.perf > 30000) {
        base = cand;
      } else {
        base.off = s.utc_offset;
      }
    }
    updateFooter(s);
  } catch (e) {}
}

function render() {
  const el = document.getElementById('time');
  const ez = document.getElementById('zone');
  const ed = document.getElementById('date');
  const bl = st && st.blackout;
  document.getElementById('main').style.opacity =
      bl ? 0 : st ? 0.12 + 0.88 * st.brightness / 100 : 1;
  document.getElementById('footer').style.opacity = bl ? 0 : 1;
  document.getElementById('bo').style.display = bl ? 'block' : 'none';
  if (!base || !S) {
    el.textContent = '--:--:--';
    ez.textContent = '';
    ed.textContent = st && !st.have_ptp ? 'WAITING FOR PTP' : '';
    return;
  }
  const df = S.date_format;

  const elapsed = performance.now() - base.perf;
  const totalNs = base.nsec + elapsed * 1e6;
  let sec = base.sec + Math.floor(totalNs / 1e9);
  // Reduced TAI ns (mod ~1 day) keeps the math exact in doubles
  const fr9 = fracDigits((base.sec % 100000) * 1e9 + totalNs);

  // Active clock line, same PTP-aligned rotation as the LED display
  const list = (S.clocks && S.clocks.length)
      ? S.clocks : [{zone: 'UTC', style: '24h', name: ''}];
  const cl = list[list.length > 1 ? Math.floor(sec / 4) % list.length : 0];
  const tz = (cl.zone === 'UTC' || cl.zone === 'TAI' || !cl.zone)
      ? 'UTC' : cl.zone;
  if (cl.zone !== 'TAI') sec -= base.off;
  const d = new Date(sec * 1000);
  let p;
  try {
    p = new Intl.DateTimeFormat('en-GB', { timeZone: tz, hourCycle: 'h23',
        hour: '2-digit', minute: '2-digit', second: '2-digit',
        weekday: 'short', day: '2-digit', month: '2-digit', year: 'numeric',
        timeZoneName: 'short'
      }).formatToParts(d).reduce((a, x) => (a[x.type] = x.value, a), {});
  } catch (e) {
    el.textContent = '--:--:--';
    ez.textContent = '';
    ed.textContent = 'UNKNOWN TIME ZONE';
    return;
  }
  // Body per style (pixel styles fall back to digital 24h)
  let sup = '', body;
  if (cl.style === 'unix') {
    body = sec + '.' + fr9;
  } else if (cl.style === '12h') {
    let h = parseInt(p.hour, 10);
    sup = h >= 12 ? 'PM' : 'AM';
    h = h % 12 || 12;
    body = String(h).padStart(2, '0') + ':' + p.minute + ':' +
        p.second + '.' + fr9;
  } else {
    body = p.hour + ':' + p.minute + ':' + p.second + '.' + fr9;
  }
  el.innerHTML = body +
      (sup ? '<span class="sup">' + sup + '</span>' : '');
  const nm = cl.name || '';
  ez.textContent = nm === '%Z'
      ? (cl.zone === 'TAI' ? 'TAI' : (p.timeZoneName || '')) : nm;
  const wd = p.weekday.toUpperCase();
  ed.textContent = df === 'iso'
      ? wd + ' ' + p.year + '-' + p.month + '-' + p.day
      : df === 'mdy'
        ? wd + ' ' + p.month + '/' + p.day + '/' + p.year
        : wd + ' ' + p.day + '.' + p.month + '.' + p.year;
}

document.body.addEventListener('click', () => {
  if (document.fullscreenElement) document.exitFullscreen();
  else document.documentElement.requestFullscreen().catch(() => {});
});
// The settings link must not toggle fullscreen
document.getElementById('cfg').addEventListener('click',
    (e) => e.stopPropagation());

(function tick() { render(); requestAnimationFrame(tick); })();
loadSettings();
poll();
setInterval(poll, 2000);
setInterval(loadSettings, 10000);
</script>
</body>
</html>
)CLOCK";

static void send_response(int fd, const char *status,
                          const char *content_type, const std::string &body) {
    std::ostringstream resp;
    resp << "HTTP/1.1 " << status << "\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Cache-Control: no-store\r\n"
         << "Connection: close\r\n\r\n"
         << body;
    std::string s = resp.str();
    send(fd, s.data(), s.size(), 0);
}

static void handle_client(int fd) {
    struct timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string req;
    char buf[4096];
    size_t body_start = std::string::npos;
    // Read until end of headers
    while (req.size() < 65536) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        req.append(buf, n);
        body_start = req.find("\r\n\r\n");
        if (body_start != std::string::npos) {
            body_start += 4;
            break;
        }
    }
    if (body_start == std::string::npos) {
        close(fd);
        return;
    }

    std::istringstream head(req.substr(0, body_start));
    std::string method, path;
    head >> method >> path;

    // Read remaining body per Content-Length
    size_t content_length = 0;
    std::string lower = req.substr(0, body_start);
    for (auto &c : lower) c = tolower(c);
    size_t cl = lower.find("content-length:");
    if (cl != std::string::npos)
        content_length = strtoul(lower.c_str() + cl + 15, nullptr, 10);
    while (req.size() - body_start < content_length && req.size() < 65536) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        req.append(buf, n);
    }
    std::string body = req.substr(body_start);

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        send_response(fd, "200 OK", "text/html; charset=utf-8", kIndexHtml);
    } else if (method == "GET" && path == "/clock") {
        send_response(fd, "200 OK", "text/html; charset=utf-8", kClockHtml);
    } else if (method == "GET" && path == "/analysis") {
        send_response(fd, "200 OK", "text/html; charset=utf-8",
                      kAnalysisHtml);
    } else if (method == "GET" &&
               (path == "/favicon.svg" || path == "/favicon.ico")) {
        send_response(fd, "200 OK", "image/svg+xml", kFaviconSvg);
    } else if (method == "GET" && path == "/api/settings") {
        send_response(fd, "200 OK", "application/json", settings_json());
    } else if (method == "GET" && path == "/api/status") {
        send_response(fd, "200 OK", "application/json", status_json());
    } else if (method == "GET" && path == "/api/history") {
        send_response(fd, "200 OK", "application/json", history_json());
    } else if (method == "POST" && path == "/api/settings") {
        auto kv = parse_form(body);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (kv.count("color")) {
                unsigned r, g, b;
                if (sscanf(kv["color"].c_str(), "#%02x%02x%02x", &r, &g, &b) == 3) {
                    g_settings.r = r; g_settings.g = g; g_settings.b = b;
                }
            }
            if (kv.count("brightness")) {
                int v = atoi(kv["brightness"].c_str());
                if (v >= 1 && v <= 100)
                    g_settings.brightness = v;
            }
            if (kv.count("blackout"))
                g_settings.blackout = (kv["blackout"] == "1");
            if (kv.count("rotate180"))
                g_settings.rotate180 = (kv["rotate180"] == "1");
            if (kv.count("show_gm"))
                g_settings.show_gm = (kv["show_gm"] == "1");
            if (kv.count("show_gm_details"))
                g_settings.show_gm_details = (kv["show_gm_details"] == "1");
            if (kv.count("show_date"))
                g_settings.show_date = (kv["show_date"] == "1");
            if (kv.count("mode")) {
                if (kv["mode"] == "tai") g_settings.mode = MODE_TAI;
                else if (kv["mode"] == "local") g_settings.mode = MODE_LOCAL;
                else if (kv["mode"] == "cycle") g_settings.mode = MODE_CYCLE;
                else g_settings.mode = MODE_UTC;
            }
            if (kv.count("timezone") && !kv["timezone"].empty()) {
                g_settings.timezone = kv["timezone"];
                apply_timezone(g_settings.timezone);
            }
            if (kv.count("tz_label") && kv["tz_label"].size() < 64)
                g_settings.tz_label = kv["tz_label"];
            if (kv.count("utc_label") && kv["utc_label"].size() < 64)
                g_settings.utc_label = kv["utc_label"];
            if (kv.count("tai_label") && kv["tai_label"].size() < 64)
                g_settings.tai_label = kv["tai_label"];
            if (kv.count("show_zone"))
                g_settings.show_zone = (kv["show_zone"] == "1");
            if (kv.count("clocks") && kv["clocks"].size() < 2048) {
                std::vector<ClockEntry> v =
                    clocks_parse(kv["clocks"], g_settings.timezone);
                if (!v.empty())
                    g_settings.clocks = v;
            }
            if (kv.count("time_format")) {
                if (kv["time_format"] == "12" || kv["time_format"] == "24")
                    g_settings.time_format = atoi(kv["time_format"].c_str());
            }
            if (kv.count("date_format")) {
                if (kv["date_format"] == "iso")
                    g_settings.date_format = DATE_ISO;
                else if (kv["date_format"] == "mdy")
                    g_settings.date_format = DATE_MDY;
                else if (kv["date_format"] == "dmy")
                    g_settings.date_format = DATE_DMY;
            }
            if (kv.count("domain")) {
                int d = atoi(kv["domain"].c_str());  // -1 = auto
                if (d >= -1 && d <= 255 && d != g_settings.domain) {
                    g_settings.domain = d;
                    g_domain = d;
                    g_reset_ptp = true;   // main loop clears PTP state
                }
            }
            if (kv.count("iface") && !kv["iface"].empty() &&
                kv["iface"].size() < 16 &&    // IFNAMSIZ
                kv["iface"] != g_settings.iface) {
                g_settings.iface = kv["iface"];
                g_iface_changed = true;   // main loop rejoins multicast
            }
            if (kv.count("acceptable_gms") &&
                kv["acceptable_gms"].size() < 4096)
                g_settings.acceptable_gms = kv["acceptable_gms"];
            if (kv.count("gm_enable"))
                g_settings.gm_enable = (kv["gm_enable"] == "1");
            if (kv.count("gm_serial") && !kv["gm_serial"].empty() &&
                kv["gm_serial"].size() < 64)
                g_settings.gm_serial = kv["gm_serial"];
            if (kv.count("gm_pps") && !kv["gm_pps"].empty() &&
                kv["gm_pps"].size() < 64)
                g_settings.gm_pps = kv["gm_pps"];
            if (kv.count("gm_prio1")) {
                int v = atoi(kv["gm_prio1"].c_str());
                if (v >= 0 && v <= 255)
                    g_settings.gm_prio1 = v;
            }
            if (kv.count("gm_prio2")) {
                int v = atoi(kv["gm_prio2"].c_str());
                if (v >= 0 && v <= 255)
                    g_settings.gm_prio2 = v;
            }
            if (kv.count("gm_utc_offset")) {
                int v = atoi(kv["gm_utc_offset"].c_str());
                if (v >= 0 && v <= 99)
                    g_settings.gm_utc_offset = v;
            }
            if (kv.count("notify"))
                g_settings.notify_gm_change = (kv["notify"] == "1");
            save_settings_locked();
        }
        send_response(fd, "200 OK", "application/json", "{\"ok\":true}");
    } else {
        send_response(fd, "404 Not Found", "text/plain", "not found");
    }
    close(fd);
}

static void http_server_thread(int port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("http socket");
        return;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("http bind");
        close(listen_fd);
        return;
    }
    listen(listen_fd, 8);
    std::cout << "Web interface on http://0.0.0.0:" << port << "\n";

    while (!interrupt_received) {
        struct pollfd pfd{listen_fd, POLLIN, 0};
        int r = poll(&pfd, 1, 500);
        if (r <= 0)
            continue;
        int client = accept(listen_fd, nullptr, nullptr);
        if (client >= 0)
            handle_client(client);
    }
    close(listen_fd);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    // Under systemd stdout is a block-buffered pipe — without this the
    // startup messages only reach journald when the process exits
    std::cout << std::unitbuf;
    resolve_config_path();
    // Container/first-run convenience: PTP_WALLCLOCK_IFACE sets the default
    // interface; a saved setting from the web UI still wins.
    const char *env_iface = getenv("PTP_WALLCLOCK_IFACE");
    if (env_iface && *env_iface)
        g_settings.iface = env_iface;
    load_settings();
    apply_timezone(g_settings.timezone);

    // --- Multicast sockets ---
    int sock_sync = socket(AF_INET, SOCK_DGRAM, 0);
    int sock_general = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_sync < 0 || sock_general < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in addr_sync{}, addr_general{};
    addr_sync.sin_family = AF_INET;
    addr_sync.sin_port = htons(319);
    addr_sync.sin_addr.s_addr = htonl(INADDR_ANY);

    addr_general.sin_family = AF_INET;
    addr_general.sin_port = htons(320);
    addr_general.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock_sync, (struct sockaddr*)&addr_sync, sizeof(addr_sync)) < 0 ||
        bind(sock_general, (struct sockaddr*)&addr_general,
             sizeof(addr_general)) < 0) {
        perror("bind PTP port");
        std::cerr <<
            "PTP uses the privileged UDP ports 319/320.\n"
            "Either run as root (sudo ./ptp-clock), or allow it once with:\n"
            "  sudo setcap cap_net_bind_service+ep ./ptp-clock\n";
        return 1;
    }

    // Hardware timestamping setup needs privileges — do it now, before
    // the matrix library drops them
    if (g_settings.hwts)
        try_enable_hw_timestamping(sock_sync);
    else
        std::cout << "PTP hardware timestamping disabled (hwts=0)\n";

    // Non-blocking: with SO_TIMESTAMPING a pending TX timestamp on the
    // error queue can mark the socket readable without data — a blocking
    // recv would then hang the main loop
    fcntl(sock_sync, F_SETFL, O_NONBLOCK);
    fcntl(sock_general, F_SETFL, O_NONBLOCK);

    // Grandmaster mode: open the GNSS devices while still privileged
    if (g_settings.gm_enable)
        gnss_open_initial();
    std::thread(gnss_thread).detach();

#ifndef NO_MATRIX
    // --- Matrix options ---
    RGBMatrix::Options matrix_options;
    RuntimeOptions runtime_opt;

    matrix_options.rows = 32;
    matrix_options.cols = 128;
    matrix_options.chain_length = 1;
    matrix_options.hardware_mapping = "adafruit-hat";
    matrix_options.brightness = g_settings.brightness;
    runtime_opt.gpio_slowdown = 2;

    RGBMatrix *matrix = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
    if (!matrix) {
        std::cerr << "Failed to initialize RGB matrix\n";
        return 1;
    }

    FrameCanvas *offscreen = matrix->CreateFrameCanvas();
    FlipCanvas flip_canvas;               // optional 180-degree rotation
    ClipCanvas clip_canvas;               // for the flip-clock animation

    // Flip-clock roll state (per character cell)
    size_t flip_entry_idx = (size_t)-1;
    bool flip_init = false;
    char flip_prev[9] = {0}, flip_old[9] = {0};
    uint64_t flip_change[9] = {0};

    // --- Fonts ---
    Font font;
    const char* font_path = "/usr/share/fonts/rpi-rgb-led-matrix/6x13B.bdf";
    if (!font.LoadFont(font_path)) {
        std::cerr << "Couldn't load font " << font_path << "\n";
        delete matrix;
        return 1;
    }

    // Small font for the second line (grandmaster ID / details)
    Font small_font;
    bool have_small_font = small_font.LoadFont(
        "/usr/share/fonts/rpi-rgb-led-matrix/4x6.bdf");
    if (!have_small_font)
        std::cerr << "4x6.bdf not found, second display line disabled\n";
#else
    std::cout << "Headless mode: the clock display is at /clock "
                 "on the web interface\n";
#endif

    signal(SIGINT, InterruptHandler);
    signal(SIGTERM, InterruptHandler);

    // Sending options for Delay_Req (the outgoing interface is set when
    // joining the multicast group)
    unsigned char ttl = 1, loop = 0;
    setsockopt(sock_sync, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(sock_sync, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    sockaddr_in dreq_dst{};
    dreq_dst.sin_family = AF_INET;
    dreq_dst.sin_port = htons(319);
    dreq_dst.sin_addr.s_addr = inet_addr("224.0.1.129");

    sockaddr_in gen_dst{};                // general messages (port 320)
    gen_dst.sin_family = AF_INET;
    gen_dst.sin_port = htons(320);
    gen_dst.sin_addr.s_addr = inet_addr("224.0.1.129");

    std::cout << "Listening for PTP messages on " << g_settings.iface
              << " (domain ";
    if (g_settings.domain < 0)
        std::cout << "auto";
    else
        std::cout << g_settings.domain;
    std::cout << ")\n";

    // --- Webserver ---
    std::thread http_thread(http_server_thread, g_settings.http_port);

    uint64_t last_dreq_ns = 0;
    uint64_t last_gm_tx_ns = 0;
    uint64_t last_bmca_ns = 0;
    struct JoinedIface { std::string name; uint32_t ip; };
    std::vector<JoinedIface> joined;      // multicast memberships we hold
    bool identity_set = false;
    uint64_t next_join_check_ns = 0;
    bool join_warned = false;

    // --- Main loop ---
    while (!interrupt_received) {

        if (g_reset_ptp.exchange(false))
            reset_ptp_state();            // e.g. domain changed via web UI

        if (g_iface_changed.exchange(false)) {
            for (const auto &j : joined)
                membership(sock_sync, sock_general, j.ip, IP_DROP_MEMBERSHIP);
            joined.clear();
            identity_set = false;
            g_iface_up = false;
            reset_ptp_state();
            next_join_check_ns = 0;
            join_warned = false;
        }

        // Keep the multicast memberships in sync with the configured
        // interface ("auto" = every interface with an IPv4 address).
        // Re-checked every 5 s so late interfaces (DHCP at boot, hotplug)
        // are picked up automatically.
        if (mono_ns() >= next_join_check_ns) {
            next_join_check_ns = mono_ns() + 5000000000ULL;
            std::string want;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                want = g_settings.iface;
            }
            std::vector<std::string> targets;
            if (want == "auto")
                targets = list_ifaces();
            else
                targets.push_back(want);

            // Drop memberships that are gone or no longer wanted
            for (auto it = joined.begin(); it != joined.end();) {
                bool keep = false;
                for (const auto &t : targets)
                    if (t == it->name && get_iface_ip(t) == it->ip)
                        keep = true;
                if (!keep) {
                    membership(sock_sync, sock_general, it->ip,
                               IP_DROP_MEMBERSHIP);
                    std::cout << "PTP multicast left on " << it->name << "\n";
                    it = joined.erase(it);
                } else {
                    ++it;
                }
            }
            // Join new ones
            for (const auto &t : targets) {
                bool have = false;
                for (const auto &j : joined)
                    if (j.name == t)
                        have = true;
                if (have)
                    continue;
                uint32_t ip = get_iface_ip(t);
                if (ip == 0)
                    continue;
                membership(sock_sync, sock_general, ip, IP_ADD_MEMBERSHIP);
                joined.push_back({t, ip});
                if (!identity_set) {
                    init_clock_identity(sock_sync, t);
                    identity_set = true;
                }
                std::cout << "PTP multicast joined on " << t
                          << ", clock identity " << format_gm(g_clock_id)
                          << "\n";
            }
            g_iface_up = !joined.empty();
            if (joined.empty()) {
                if (!join_warned) {
                    std::cerr << "No usable interface ('" << want
                              << "'), retrying every 5 s (configurable in "
                                 "the web UI)\n";
                    join_warned = true;
                }
            } else {
                join_warned = false;
            }
            std::string names;
            for (const auto &j : joined)
                names += (names.empty() ? "" : ", ") + j.name;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_joined_names = names;
            }
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock_sync, &fds);
        FD_SET(sock_general, &fds);

        struct timeval tv{0, 20000}; // 20 ms
        select(std::max(sock_sync, sock_general) + 1,
               &fds, nullptr, nullptr, &tv);

        // --- SYNC / Delay_Req (port 319) ---
        if (FD_ISSET(sock_sync, &fds)) {
            uint8_t buf[128];
            ssize_t len = 0;
            uint64_t ts = recv_event_packet(sock_sync, buf, sizeof(buf), &len);
            if (len >= 44 && (buf[0] & 0x0F) == 0x01) {
                // Delay_Req from a client: answer when we are the GM
                if (g_role.load() == ROLE_GM_ACTIVE && (buf[1] & 0x0F) == 2 &&
                    domain_ok(buf[4], false) && ts != 0) {
                    uint8_t resp[54];
                    build_delay_resp(resp, buf, (int64_t)ts - g_offset_ns);
                    for (const auto &j : joined) {
                        in_addr mif{};
                        mif.s_addr = j.ip;
                        setsockopt(sock_general, IPPROTO_IP, IP_MULTICAST_IF,
                                   &mif, sizeof(mif));
                        sendto(sock_general, resp, sizeof(resp), 0,
                               (struct sockaddr *)&gen_dst, sizeof(gen_dst));
                    }
                }
            } else if (len > 0) {
                process_event_packet(buf, len, ts);
            }
        }

        // --- ANNOUNCE / FOLLOW_UP / DELAY_RESP (port 320) ---
        if (FD_ISSET(sock_general, &fds)) {
            uint8_t buf[128];
            ssize_t len = recv(sock_general, buf, sizeof(buf), 0);
            if (len > 0)
                process_general_packet(buf, len, mono_ns());
        }

        uint64_t now_ns = mono_ns();

        // --- GNSS time sample (grandmaster mode): disciplines the clock ---
        {
            bool have = false;
            int64_t t1 = 0, t2 = 0;
            {
                std::lock_guard<std::mutex> lk(g_gnss_mutex);
                if (g_gnss_sample_valid) {
                    have = true;
                    t1 = g_gnss_t1;
                    t2 = g_gnss_t2;
                    g_gnss_sample_valid = false;
                }
            }
            if (have)
                complete_sync_pair(t1, t2, false);
        }

        // --- Auto domain: rescan when Announce stops for 15 s ---
        if (g_domain.load() < 0 && g_active_domain.load() >= 0) {
            unsigned long long last_ann = g_last_announce_mono_ns.load();
            if (last_ann > 0 && now_ns - last_ann > 15000000000ULL) {
                std::cout << "PTP domain lost, rescanning...\n";
                reset_ptp_state();
            }
        }

        // --- BMCA housekeeping + per-second analysis tick ---
        if (now_ns - last_bmca_ns >= 1000000000ULL) {
            last_bmca_ns = now_ns;
            std::lock_guard<std::mutex> lock(g_mutex);

            // Grandmaster mode: keep our own candidacy in the master
            // list so the ordinary BMCA decides between us and the net
            uint64_t smp = g_gnss_last_sample.load();
            bool gnss_ok = smp != 0 && now_ns - smp < 10000000000ULL;
            bool holdover = !gnss_ok && smp != 0 &&
                            now_ns - smp < 300000000000ULL;
            int role = ROLE_CLIENT;
            if (g_settings.gm_enable) {
                role = ROLE_GM_WAIT;
                if (gnss_ok || holdover) {
                    if (g_domain.load() < 0 && g_active_domain.load() < 0) {
                        g_active_domain = 0;   // we define the domain now
                        std::cout << "Grandmaster mode: using domain 0\n";
                    }
                    GMInfo self;
                    memcpy(self.id, g_clock_id, 8);
                    self.priority1 = (uint8_t)g_settings.gm_prio1;
                    self.priority2 = (uint8_t)g_settings.gm_prio2;
                    self.clock_class = gnss_ok ? 6 : 7;    // GNSS/holdover
                    self.clock_accuracy = !gnss_ok ? 0xFE
                                        : g_hwts ? 0x22 : 0x24;
                    self.variance = 0xFFFF;
                    self.steps_removed = 0;
                    self.time_source = 0x20;               // GPS
                    uint8_t sender[10];
                    memcpy(sender, g_clock_id, 8);
                    sender[8] = 0;
                    sender[9] = 1;
                    ForeignMaster *fm = nullptr;
                    for (auto &m : g_masters)
                        if (memcmp(m.sender, sender, 10) == 0) {
                            fm = &m;
                            break;
                        }
                    if (!fm) {
                        g_masters.push_back(ForeignMaster{});
                        fm = &g_masters.back();
                        memcpy(fm->sender, sender, 10);
                    }
                    fm->gm = self;
                    fm->last_seen = now_ns;
                    fm->timeout_ns = 3500000000ULL;
                    g_last_announce_mono_ns = now_ns;  // no auto rescan
                }
            }

            bmca_elect_locked(now_ns);

            if (role >= ROLE_GM_WAIT && (gnss_ok || holdover)) {
                bool self_elected = g_have_master &&
                    memcmp(g_elected_sender, g_clock_id, 8) == 0 &&
                    g_elected_sender[8] == 0 && g_elected_sender[9] == 1;
                role = self_elected ? ROLE_GM_ACTIVE : ROLE_GM_PASSIVE;
                current_utc_offset = (int16_t)g_settings.gm_utc_offset;

                // Comparison target: the best FOREIGN master still
                // sending — its Syncs are measured against GNSS
                int best = -1;
                for (size_t i = 0; i < g_masters.size(); ++i) {
                    if (memcmp(g_masters[i].sender + 0, g_clock_id, 8) == 0)
                        continue;         // that's us
                    if (best < 0 ||
                        compare_datasets(g_masters[i].gm,
                                         g_masters[best].gm) < 0)
                        best = (int)i;
                }
                if (best >= 0) {
                    if (!g_cmp_target_valid.load() ||
                        memcmp(g_cmp_target, g_masters[best].sender,
                               10) != 0) {
                        memcpy(g_cmp_target, g_masters[best].sender, 10);
                        g_cmp_have = false;        // new reference: restart
                        g_cmp_count = 0;
                        g_cmp_target_valid = true;
                    }
                    snprintf(g_cmp_gm_str, sizeof(g_cmp_gm_str), "%s",
                             format_gm(g_masters[best].gm.id).c_str());
                } else {
                    g_cmp_target_valid = false;
                    g_cmp_gm_str[0] = 0;
                }
            } else {
                g_cmp_target_valid = false;
                g_cmp_have = false;
                g_cmp_gm_str[0] = 0;
            }
            g_role = role;
            g_gnss_lock = gnss_ok;

            g_hist_rate[g_hist_rate_i] = {
                (uint16_t)g_cnt_sync.exchange(0),
                (uint16_t)g_cnt_fup.exchange(0),
                (uint16_t)g_cnt_ann.exchange(0),
                (uint16_t)g_cnt_dresp.exchange(0)};
            g_hist_rate_i = (g_hist_rate_i + 1) % kHistN;
            if (g_hist_rate_n < kHistN)
                g_hist_rate_n++;
        }

        // --- Delay_Req once per second, out of every joined interface
        //     (only the elected master answers; matched by our identity).
        //     In GNSS mode it measures the path to the comparison master ---
        if (g_have_pair && !joined.empty() &&
            (g_role.load() < ROLE_GM_PASSIVE || g_cmp_target_valid.load()) &&
            now_ns - last_dreq_ns >= 1000000000ULL) {
            uint8_t pkt[44];
            build_delay_req(pkt, ++g_dreq_seq);
#ifdef __linux__
            if (g_hwts)
                drain_errqueue(sock_sync);
#endif
            uint64_t t_a = mono_ns();
            int sent_ok = 0;
            for (const auto &j : joined) {
                in_addr mif{};
                mif.s_addr = j.ip;
                setsockopt(sock_sync, IPPROTO_IP, IP_MULTICAST_IF,
                           &mif, sizeof(mif));
                if (sendto(sock_sync, pkt, sizeof(pkt), 0,
                           (struct sockaddr*)&dreq_dst,
                           sizeof(dreq_dst)) == (ssize_t)sizeof(pkt))
                    sent_ok++;
            }
            uint64_t t_b = mono_ns();
            if (sent_ok > 0) {
                uint64_t t3 = 0;
#ifdef __linux__
                if (g_hwts)
                    // PHC send time from the error queue; if the NIC gives
                    // none, skip this round (never mix in software times)
                    t3 = fetch_tx_timestamp(sock_sync);
                else
#endif
                    t3 = t_a + (t_b - t_a) / 2;
                if (t3) {
                    g_pending_dreq.valid = true;
                    g_pending_dreq.seq = g_dreq_seq;
                    g_pending_dreq.t3_mono = t3;
                    g_dreq_sent++;
                }
            }
            last_dreq_ns = now_ns;
        }

        // --- Grandmaster TX: Announce + two-step Sync/Follow_Up at 1 Hz ---
        if (g_role.load() == ROLE_GM_ACTIVE && !joined.empty() &&
            now_ns - last_gm_tx_ns >= 1000000000ULL) {
            last_gm_tx_ns = now_ns;
            GMInfo self;
            int16_t utc_off;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                self = g_gm;              // our own dataset — we are elected
                utc_off = (int16_t)g_settings.gm_utc_offset;
            }
            uint8_t ann[64], sync[44], fup[44];
            build_announce(ann, ++g_ann_seq, self, utc_off,
                           self.clock_class == 6);
            ++g_sync_seq;
            for (const auto &j : joined) {
                in_addr mif{};
                mif.s_addr = j.ip;
                setsockopt(sock_sync, IPPROTO_IP, IP_MULTICAST_IF,
                           &mif, sizeof(mif));
                setsockopt(sock_general, IPPROTO_IP, IP_MULTICAST_IF,
                           &mif, sizeof(mif));
                sendto(sock_general, ann, sizeof(ann), 0,
                       (struct sockaddr *)&gen_dst, sizeof(gen_dst));
#ifdef __linux__
                if (g_hwts)
                    drain_errqueue(sock_sync);
#endif
                uint64_t ta = local_clock_ns();
                build_sync(sync, g_sync_seq, (int64_t)ta - g_offset_ns);
                if (sendto(sock_sync, sync, sizeof(sync), 0,
                           (struct sockaddr *)&dreq_dst,
                           sizeof(dreq_dst)) != (ssize_t)sizeof(sync))
                    continue;
                uint64_t tb = local_clock_ns();
                uint64_t t1 = 0;
#ifdef __linux__
                if (g_hwts)
                    t1 = fetch_tx_timestamp(sock_sync);
#endif
                if (!t1)
                    t1 = ta + (tb - ta) / 2;
                build_follow_up(fup, g_sync_seq, (int64_t)t1 - g_offset_ns);
                sendto(sock_general, fup, sizeof(fup), 0,
                       (struct sockaddr *)&gen_dst, sizeof(gen_dst));
            }
        }

#ifndef NO_MATRIX
        // Snapshot the settings + GM info for this frame
        Settings s;
        std::string id_line, detail_line;
        bool gm_recent_change = false;
        bool gm_unaccepted = false;
        // Analysis history copies (oldest first) for the graph styles
        int32_t hoff[kHistN], hdel[kHistN];
        uint16_t hrate[4][kHistN];
        int hoff_n = 0, hdel_n = 0, hrate_n = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            s = g_settings;
            bool want_hist = false;
            for (const auto &ce : s.clocks)
                if (ce.style == "graph" || ce.style == "rates")
                    want_hist = true;
            if (want_hist) {
                auto cp = [](const int32_t *buf, int n, int i, int32_t *dst) {
                    for (int k = 0; k < n; ++k)
                        dst[k] = buf[(i - n + k + 2 * kHistN) % kHistN];
                    return n;
                };
                hoff_n = cp(g_hist_off, g_hist_off_n, g_hist_off_i, hoff);
                hdel_n = cp(g_hist_del, g_hist_del_n, g_hist_del_i, hdel);
                hrate_n = g_hist_rate_n;
                for (int k = 0; k < hrate_n; ++k) {
                    int idx = (g_hist_rate_i - hrate_n + k + 2 * kHistN)
                              % kHistN;
                    hrate[0][k] = g_hist_rate[idx].sync;
                    hrate[1][k] = g_hist_rate[idx].fup;
                    hrate[2][k] = g_hist_rate[idx].ann;
                    hrate[3][k] = g_hist_rate[idx].dresp;
                }
            }
            gm_unaccepted = g_have_gm && !gm_acceptable_locked();
            if (g_have_gm) {
                id_line = format_gm(g_gm.id);
                char tsbuf[8];
                const char *ts = time_source_short(g_gm.time_source);
                if (!ts) {
                    snprintf(tsbuf, sizeof(tsbuf), "TS%02X",
                             g_gm.time_source);
                    ts = tsbuf;
                }
                char det[48];
                snprintf(det, sizeof(det), "P%u/%u CL%u %s",
                         g_gm.priority1, g_gm.priority2,
                         g_gm.clock_class, ts);
                detail_line = det;
            }
            timespec now;
            clock_gettime(CLOCK_MONOTONIC_RAW, &now);
            if (g_gm_changes > 0 &&
                (now.tv_sec - g_last_gm_change.tv_sec) < 10)
                gm_recent_change = true;
        }

        offscreen->SetBrightness((uint8_t)s.brightness);
        flip_canvas.inner = offscreen;
        flip_canvas.flip = s.rotate180;

        // Blackout, or no PTP reference (yet): show a black display
        if (s.blackout || !have_ptp_ref) {
            offscreen->Fill(0, 0, 0);
            offscreen = matrix->SwapOnVSync(offscreen);
            continue;
        }

        // --- Smooth time: master(TAI) = local(mono) - offset ---
        int64_t display_signed = (int64_t)local_clock_ns() - g_offset_ns;
        if (display_signed < 0)
            display_signed = 0;
        uint64_t display_ns = (uint64_t)display_signed;

        time_t sec = display_ns / 1000000000ULL;
        uint32_t nsec = display_ns % 1000000000ULL;

        // Active clock line: one entry = static, several = alternating
        // every 4 s, aligned to the PTP second (so LED, browser clocks and
        // the info-line rotation below all switch at the same moment)
        ClockEntry entry;
        if (!s.clocks.empty()) {
            size_t idx = s.clocks.size() > 1
                ? (size_t)((sec / 4) % s.clocks.size()) : 0;
            entry = s.clocks[idx];
            if (idx != flip_entry_idx) {
                flip_entry_idx = idx;
                flip_init = false;        // reset the flip animation
            }
        }

        bool zone_tai = entry.zone == "TAI";
        bool zone_utc = entry.zone == "UTC" || entry.zone.empty();
        if (!zone_tai)
            sec -= current_utc_offset;   // TAI -> UTC

        struct tm tm_disp;
        if (zone_tai || zone_utc) {
            gmtime_r(&sec, &tm_disp);
        } else {
            ensure_tz(entry.zone);
            localtime_r(&sec, &tm_disp);
        }

        // Line label: the entry's name; %Z expands to the zone abbreviation
        std::string cycle_label = entry.name;
        if (cycle_label == "%Z") {
            if (zone_tai) {
                cycle_label = "TAI";
            } else if (zone_utc) {
                cycle_label = "UTC";
            } else {
                char zb[24];
                cycle_label = (strftime(zb, sizeof(zb), "%Z", &tm_disp) > 0)
                    ? std::string(zb) : std::string();
            }
        }
        // 12h style: AM/PM lives on the label line so the big line keeps
        // all nine fractional digits
        if (entry.style == "12h") {
            const char *ampm = tm_disp.tm_hour < 12 ? "AM" : "PM";
            cycle_label += cycle_label.empty() ? ampm
                                               : (std::string(" ") + ampm);
        }

        offscreen->Fill(0,0,0);

        // Second line: date, GM ID, details — alternating every 4 s.
        // A recent grandmaster change overrides everything with "! NEUER GM !".
        std::string line2, mid_line;
        bool line2_alert = false;
        if (have_small_font) {
            std::vector<std::string> lines;
            if (s.show_date) {
                static const char *kDays[7] =
                    {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
                char date_buf[24];
                if (s.date_format == DATE_ISO)
                    snprintf(date_buf, sizeof(date_buf), "%s %04d-%02d-%02d",
                             kDays[tm_disp.tm_wday], tm_disp.tm_year + 1900,
                             tm_disp.tm_mon + 1, tm_disp.tm_mday);
                else if (s.date_format == DATE_MDY)
                    snprintf(date_buf, sizeof(date_buf), "%s %02d/%02d/%04d",
                             kDays[tm_disp.tm_wday], tm_disp.tm_mon + 1,
                             tm_disp.tm_mday, tm_disp.tm_year + 1900);
                else
                    snprintf(date_buf, sizeof(date_buf), "%s %02d.%02d.%04d",
                             kDays[tm_disp.tm_wday], tm_disp.tm_mday,
                             tm_disp.tm_mon + 1, tm_disp.tm_year + 1900);
                lines.push_back(date_buf);
            }
            if (s.show_gm && !id_line.empty())
                lines.push_back(id_line);
            if (s.show_gm_details && !detail_line.empty())
                lines.push_back(detail_line);
            // Rotation window from the PTP time (like the cycle mode), so
            // all display lines switch at the same moment
            if (!lines.empty())
                line2 = lines[(display_ns / 4000000000ULL) % lines.size()];
            if (gm_recent_change && s.notify_gm_change) {
                line2 = "! NEW GM !";
                line2_alert = true;
            }
            // Persistent error state beats the transient change alert
            if (gm_unaccepted) {
                line2 = "! UNACCEPTED GM !";
                line2_alert = true;
            }
            // Cycle mode: label the displayed scale — in a small line of
            // its own between time and second line when the latter is in
            // use, otherwise as the second line
            if (!cycle_label.empty()) {
                if (line2.empty())
                    line2 = cycle_label;
                else
                    mid_line = cycle_label;
            }
        }

        // --- Render the active clock line in its style ---
        // Vertical band available to the renderer above the small lines
        int band_h = 32;
        if (!line2.empty())
            band_h = mid_line.empty() ? 23 : 16;

        // Without a small font the alerts fall back to coloring the time
        // itself red (10 s on a GM change; while an unaccepted GM is elected).
        bool time_alert = !have_small_font &&
                          ((gm_recent_change && s.notify_gm_change) ||
                           gm_unaccepted);
        Color c_main = time_alert ? Color(255, 0, 0)
                                  : Color(s.r, s.g, s.b);
        Color c_dim(s.r / 5, s.g / 5, s.b / 5);
        Canvas *cv = &flip_canvas;

        // Baseline for single-line text styles (legacy placement)
        int text_base = !line2.empty()
            ? font.baseline() + 3
            : (matrix_options.rows + font.baseline()) / 2;

        // Centered text: N monospace chars at extra_spacing 1 are
        // N * (charwidth + 1) - 1 pixels wide
        auto draw_center = [&](Font &f, const char *txt, int ybase,
                               Color col) {
            int n = (int)strlen(txt);
            int x = (matrix_options.cols -
                     ((f.CharacterWidth('0') + 1) * n - 1)) / 2;
            DrawText(cv, f, x, ybase, col, nullptr, txt, 1);
        };

        const std::string &style = entry.style;
        if (style == "unix") {
            // Seconds since the epoch, big — all nine fractional digits
            // follow in the small font on the same baseline
            char bsec[24], bfrac[16];
            snprintf(bsec, sizeof(bsec), "%lld", (long long)sec);
            bfrac[0] = '.';
            format_fraction(bfrac + 1, display_ns);
            if (have_small_font) {
                int wbig = (font.CharacterWidth('0') + 1) *
                           (int)strlen(bsec) - 1;
                int wsm = (small_font.CharacterWidth('0') + 1) *
                          (int)strlen(bfrac) - 1;
                int x0 = (matrix_options.cols - (wbig + 2 + wsm)) / 2;
                DrawText(cv, font, x0, text_base, c_main, nullptr, bsec, 1);
                DrawText(cv, small_font, x0 + wbig + 2, text_base, c_main,
                         nullptr, bfrac, 1);
            } else {
                draw_center(font, bsec, text_base, c_main);
            }
        } else if (style == "bcd") {
            // Binary clock: six BCD columns (HHMMSS), MSB at the top
            int dg[6] = {tm_disp.tm_hour / 10, tm_disp.tm_hour % 10,
                         tm_disp.tm_min / 10,  tm_disp.tm_min % 10,
                         tm_disp.tm_sec / 10,  tm_disp.tm_sec % 10};
            int cell = band_h >= 24 ? 5 : 3;
            int dot = cell - 1;
            int ytop = (band_h - (4 * cell - 1)) / 2;
            if (ytop < 0)
                ytop = 0;
            int total = 6 * (dot + 2) - 2 + 2 * 4;
            int x0 = (matrix_options.cols - total) / 2;
            for (int c = 0; c < 6; ++c) {
                int x = x0 + c * (dot + 2) + (c / 2) * 4;
                for (int b = 0; b < 4; ++b) {
                    bool on = (dg[c] >> (3 - b)) & 1;
                    Color cc = on ? c_main : c_dim;
                    for (int dy = 0; dy < dot; ++dy)
                        for (int dx = 0; dx < dot; ++dx)
                            cv->SetPixel(x + dx, ytop + b * cell + dy,
                                         cc.r, cc.g, cc.b);
                }
            }
        } else if (style == "flip") {
            // Solari-style roll on HH:MM:SS — the nine fractional digits
            // follow plainly and spin on their own
            char fracbuf[10];
            format_fraction(fracbuf, display_ns);
            char cur[24];
            snprintf(cur, sizeof(cur), "%02d:%02d:%02d.%s",
                     tm_disp.tm_hour, tm_disp.tm_min, tm_disp.tm_sec,
                     fracbuf);
            if (!flip_init) {
                memcpy(flip_prev, cur, 8);
                memcpy(flip_old, cur, 8);
                flip_prev[8] = flip_old[8] = 0;
                for (int i = 0; i < 8; ++i)
                    flip_change[i] = 0;
                flip_init = true;
            }
            int cw = font.CharacterWidth('0') + 1;
            int x0 = (matrix_options.cols - (cw * 18 - 1)) / 2;
            clip_canvas.inner = cv;
            clip_canvas.y0 = text_base - font.baseline();
            clip_canvas.y1 = text_base + 2;
            for (int i = 0; i < 8; ++i) {
                if (cur[i] != flip_prev[i]) {
                    flip_old[i] = flip_prev[i];
                    flip_prev[i] = cur[i];
                    flip_change[i] = display_ns;
                }
                char one[2] = {cur[i], 0};
                uint64_t dt = display_ns - flip_change[i];
                if (flip_change[i] != 0 && dt < 250000000ULL) {
                    int off = (int)(dt * 14 / 250000000ULL);
                    char oldc[2] = {flip_old[i], 0};
                    DrawText(&clip_canvas, font, x0 + i * cw,
                             text_base + off, c_main, nullptr, oldc, 1);
                    DrawText(&clip_canvas, font, x0 + i * cw,
                             text_base + off - 14, c_main, nullptr, one, 1);
                } else {
                    DrawText(cv, font, x0 + i * cw, text_base, c_main,
                             nullptr, one, 1);
                }
            }
            // ".nnnnnnnnn" — drawn plainly, no roll
            DrawText(cv, font, x0 + 8 * cw, text_base, c_main,
                     nullptr, cur + 8, 1);
        } else if (style == "dcf77") {
            // The current minute as a DCF77 telegram: 59 second marks
            // (short = 0, long = 1), second 59 is the sync gap. DCF77
            // announces the NEXT minute, so encode that.
            time_t tnext = sec + (60 - tm_disp.tm_sec);
            struct tm tn;
            if (zone_tai || zone_utc)
                gmtime_r(&tnext, &tn);
            else
                localtime_r(&tnext, &tn);   // ensure_tz already applied
            bool bit[59] = {false};
            auto put_bcd = [&](int v, int start, int n, int &par) {
                int b = ((v / 10) << 4) | (v % 10);
                for (int i = 0; i < n; ++i) {
                    bit[start + i] = (b >> i) & 1;
                    par ^= (int)bit[start + i];
                }
            };
            int par = 0;
            bit[17] = tn.tm_isdst > 0;
            bit[18] = !bit[17];
            bit[20] = true;
            par = 0; put_bcd(tn.tm_min, 21, 7, par);  bit[28] = par;
            par = 0; put_bcd(tn.tm_hour, 29, 6, par); bit[35] = par;
            par = 0;
            put_bcd(tn.tm_mday, 36, 6, par);
            put_bcd(tn.tm_wday == 0 ? 7 : tn.tm_wday, 42, 3, par);
            put_bcd(tn.tm_mon + 1, 45, 5, par);
            put_bcd(tn.tm_year % 100, 50, 8, par);
            bit[58] = par;

            int ytop = (band_h - 9) / 2;
            if (ytop < 0)
                ytop = 0;
            for (int i = 0; i < 60; ++i) {
                int cx = 4 + (i % 30) * 4;
                int cy = ytop + (i / 30) * 6;
                if (i == 59) {            // sync gap: no mark
                    cv->SetPixel(cx, cy + 2, c_dim.r, c_dim.g, c_dim.b);
                    continue;
                }
                int h = bit[i] ? 3 : 1;   // long pulse = 1, short = 0
                Color cc(0, 0, 0);
                if (i < tm_disp.tm_sec) {
                    cc = c_main;          // already transmitted
                } else if (i == tm_disp.tm_sec) {
                    // live carrier dip: 100 ms for 0, 200 ms for 1
                    bool pulse = nsec < (bit[i] ? 200000000u : 100000000u);
                    cc = pulse ? Color(255, 255, 255) : c_main;
                } else {
                    cc = c_dim;           // still to come
                    h = 1;
                }
                for (int dy = 0; dy < h; ++dy)
                    for (int dx = 0; dx < 3; ++dx)
                        cv->SetPixel(cx + dx, cy + 2 - dy, cc.r, cc.g, cc.b);
            }
        } else if (style == "graph" || style == "rates") {
            // PTP analysis on the matrix — same data as the web charts.
            // "graph" plots the offset deviation per Sync plus the path
            // delay samples, "rates" the received messages per second.
            struct Series { const int32_t *v; int n; Color col; };
            int32_t r32[4][kHistN];
            std::vector<Series> series;
            if (style == "graph") {
                series.push_back({hdel, hdel_n, Color(40, 140, 255)});
                series.push_back({hoff, hoff_n, c_main});
            } else {
                const Color rc[4] = {Color(60, 220, 100),
                                     Color(80, 190, 255),
                                     Color(255, 210, 0),
                                     Color(255, 90, 190)};
                for (int f = 0; f < 4; ++f) {
                    for (int k = 0; k < hrate_n; ++k)
                        r32[f][k] = hrate[f][k];
                    series.push_back({r32[f], hrate_n, rc[f]});
                }
            }
            std::vector<int32_t> all;
            for (const auto &sr : series)
                all.insert(all.end(), sr.v, sr.v + sr.n);
            if ((int)all.size() < 2) {
                if (have_small_font)
                    draw_center(small_font, "COLLECTING...",
                                band_h / 2 + 3, c_dim);
            } else {
                // Robust autoscale like the web charts: percentiles with
                // zero included, outliers clip at the band edge
                std::sort(all.begin(), all.end());
                auto q = [&](double f) {
                    return (int64_t)all[(size_t)(f * (all.size() - 1))];
                };
                int64_t mn = std::min<int64_t>(q(0.03), 0);
                int64_t mx = std::max<int64_t>(q(0.97), 0);
                if (mx == mn) { mx += 1; mn -= 1; }
                int64_t margin = (mx - mn) * 12 / 100;
                int64_t lo = mn - margin, hi = mx + margin;
                int W = matrix_options.cols;
                auto ypix = [&](int64_t v) {
                    if (v < lo) v = lo;
                    if (v > hi) v = hi;
                    return (int)((int64_t)(band_h - 1) -
                                 (v - lo) * (band_h - 1) / (hi - lo));
                };
                int yz = ypix(0);
                for (int x = 0; x < W; ++x)
                    cv->SetPixel(x, yz, c_dim.r, c_dim.g, c_dim.b);
                for (const auto &sr : series) {
                    if (sr.n < 2)
                        continue;
                    int prev = -1;
                    for (int x = 0; x < W; ++x) {
                        int y = ypix(sr.v[(int64_t)x * (sr.n - 1)
                                          / (W - 1)]);
                        int a = prev < 0 ? y : std::min(prev, y);
                        int b = prev < 0 ? y : std::max(prev, y);
                        for (int yy = a; yy <= b; ++yy)
                            cv->SetPixel(x, yy, sr.col.r, sr.col.g,
                                         sr.col.b);
                        prev = y;
                    }
                }
            }
        } else {
            // Digital 24h (default) / 12h — always all nine fractional
            // digits; in 12h the AM/PM sits on the label line
            char time_buffer[64];
            int hh = tm_disp.tm_hour;
            if (style == "12h") {
                hh = hh % 12;
                if (hh == 0)
                    hh = 12;
            }
            char fracbuf[10];
            format_fraction(fracbuf, display_ns);
            snprintf(time_buffer, sizeof(time_buffer),
                     "%02d:%02d:%02d.%s",
                     hh, tm_disp.tm_min, tm_disp.tm_sec, fracbuf);
            draw_center(font, time_buffer, text_base, c_main);
        }

        if (!line2.empty()) {
            int x2 = (matrix_options.cols -
                      ((small_font.CharacterWidth('0') + 1) *
                       (int)line2.size() - 1)) / 2;
            Color c2 = line2_alert
                ? Color(255, 0, 0)
                : Color(s.r / 2, s.g / 2, s.b / 2);
            DrawText(&flip_canvas, small_font,
                     x2, matrix_options.rows - 2,
                     c2, nullptr, line2.c_str(), 1);
        }

        // Time-scale label between the time and the second line
        // (time rows 1..14, label rows 17..22, second line rows 25..30)
        if (!mid_line.empty()) {
            int xm = (matrix_options.cols -
                      ((small_font.CharacterWidth('0') + 1) *
                       (int)mid_line.size() - 1)) / 2;
            DrawText(&flip_canvas, small_font, xm, 22,
                     Color(s.r / 2, s.g / 2, s.b / 2),
                     nullptr, mid_line.c_str(), 1);
        }

        offscreen = matrix->SwapOnVSync(offscreen);
#endif  // NO_MATRIX
    }

    http_thread.join();
    close(sock_sync);
    close(sock_general);
#ifndef NO_MATRIX
    delete matrix;
#endif
    return 0;
}
