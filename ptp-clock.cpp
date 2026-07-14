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
    int time_format = 24;                 // 24 or 12 (AM/PM)
    int date_format = DATE_DMY;           // DD.MM.YYYY / ISO / MM/DD/YYYY
    bool notify_gm_change = false;        // notify on grandmaster change
    int http_port = 8319;
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

// ---- Helpers ----
static uint64_t mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
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
    f << "acceptable_gms=" << g_settings.acceptable_gms << "\n";
}

static void load_settings() {
    std::ifstream f(g_config_path);
    if (!f)
        return;
    std::string line;
    while (std::getline(f, line)) {
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
        } else if (key == "acceptable_gms") {
            g_settings.acceptable_gms = val;
        }
    }
    g_domain = g_settings.domain;
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
static void complete_sync_pair(int64_t t1, int64_t t2) {
    g_last_t1 = t1;
    g_last_t2 = t2;
    g_have_pair = true;

    int64_t sample = t2 - t1 - (g_have_mpd ? g_mpd_ns : 0);
    if (!g_have_offset || llabs(sample - g_offset_ns) > 100000000LL) {
        g_offset_ns = sample;             // first sample or step: jump
        g_have_offset = true;
    } else {
        g_offset_ns += (sample - g_offset_ns) / 8;
    }
    g_offset_atomic = g_offset_ns;
    g_last_sync_mono_ns = (uint64_t)t2;
    have_ptp_ref = true;
}

