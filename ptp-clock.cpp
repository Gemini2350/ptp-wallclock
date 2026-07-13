#include "led-matrix.h"
#include "graphics.h"

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

using namespace rgb_matrix;

volatile bool interrupt_received = false;
static void InterruptHandler(int signo) {
    (void)signo;
    interrupt_received = true;
}

// ---- Settings (served / edited via the web interface) ----
enum TimeMode { MODE_UTC = 0, MODE_TAI = 1, MODE_LOCAL = 2 };
enum DateFormat { DATE_DMY = 0, DATE_ISO = 1, DATE_MDY = 2 };

struct Settings {
    uint8_t r = 255, g = 255, b = 0;      // display color
    bool show_gm = false;                 // show grandmaster ID on the matrix
    bool show_gm_details = false;         // show priorities / clock quality
    bool show_date = false;               // show the date on the matrix
    int mode = MODE_UTC;                  // UTC / TAI / local time
    std::string timezone = "Europe/Berlin";
    int time_format = 24;                 // 24 or 12 (AM/PM)
    int date_format = DATE_DMY;           // DD.MM.YYYY / ISO / MM/DD/YYYY
    bool notify_gm_change = false;        // notify on grandmaster change
    int http_port = 8080;
    int domain = -1;                      // PTP domain, -1 = auto detect
    std::string iface = "eth0";           // network interface
};