// Port 319 (event): Sync
static void process_event_packet(const uint8_t *buf, ssize_t len,
                                 uint64_t now_mono) {
    if (len < 44 || (buf[1] & 0x0F) != 2 || !domain_ok(buf[4], false))
        return;
    uint8_t msg_type = buf[0] & 0x0F;
    if (msg_type != 0x00)                 // only Sync
        return;

    // BMCA: only accept Sync from the elected master
    if (!g_have_master || memcmp(buf + 20, g_elected_sender, 10) != 0)
        return;

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
      << "\"time_format\":\"" << g_settings.time_format << "\","
      << "\"date_format\":\"" << (g_settings.date_format == DATE_ISO ? "iso" :
                                  g_settings.date_format == DATE_MDY ? "mdy" :
                                  "dmy") << "\","
      << "\"notify_gm_change\":" << (g_settings.notify_gm_change ? "true" : "false") << ","
      << "\"domain\":" << g_settings.domain << ","
      << "\"acceptable_gms\":\"" << json_escape(g_settings.acceptable_gms) << "\","
      << "\"iface\":\"" << json_escape(g_settings.iface) << "\","
      << "\"ifaces\":[";
    std::vector<std::string> ifs = list_ifaces();
    for (size_t i = 0; i < ifs.size(); ++i)
        j << (i ? "," : "") << "\"" << json_escape(ifs[i]) << "\"";
    j << "]}";
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
    unsigned long long last_sync = g_last_sync_mono_ns.load();
    if (last_sync > 0) {
        uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
        sync_age = (double)(now_ns - last_sync) / 1e9;
    }

    // Current PTP time (TAI), split so the values stay exact in JS doubles
    long long tai_sec = -1, tai_nsec = 0;
    if (have_ptp_ref) {
        long long tai = (long long)mono_ns() - g_offset_atomic.load();
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
      << "\"gm_id\":\"" << (g_have_gm ? format_gm(g_gm.id) : "") << "\","
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
          << "\"priority1\":" << (int)m.gm.priority1 << ","
          << "\"priority2\":" << (int)m.gm.priority2 << ","
          << "\"clock_class\":" << (int)m.gm.clock_class << ","
          << "\"steps_removed\":" << m.gm.steps_removed << ","
          << "\"elected\":" << (m.elected ? "true" : "false") << "}";
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
 #clock { font-family: monospace; text-align: center; color: #fd0;
        font-size: clamp(1em, 4.4vw, 1.8em); white-space: nowrap; }
 a { color: #7ab; }
 #clockdate { font-family: monospace; text-align: center; color: #aaa;
        margin-top: 0.2em; }
</style>
</head>
<body>
<h1>PTP Wallclock &ndash; Settings</h1>
<div id="banner"></div>
<div id="gmwarn"></div>

<fieldset>
<legend>PTP time &nbsp;<a href="/clock" target="_blank" rel="noopener">fullscreen clock &rarr;</a></legend>
<div id="clock">--:--:--</div>
<div id="clockdate"></div>
</fieldset>

<fieldset>
<legend>Status</legend>
<table class="status">
 <tr><td>PTP</td><td id="s_ptp">&ndash;</td></tr>
 <tr><td>Interface</td><td id="s_iface">&ndash;</td></tr>
 <tr><td>Domain</td><td id="s_domain">&ndash;</td></tr>
 <tr><td>Grandmaster</td><td id="s_gm">&ndash;</td></tr>
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
<label>
 <input type="checkbox" id="rotate180"> Rotate display 180&deg; (LED matrix mounted upside down)
</label>
<p class="hint">The 2nd-line options below only affect the physical LED
matrix &mdash; the <a href="/clock">browser clock</a> always shows its own
date and status line:</p>
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
<legend>Time</legend>
<label><input type="radio" name="mode" value="utc" checked> UTC</label>
<label><input type="radio" name="mode" value="tai"> TAI</label>
<label><input type="radio" name="mode" value="local"> Local time (time zone)</label>
<label><input type="radio" name="mode" value="cycle"> Alternating UTC &rarr; TAI &rarr; local (4 s each, labelled)</label>
<label>Time zone:
 <input type="text" id="timezone" list="tzlist" value="Europe/Berlin">
 <datalist id="tzlist">
  <option value="Europe/Berlin"><option value="Europe/Zurich">
  <option value="Europe/Vienna"><option value="Europe/London">
  <option value="UTC"><option value="America/New_York">
  <option value="America/Chicago"><option value="America/Los_Angeles">
  <option value="Asia/Tokyo"><option value="Asia/Shanghai">
  <option value="Australia/Sydney">
 </datalist>
</label>
<label>Time format:
 <select id="time_format">
  <option value="24">24-hour</option>
  <option value="12">12-hour (AM/PM)</option>
 </select>
</label>
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
  blackout = s.blackout;
  renderBlackout();
  document.getElementById('show_gm').checked = s.show_gm;
  document.getElementById('show_gm_details').checked = s.show_gm_details;
  document.getElementById('show_date').checked = s.show_date;
  document.getElementById('timezone').value = s.timezone;
  document.getElementById('notify').checked = s.notify_gm_change;
  document.getElementById('domain_auto').checked = (s.domain === -1);
  document.getElementById('domain').value = (s.domain === -1) ? 0 : s.domain;
  syncDomainInput();
  document.getElementById('acceptable_gms').value = s.acceptable_gms;
  document.getElementById('iface').value = s.iface;
  document.getElementById('iflist').innerHTML =
      ['auto'].concat(s.ifaces).map(i => '<option value="' + i + '">').join('');
  document.getElementById('time_format').value = s.time_format;
  document.getElementById('date_format').value = s.date_format;
  document.querySelector('input[name=mode][value="' + s.mode + '"]').checked = true;
}

document.getElementById('form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const body = new URLSearchParams({
    color: document.getElementById('color').value,
    brightness: document.getElementById('brightness').value,
    rotate180: document.getElementById('rotate180').checked ? 1 : 0,
    show_gm: document.getElementById('show_gm').checked ? 1 : 0,
    show_gm_details: document.getElementById('show_gm_details').checked ? 1 : 0,
    show_date: document.getElementById('show_date').checked ? 1 : 0,
    mode: document.querySelector('input[name=mode]:checked').value,
    timezone: document.getElementById('timezone').value,
    time_format: document.getElementById('time_format').value,
    date_format: document.getElementById('date_format').value,
    domain: document.getElementById('domain_auto').checked
        ? -1 : document.getElementById('domain').value,
    acceptable_gms: document.getElementById('acceptable_gms').value,
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
// extrapolates in between and renders in the configured mode/format.
// Digits finer than the browser timer resolution (performance.now() is
// coarsened to 0.1-1 ms for privacy) would freeze between polls, so they
// are dithered every frame — like the unreadably fast digits on the LED.
let quantumNs = 1e6;
(function () {
  let last = performance.now(), min = Infinity, seen = 0;
  for (let i = 0; i < 20000 && seen < 20; i++) {
    const t = performance.now();
    if (t > last) { min = Math.min(min, t - last); seen++; }
    last = t;
  }
  if (seen > 0)
    quantumNs = Math.min(1e6, Math.max(1e3,
        Math.pow(10, Math.ceil(Math.log10(min * 1e6)))));
})();
const dither = (frac) =>
    frac - (frac % quantumNs) + Math.floor(Math.random() * quantumNs);

let clockBase = null;   // {sec, nsec, perf, off}
function renderClock() {
  const el = document.getElementById('clock');
  const ed = document.getElementById('clockdate');
  if (!clockBase) {
    el.textContent = '--:--:--';
    ed.textContent = '';
    return;
  }
  let mode = document.querySelector('input[name=mode]:checked').value;
  const h12 = document.getElementById('time_format').value === '12';
  const df = document.getElementById('date_format').value;

  const elapsed = performance.now() - clockBase.perf;
  const totalNs = clockBase.nsec + elapsed * 1e6;
  let sec = clockBase.sec + Math.floor(totalNs / 1e9);
  const frac = dither(Math.floor(totalNs % 1e9));
  const cycling = mode === 'cycle';              // rotate per PTP second
  if (cycling) mode = ['utc', 'tai', 'local'][Math.floor(sec / 4) % 3];
  let tz = mode === 'local'
      ? document.getElementById('timezone').value : 'UTC';
  if (mode !== 'tai') sec -= clockBase.off;      // TAI -> UTC

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
    ed.textContent = 'unknown time zone: ' + tz;
    return;
  }
  let hh = p.hour, suffix = '';
  if (h12) {
    let h = parseInt(hh, 10);
    suffix = h >= 12 ? ' PM' : ' AM';
    h = h % 12 || 12;
    hh = String(h).padStart(2, '0');
  }
  let scale = '';
  if (cycling)
    scale = ' ' + (mode === 'local'
        ? (p.timeZoneName || 'LOCAL') : mode.toUpperCase());
  else if (mode === 'tai')
    scale = ' TAI';
  el.textContent = hh + ':' + p.minute + ':' + p.second + '.' +
      String(frac).padStart(9, '0') + suffix + scale;
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
    set('s_domain', s.domain === -1
        ? (s.active_domain >= 0
           ? s.active_domain + ' (auto-detected)'
           : 'scanning... (auto)')
        : s.domain);
    const gm = s.gm_id !== '';
    const badGm = gm && s.gm_accepted === false;
    set('s_gm', gm ? s.gm_id + (badGm ? ' — NOT ACCEPTED' : '') : 'unknown');
    document.getElementById('s_gm').style.color = badGm ? '#f66' : '';
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
          '<tr' + (m.elected ? ' class="elected"' : '') + '><td>' + m.gm_id +
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
 #date { margin-top: 1.4vw; font-size: min(2.4vw, 7vh);
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
   #date, #footer { display: none; }
   #time { font-size: min(8.5vw, 72vh); }
 }
</style>
</head>
<body>
<div id="main">
 <div id="time">--:--:--</div>
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