static Settings g_settings;
static std::mutex g_mutex;                // guards g_settings and GM state
static std::string g_config_path = "ptp-wallclock.conf";

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
        } else if (key == "show_gm") {
            g_settings.show_gm = (val == "1");
        } else if (key == "show_gm_details") {
            g_settings.show_gm_details = (val == "1");
        } else if (key == "show_date") {
            g_settings.show_date = (val == "1");
        } else if (key == "mode") {
            int m = atoi(val.c_str());
            if (m >= MODE_UTC && m <= MODE_LOCAL)
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

// Join the PTP multicast group on the given interface (returns its IP,
// 0 if the interface has no IPv4 address yet). Also (re-)derives our
// clock identity and sets the outgoing multicast interface.
static uint32_t join_ptp_multicast(int sock_event, int sock_gen,
                                   const std::string &ifname) {
    uint32_t ip = get_iface_ip(ifname);
    if (ip == 0)
        return 0;

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr("224.0.1.129");
    mreq.imr_interface.s_addr = ip;
    setsockopt(sock_event, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    setsockopt(sock_gen, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    in_addr mif{};
    mif.s_addr = ip;
    setsockopt(sock_event, IPPROTO_IP, IP_MULTICAST_IF, &mif, sizeof(mif));

    init_clock_identity(sock_event, ifname);
    std::cout << "PTP multicast joined on " << ifname
              << ", clock identity " << format_gm(g_clock_id) << "\n";
    return ip;
}

static void leave_ptp_multicast(int sock_event, int sock_gen, uint32_t ip) {
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr("224.0.1.129");
    mreq.imr_interface.s_addr = ip;
    setsockopt(sock_event, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
    setsockopt(sock_gen, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
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
      << "\"show_gm\":" << (g_settings.show_gm ? "true" : "false") << ","
      << "\"show_gm_details\":" << (g_settings.show_gm_details ? "true" : "false") << ","
      << "\"show_date\":" << (g_settings.show_date ? "true" : "false") << ","
      << "\"mode\":\"" << (g_settings.mode == MODE_TAI ? "tai" :
                           g_settings.mode == MODE_LOCAL ? "local" : "utc") << "\","
      << "\"timezone\":\"" << json_escape(g_settings.timezone) << "\","
      << "\"time_format\":\"" << g_settings.time_format << "\","
      << "\"date_format\":\"" << (g_settings.date_format == DATE_ISO ? "iso" :
                                  g_settings.date_format == DATE_MDY ? "mdy" :
                                  "dmy") << "\","
      << "\"notify_gm_change\":" << (g_settings.notify_gm_change ? "true" : "false") << ","
      << "\"domain\":" << g_settings.domain << ","
      << "\"iface\":\"" << json_escape(g_settings.iface) << "\","
      << "\"ifaces\":[";
    std::vector<std::string> ifs = list_ifaces();
    for (size_t i = 0; i < ifs.size(); ++i)
        j << (i ? "," : "") << "\"" << json_escape(ifs[i]) << "\"";
    j << "]}";
    return j.str();
}

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

    j << "{\"have_ptp\":" << (have_ptp_ref ? "true" : "false") << ","
      << "\"sync_age\":" << sync_age << ","
      << "\"domain\":" << g_domain.load() << ","
      << "\"active_domain\":" << g_active_domain.load() << ","
      << "\"iface\":\"" << json_escape(g_settings.iface) << "\","
      << "\"iface_up\":" << (g_iface_up ? "true" : "false") << ","
      << "\"gm_id\":\"" << (g_have_gm ? format_gm(g_gm.id) : "") << "\","
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
 #banner { display: none; background: #b33; color: #fff; padding: 0.6em;
        border-radius: 4px; margin-bottom: 1em; }
 #saved { color: #6c6; visibility: hidden; margin-left: 1em; }
</style>
</head>
<body>
<h1>PTP Wallclock &ndash; Settings</h1>
<div id="banner"></div>

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
<label>Display color:
 <input type="color" id="color" value="#ffff00">
</label>
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

async function loadSettings() {
  const s = await fetch('/api/settings').then(r => r.json());
  document.getElementById('color').value = s.color;
  document.getElementById('show_gm').checked = s.show_gm;
  document.getElementById('show_gm_details').checked = s.show_gm_details;
  document.getElementById('show_date').checked = s.show_date;
  document.getElementById('timezone').value = s.timezone;
  document.getElementById('notify').checked = s.notify_gm_change;
  document.getElementById('domain_auto').checked = (s.domain === -1);
  document.getElementById('domain').value = (s.domain === -1) ? 0 : s.domain;
  syncDomainInput();
  document.getElementById('iface').value = s.iface;
  document.getElementById('iflist').innerHTML =
      s.ifaces.map(i => '<option value="' + i + '">').join('');
  document.getElementById('time_format').value = s.time_format;
  document.getElementById('date_format').value = s.date_format;
  document.querySelector('input[name=mode][value="' + s.mode + '"]').checked = true;
}

document.getElementById('form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const body = new URLSearchParams({
    color: document.getElementById('color').value,
    show_gm: document.getElementById('show_gm').checked ? 1 : 0,
    show_gm_details: document.getElementById('show_gm_details').checked ? 1 : 0,
    show_date: document.getElementById('show_date').checked ? 1 : 0,
    mode: document.querySelector('input[name=mode]:checked').value,
    timezone: document.getElementById('timezone').value,
    time_format: document.getElementById('time_format').value,
    date_format: document.getElementById('date_format').value,
    domain: document.getElementById('domain_auto').checked
        ? -1 : document.getElementById('domain').value,
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

const set = (id, text) => document.getElementById(id).textContent = text;
let lastChanges = null;
async function poll() {
  try {
    const s = await fetch('/api/status').then(r => r.json());
    set('s_ptp', s.have_ptp
        ? 'synchronized (sync ' + s.sync_age.toFixed(1) + ' s ago)'
        : 'waiting for PTP...');
    set('s_iface', s.iface + (s.iface_up ? '' : ' (not connected)'));
    set('s_domain', s.domain === -1
        ? (s.active_domain >= 0
           ? s.active_domain + ' (auto-detected)'
           : 'scanning... (auto)')
        : s.domain);
    const gm = s.gm_id !== '';
    set('s_gm', gm ? s.gm_id : 'unknown');
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
            if (kv.count("show_gm"))
                g_settings.show_gm = (kv["show_gm"] == "1");
            if (kv.count("show_gm_details"))
                g_settings.show_gm_details = (kv["show_gm_details"] == "1");
            if (kv.count("show_date"))
                g_settings.show_date = (kv["show_date"] == "1");
            if (kv.count("mode")) {
                if (kv["mode"] == "tai") g_settings.mode = MODE_TAI;
                else if (kv["mode"] == "local") g_settings.mode = MODE_LOCAL;
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

    // --- Matrix options ---
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
    uint32_t joined_ip = 0;               // iface IP we joined multicast on
    uint64_t next_join_try_ns = 0;
    bool join_warned = false;

    // --- Main loop ---
    while (!interrupt_received) {

        if (g_reset_ptp.exchange(false))
            reset_ptp_state();            // e.g. domain changed via web UI

        if (g_iface_changed.exchange(false)) {
            if (joined_ip) {
                leave_ptp_multicast(sock_sync, sock_general, joined_ip);
                joined_ip = 0;
                g_iface_up = false;
            }
            reset_ptp_state();
            next_join_try_ns = 0;
            join_warned = false;
        }

        // (Re-)join multicast; retries every 5 s until the interface has
        // an IPv4 address (e.g. DHCP still running at boot)
        if (joined_ip == 0) {
            uint64_t now = mono_ns();
            if (now >= next_join_try_ns) {
                next_join_try_ns = now + 5000000000ULL;
                std::string ifname;
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    ifname = g_settings.iface;
                }
                joined_ip = join_ptp_multicast(sock_sync, sock_general,
                                               ifname);
                if (joined_ip) {
                    g_iface_up = true;
                    join_warned = false;
                } else if (!join_warned) {
                    std::cerr << "No IPv4 address on interface '" << ifname
                              << "', retrying (configurable in the web UI)\n";
                    join_warned = true;
                }
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

        // --- Delay_Req once per second ---
        if (g_have_pair && now_ns - last_dreq_ns >= 1000000000ULL) {
            uint8_t pkt[44];
            build_delay_req(pkt, ++g_dreq_seq);
            uint64_t t_a = mono_ns();
            ssize_t sent = sendto(sock_sync, pkt, sizeof(pkt), 0,
                                  (struct sockaddr*)&dreq_dst,
                                  sizeof(dreq_dst));
            uint64_t t_b = mono_ns();
            if (sent == (ssize_t)sizeof(pkt)) {
                g_pending_dreq.valid = true;
                g_pending_dreq.seq = g_dreq_seq;
                g_pending_dreq.t3_mono = t_a + (t_b - t_a) / 2;
                g_dreq_sent++;
            }
            last_dreq_ns = now_ns;
        }

        if (!have_ptp_ref)
            continue;

        // --- Smooth time: master(TAI) = local(mono) - offset ---
        int64_t display_signed = (int64_t)mono_ns() - g_offset_ns;
        if (display_signed < 0)
            display_signed = 0;
        uint64_t display_ns = (uint64_t)display_signed;

        // Snapshot the settings + GM info for this frame
        Settings s;
        std::string id_line, detail_line;
        bool gm_recent_change = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            s = g_settings;
            if (g_have_gm) {
                id_line = format_gm(g_gm.id);
                char det[40];
                snprintf(det, sizeof(det), "P%u/%u CL%u AC%02X",
                         g_gm.priority1, g_gm.priority2,
                         g_gm.clock_class, g_gm.clock_accuracy);
                detail_line = det;
            }
            timespec now;
            clock_gettime(CLOCK_MONOTONIC_RAW, &now);
            if (g_gm_changes > 0 &&
                (now.tv_sec - g_last_gm_change.tv_sec) < 10)
                gm_recent_change = true;
        }

        time_t sec = display_ns / 1000000000ULL;
        uint32_t nsec = display_ns % 1000000000ULL;

        if (s.mode != MODE_TAI)
            sec -= current_utc_offset;   // TAI -> UTC

        struct tm tm_disp;
        if (s.mode == MODE_LOCAL)
            localtime_r(&sec, &tm_disp);
        else
            gmtime_r(&sec, &tm_disp);

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
            if (gm_recent_change && s.notify_gm_change) {
                line2 = "! NEW GM !";
                line2_alert = true;
            }
        }

        int x_center =
            (matrix_options.cols -
             font.CharacterWidth('0') * strlen(time_buffer)) / 2
            - 9;

        // With a second line active, move the time up to make room
        int y_center = !line2.empty()
            ? font.baseline() + 3
            : (matrix_options.rows + font.baseline()) / 2;

        // Without a small font the GM change indicator falls back to
        // coloring the time itself red for 10 seconds.
        bool time_alert = gm_recent_change && s.notify_gm_change &&
                          !have_small_font;
        DrawText(offscreen, font,
                 x_center, y_center,
                 time_alert ? Color(255, 0, 0) : Color(s.r, s.g, s.b),
                 nullptr, time_buffer, 1);

        if (!line2.empty()) {
            int x2 = (matrix_options.cols -
                      small_font.CharacterWidth('0') *
                      (int)line2.size()) / 2;
            Color c2 = line2_alert
                ? Color(255, 0, 0)
                : Color(s.r / 2, s.g / 2, s.b / 2);
            DrawText(offscreen, small_font,
                     x2, matrix_options.rows - 2,
                     c2, nullptr, line2.c_str(), 1);
        }

        offscreen = matrix->SwapOnVSync(offscreen);
    }

    http_thread.join();
    close(sock_sync);
    close(sock_general);
    delete matrix;
    return 0;
}