// Digits finer than the browser timer resolution are dithered every frame
// (see the settings page clock for the same trick)
let quantumNs = 1e6;
(function () {
  let last = performance.now(), min = Infinity, seen = 0;
  for (let i = 0; i < 20000 && seen < 20; i++) {
    const t = performance.now();
    if (t > last) { min = Math.min(min, t - last); seen++; }
    last = t;
  }
  if (seen > 0)
    quantumNs = Math.min(1e6, Math.max(1e3,
        Math.pow(10, Math.ceil(Math.log10(min * 1e6)))));
})();
const dither = (frac) =>
    frac - (frac % quantumNs) + Math.floor(Math.random() * quantumNs);

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
  const ed = document.getElementById('date');
  const bl = st && st.blackout;
  document.getElementById('main').style.opacity =
      bl ? 0 : st ? 0.12 + 0.88 * st.brightness / 100 : 1;
  document.getElementById('footer').style.opacity = bl ? 0 : 1;
  document.getElementById('bo').style.display = bl ? 'block' : 'none';
  if (!base || !S) {
    el.textContent = '--:--:--';
    ed.textContent = st && !st.have_ptp ? 'WAITING FOR PTP' : '';
    return;
  }
  let mode = S.mode;
  const h12 = S.time_format === '12', df = S.date_format;

  const elapsed = performance.now() - base.perf;
  const totalNs = base.nsec + elapsed * 1e6;
  let sec = base.sec + Math.floor(totalNs / 1e9);
  const frac = dither(Math.floor(totalNs % 1e9));
  const cycling = mode === 'cycle';              // rotate per PTP second
  if (cycling) mode = ['utc', 'tai', 'local'][Math.floor(sec / 4) % 3];
  const tz = mode === 'local' ? S.timezone : 'UTC';
  if (mode !== 'tai') sec -= base.off;
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
    ed.textContent = 'UNKNOWN TIME ZONE';
    return;
  }
  let hh = p.hour, sup = '';
  if (h12) {
    let h = parseInt(hh, 10);
    sup = h >= 12 ? 'PM' : 'AM';
    h = h % 12 || 12;
    hh = String(h).padStart(2, '0');
  }
  if (cycling)
    sup += (sup ? ' ' : '') + (mode === 'local'
        ? (p.timeZoneName || 'LOCAL') : mode.toUpperCase());
  else if (mode === 'tai')
    sup += (sup ? ' ' : '') + 'TAI';
  el.innerHTML = hh + ':' + p.minute + ':' + p.second + '.' +
      String(frac).padStart(9, '0') +
      (sup ? '<span class="sup">' + sup + '</span>' : '');
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
    } else if (method == "GET" &&
               (path == "/favicon.svg" || path == "/favicon.ico")) {
        send_response(fd, "200 OK", "image/svg+xml", kFaviconSvg);
    } else if (method == "GET" && path == "/api/settings") {
        send_response(fd, "200 OK", "application/json", settings_json());
    } else if (method == "GET" && path == "/api/status") {
        send_response(fd, "200 OK", "application/json", status_json());
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

        // --- SYNC (port 319) ---
        if (FD_ISSET(sock_sync, &fds)) {
            uint8_t buf[128];
            ssize_t len = recv(sock_sync, buf, sizeof(buf), 0);
            if (len > 0)
                process_event_packet(buf, len, mono_ns());
        }

        // --- ANNOUNCE / FOLLOW_UP / DELAY_RESP (port 320) ---
        if (FD_ISSET(sock_general, &fds)) {
            uint8_t buf[128];
            ssize_t len = recv(sock_general, buf, sizeof(buf), 0);
            if (len > 0)
                process_general_packet(buf, len, mono_ns());
        }

        uint64_t now_ns = mono_ns();

        // --- Auto domain: rescan when Announce stops for 15 s ---
        if (g_domain.load() < 0 && g_active_domain.load() >= 0) {
            unsigned long long last_ann = g_last_announce_mono_ns.load();
            if (last_ann > 0 && now_ns - last_ann > 15000000000ULL) {
                std::cout << "PTP domain lost, rescanning...\n";
                reset_ptp_state();
            }
        }

        // --- BMCA housekeeping: drop masters that stopped announcing ---
        if (now_ns - last_bmca_ns >= 1000000000ULL) {
            last_bmca_ns = now_ns;
            std::lock_guard<std::mutex> lock(g_mutex);
            bmca_elect_locked(now_ns);
        }

        // --- Delay_Req once per second, out of every joined interface
        //     (only the elected master answers; matched by our identity) ---
        if (g_have_pair && !joined.empty() &&
            now_ns - last_dreq_ns >= 1000000000ULL) {
            uint8_t pkt[44];
            build_delay_req(pkt, ++g_dreq_seq);
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
                g_pending_dreq.valid = true;
                g_pending_dreq.seq = g_dreq_seq;
                g_pending_dreq.t3_mono = t_a + (t_b - t_a) / 2;
                g_dreq_sent++;
            }
            last_dreq_ns = now_ns;
        }

#ifndef NO_MATRIX
        // Snapshot the settings + GM info for this frame
        Settings s;
        std::string id_line, detail_line;
        bool gm_recent_change = false;
        bool gm_unaccepted = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            s = g_settings;
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
        int64_t display_signed = (int64_t)mono_ns() - g_offset_ns;
        if (display_signed < 0)
            display_signed = 0;
        uint64_t display_ns = (uint64_t)display_signed;

        time_t sec = display_ns / 1000000000ULL;
        uint32_t nsec = display_ns % 1000000000ULL;

        // Cycle mode rotates UTC -> TAI -> local every 4 s. The window is
        // derived from the PTP second, so LED and browser clocks flip in
        // sync; the second line labels which scale is shown.
        static const int kCycle[3] = {MODE_UTC, MODE_TAI, MODE_LOCAL};
        int eff_mode = (s.mode == MODE_CYCLE)
            ? kCycle[(sec / 4) % 3] : s.mode;

        if (eff_mode != MODE_TAI)
            sec -= current_utc_offset;   // TAI -> UTC

        struct tm tm_disp;
        if (eff_mode == MODE_LOCAL)
            localtime_r(&sec, &tm_disp);
        else
            gmtime_r(&sec, &tm_disp);

        std::string cycle_label;
        if (s.mode == MODE_CYCLE) {
            if (eff_mode == MODE_UTC) {
                cycle_label = "UTC";
            } else if (eff_mode == MODE_TAI) {
                cycle_label = "TAI";
            } else {
                char zb[24];
                if (strftime(zb, sizeof(zb), "%Z", &tm_disp) > 0)
                    cycle_label = zb;    // e.g. CEST
                else
                    cycle_label = "LOCAL";
            }
        }

        char time_buffer[64];
        if (s.time_format == 12) {
            // 12-hour: fewer fractional digits to make room for AM/PM
            int h12 = tm_disp.tm_hour % 12;
            if (h12 == 0)
                h12 = 12;
            snprintf(time_buffer, sizeof(time_buffer),
                     "%02d:%02d:%02d.%06u %cM",
                     h12, tm_disp.tm_min, tm_disp.tm_sec,
                     nsec / 1000,
                     tm_disp.tm_hour < 12 ? 'A' : 'P');
        } else {
            snprintf(time_buffer, sizeof(time_buffer),
                     "%02d:%02d:%02d.%09u",
                     tm_disp.tm_hour, tm_disp.tm_min, tm_disp.tm_sec,
                     nsec);
        }

        offscreen->Fill(0,0,0);

        // Second line: date, GM ID, details — alternating every 4 s.
        // A recent grandmaster change overrides everything with "! NEUER GM !".
        std::string line2;
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
            if (!lines.empty())
                line2 = lines[(now_ns / 4000000000ULL) % lines.size()];
            // Cycle mode: the second line labels the displayed time scale
            if (!cycle_label.empty())
                line2 = cycle_label;
            if (gm_recent_change && s.notify_gm_change) {
                line2 = "! NEW GM !";
                line2_alert = true;
            }
            // Persistent error state beats the transient change alert
            if (gm_unaccepted) {
                line2 = "! UNACCEPTED GM !";
                line2_alert = true;
            }
        }

        // Drawn width of N monospace chars with DrawText extra_spacing 1:
        // N * (charwidth + 1) - 1  — the trailing spacing is not visible.
        int time_len = (int)strlen(time_buffer);
        int x_center =
            (matrix_options.cols -
             ((font.CharacterWidth('0') + 1) * time_len - 1)) / 2;

        // With a second line active, move the time up to make room
        int y_center = !line2.empty()
            ? font.baseline() + 3
            : (matrix_options.rows + font.baseline()) / 2;

        // Without a small font the alerts fall back to coloring the time
        // itself red (10 s on a GM change; while an unaccepted GM is elected).
        bool time_alert = !have_small_font &&
                          ((gm_recent_change && s.notify_gm_change) ||
                           gm_unaccepted);
        DrawText(&flip_canvas, font,
                 x_center, y_center,
                 time_alert ? Color(255, 0, 0) : Color(s.r, s.g, s.b),
                 nullptr, time_buffer, 1);

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
