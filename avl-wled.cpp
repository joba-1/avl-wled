// avl-wled: poll an AVL iCal feed and drive a WLED instance.
// Build: g++ -O2 -std=c++17 -o avl-wled avl-wled.cpp -lcurl -lpthread

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <curl/curl.h>

// ---------- config ----------

struct Config {
    std::string ical_url;
    std::string wled_host;                      // host[:port], no scheme
    std::string state_file   = "/var/lib/avl-wled/acked";
    std::string ical_cache   = "/var/lib/avl-wled/calendar.ics";
    int fetch_interval       = 7 * 24 * 3600;   // weekly
    int check_interval       = 3600;            // hourly
    int urgent_window        = 3 * 3600;        // 3h
    int warn_window          = 24 * 3600;       // 24h
    int night_start          = 22;              // 22:00 local
    int night_end            = 7;               // 07:00 local
    int http_port            = 8765;
    int led_count            = 60;
    int wled_brightness      = 128;
    int max_segments         = 8;               // hardware-ish upper bound
    std::string urgent_color = "FF0000";
    std::string normal_color = "00FF00";
    std::map<std::string, std::string> category_colors; // lowercase keyword -> RRGGBB
};

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}
static std::string toLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool applyKV(Config& c, const std::string& key, const std::string& val) {
    try {
        if      (key == "ical_url")        c.ical_url = val;
        else if (key == "wled_host")       c.wled_host = val;
        else if (key == "state_file")      c.state_file = val;
        else if (key == "ical_cache")      c.ical_cache = val;
        else if (key == "fetch_interval")  c.fetch_interval = std::stoi(val);
        else if (key == "check_interval")  c.check_interval = std::stoi(val);
        else if (key == "urgent_window")   c.urgent_window = std::stoi(val);
        else if (key == "warn_window")     c.warn_window = std::stoi(val);
        else if (key == "night_start")     c.night_start = std::stoi(val);
        else if (key == "night_end")       c.night_end = std::stoi(val);
        else if (key == "http_port")       c.http_port = std::stoi(val);
        else if (key == "led_count")       c.led_count = std::stoi(val);
        else if (key == "wled_brightness") c.wled_brightness = std::stoi(val);
        else if (key == "max_segments")    c.max_segments = std::stoi(val);
        else if (key == "urgent_color")    c.urgent_color = val;
        else if (key == "normal_color")    c.normal_color = val;
        else if (key.rfind("color_", 0) == 0)
            c.category_colors[toLower(key.substr(6))] = val;
        else return false;
    } catch (...) { return false; }
    return true;
}

static void loadConfigFile(Config& c, const std::string& path) {
    std::ifstream f(path);
    if (!f) { std::cerr << "config: cannot open " << path << "\n"; return; }
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(t.substr(0, eq));
        std::string v = trim(t.substr(eq + 1));
        if (!applyKV(c, k, v))
            std::cerr << "config " << path << ":" << lineno
                      << ": unknown key '" << k << "'\n";
    }
}

// ---------- iCal parsing ----------

struct Event {
    std::string uid;
    std::string summary;
    time_t      start = 0;
};

static std::vector<std::string> unfold(std::istream& in) {
    std::vector<std::string> out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!out.empty() && !line.empty() && (line[0] == ' ' || line[0] == '\t'))
            out.back() += line.substr(1);
        else
            out.push_back(line);
    }
    return out;
}

static time_t parseIcalDt(const std::string& v) {
    // 20260605, 20260605T080000, 20260605T080000Z
    if (v.size() < 8) return 0;
    struct tm tm = {};
    if (std::sscanf(v.c_str(), "%4d%2d%2d",
                    &tm.tm_year, &tm.tm_mon, &tm.tm_mday) < 3) return 0;
    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;
    bool utc = false;
    if (v.size() >= 15 && v[8] == 'T') {
        std::sscanf(v.c_str() + 9, "%2d%2d%2d",
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
        if (v.back() == 'Z') utc = true;
    }
    tm.tm_isdst = -1;
    return utc ? timegm(&tm) : mktime(&tm);
}

static std::vector<Event> parseIcal(const std::string& path) {
    std::vector<Event> out;
    std::ifstream f(path);
    if (!f) return out;
    auto lines = unfold(f);
    Event cur;
    bool in_ev = false;
    for (auto& l : lines) {
        if (l == "BEGIN:VEVENT") { cur = Event{}; in_ev = true; continue; }
        if (l == "END:VEVENT")   {
            if (in_ev && cur.start) {
                if (cur.uid.empty())
                    cur.uid = std::to_string(cur.start) + "|" + cur.summary;
                out.push_back(cur);
            }
            in_ev = false; continue;
        }
        if (!in_ev) continue;
        size_t colon = l.find(':');
        if (colon == std::string::npos) continue;
        std::string name = l.substr(0, colon);
        std::string val  = l.substr(colon + 1);
        std::string base = name.substr(0, name.find(';'));
        if      (base == "UID")     cur.uid = val;
        else if (base == "SUMMARY") cur.summary = val;
        else if (base == "DTSTART") cur.start = parseIcalDt(val);
    }
    return out;
}

// ---------- HTTP via libcurl ----------

static size_t curlSink(char* p, size_t s, size_t n, void* u) {
    if (u) ((std::string*)u)->append(p, s * n);
    return s * n;
}

static bool httpGetToFile(const std::string& url, const std::string& path) {
    std::string body;
    CURL* c = curl_easy_init();
    if (!c) return false;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlSink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    CURLcode r = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (r != CURLE_OK || (code != 0 && code >= 400)) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(body.data(), body.size());
    return (bool)f;
}

static bool httpPostJson(const std::string& url, const std::string& body) {
    CURL* c = curl_easy_init();
    if (!c) return false;
    curl_slist* h = nullptr;
    h = curl_slist_append(h, "Content-Type: application/json");
    std::string sink;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlSink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    CURLcode r = curl_easy_perform(c);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return r == CURLE_OK;
}

// ---------- color + WLED ----------

struct RGB { int r=0, g=0, b=0; };

static RGB parseHex(const std::string& in) {
    std::string s = in;
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() < 6) return {0,0,0};
    auto hv = [](char c) {
        if (c>='0'&&c<='9') return c-'0';
        if (c>='a'&&c<='f') return c-'a'+10;
        if (c>='A'&&c<='F') return c-'A'+10;
        return 0;
    };
    return { hv(s[0])*16 + hv(s[1]),
             hv(s[2])*16 + hv(s[3]),
             hv(s[4])*16 + hv(s[5]) };
}

struct ActiveCode {
    std::string uid;
    time_t      start = 0;
    bool        urgent = false;
    RGB         color;
};

static std::string buildWledJson(const Config& cfg,
                                 const std::vector<ActiveCode>& codes) {
    std::ostringstream o;
    o << "{\"on\":";
    if (codes.empty()) {
        // all segments off
        o << "false,\"seg\":[";
        for (int i = 0; i < cfg.max_segments; ++i) {
            if (i) o << ",";
            o << "{\"id\":" << i << ",\"stop\":0}";
        }
        o << "]}";
        return o.str();
    }

    bool any_urgent = false;
    for (auto& c : codes) if (c.urgent) { any_urgent = true; break; }

    // status segment + one segment per code.
    int seg_count = 1 + (int)codes.size();
    if (seg_count > cfg.max_segments) seg_count = cfg.max_segments;
    int leds_per  = cfg.led_count / seg_count;
    if (leds_per < 1) leds_per = 1;

    RGB status = parseHex(any_urgent ? cfg.urgent_color : cfg.normal_color);

    o << "true,\"bri\":" << cfg.wled_brightness << ",\"seg\":[";

    int pos = 0;
    auto emit = [&](int idx, int start, int stop, RGB col) {
        if (idx) o << ",";
        o << "{\"id\":" << idx
          << ",\"start\":" << start
          << ",\"stop\":"  << stop
          << ",\"on\":true,\"col\":[["
          << col.r << "," << col.g << "," << col.b
          << "]],\"fx\":0}";
    };

    // status segment
    emit(0, pos, pos + leds_per, status);
    pos += leds_per;

    for (int i = 0; i < seg_count - 1; ++i) {
        int stop = (i == seg_count - 2) ? cfg.led_count : (pos + leds_per);
        emit(i + 1, pos, stop, codes[i].color);
        pos = stop;
    }
    // disable remaining segments
    for (int i = seg_count; i < cfg.max_segments; ++i) {
        o << ",{\"id\":" << i << ",\"stop\":0}";
    }
    o << "]}";
    return o.str();
}

// ---------- state (acked UIDs) ----------

static std::set<std::string> loadAcked(const std::string& path) {
    std::set<std::string> s;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (!line.empty()) s.insert(line);
    }
    return s;
}

static void saveAcked(const std::string& path,
                      const std::set<std::string>& s) {
    std::ofstream f(path, std::ios::trunc);
    for (auto& u : s) f << u << "\n";
}

// ---------- service core ----------

static std::atomic<bool> g_stop{false};
static std::mutex        g_mu;
static std::condition_variable g_cv;

struct State {
    Config              cfg;
    std::vector<Event>  events;
    std::set<std::string> acked;
    std::vector<ActiveCode> active;
    time_t              last_fetch = 0;
    time_t              last_check = 0;
};
static State g;

static bool isNightTime(const Config& cfg, time_t t) {
    struct tm lt;
    localtime_r(&t, &lt);
    int h = lt.tm_hour;
    if (cfg.night_start == cfg.night_end) return false;
    if (cfg.night_start < cfg.night_end)
        return h >= cfg.night_start && h < cfg.night_end;
    // wraps midnight, e.g. 22..7
    return h >= cfg.night_start || h < cfg.night_end;
}

static RGB colorFor(const Config& cfg, const std::string& summary) {
    std::string s = toLower(summary);
    for (auto& kv : cfg.category_colors) {
        if (s.find(kv.first) != std::string::npos)
            return parseHex(kv.second);
    }
    return parseHex(cfg.normal_color);
}

// Recompute active codes from events + acked + now. Caller holds g_mu.
static void recomputeActive() {
    g.active.clear();
    time_t now = time(nullptr);
    bool night = isNightTime(g.cfg, now);

    // also: drop acked UIDs that no longer appear (event already past)
    std::set<std::string> still;
    for (auto& e : g.events) still.insert(e.uid);

    std::set<std::string> trimmed;
    for (auto& u : g.acked) if (still.count(u)) trimmed.insert(u);
    if (trimmed.size() != g.acked.size()) {
        g.acked = std::move(trimmed);
        saveAcked(g.cfg.state_file, g.acked);
    }

    for (auto& e : g.events) {
        if (e.start <= now) continue;
        if (g.acked.count(e.uid)) continue;
        long delta = (long)(e.start - now);
        bool urgent = delta <= g.cfg.urgent_window;
        bool warn   = delta <= g.cfg.warn_window;
        if (!urgent && !warn) continue;
        if (!urgent && night) continue;       // night suppresses non-urgent
        ActiveCode a;
        a.uid    = e.uid;
        a.start  = e.start;
        a.urgent = urgent;
        a.color  = colorFor(g.cfg, e.summary);
        g.active.push_back(a);
    }
    std::sort(g.active.begin(), g.active.end(),
              [](const ActiveCode& a, const ActiveCode& b){
                  return a.start < b.start;
              });
}

static void pushWled() {
    if (g.cfg.wled_host.empty()) return;
    std::string url = "http://" + g.cfg.wled_host + "/json/state";
    std::string body = buildWledJson(g.cfg, g.active);
    if (!httpPostJson(url, body))
        std::cerr << "wled: post failed to " << url << "\n";
}

static void doFetch() {
    if (g.cfg.ical_url.empty()) return;
    std::cerr << "fetch: " << g.cfg.ical_url << "\n";
    if (!httpGetToFile(g.cfg.ical_url, g.cfg.ical_cache)) {
        std::cerr << "fetch: failed\n";
        return;
    }
    g.events = parseIcal(g.cfg.ical_cache);
    std::cerr << "fetch: " << g.events.size() << " events\n";
}

static void doCheck() {
    // reload events from cache in case file was updated externally
    if (g.events.empty())
        g.events = parseIcal(g.cfg.ical_cache);
    size_t before = g.active.size();
    recomputeActive();
    if (g.active.size() != before ||
        true /* always push, cheap and idempotent */) {
        pushWled();
    }
}

static bool handleAcknowledge() {
    // Caller holds g_mu.
    if (g.active.empty()) return false;
    const std::string uid = g.active.front().uid; // oldest
    g.acked.insert(uid);
    saveAcked(g.cfg.state_file, g.acked);
    recomputeActive();
    pushWled();
    std::cerr << "ack: removed " << uid
              << " (" << g.active.size() << " remaining)\n";
    return true;
}

// ---------- tiny HTTP server for /ack and /status ----------

static void httpServerLoop(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(s, (sockaddr*)&a, sizeof(a)) < 0) {
        perror("bind"); close(s); return;
    }
    if (listen(s, 4) < 0) { perror("listen"); close(s); return; }
    std::cerr << "http: listening on port " << port << "\n";

    while (!g_stop) {
        timeval tv{1, 0};
        fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
        int r = select(s + 1, &rf, nullptr, nullptr, &tv);
        if (r <= 0) continue;
        int c = accept(s, nullptr, nullptr);
        if (c < 0) continue;

        char buf[2048];
        int n = (int)recv(c, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { close(c); continue; }
        buf[n] = 0;

        // parse request line: METHOD SP PATH SP HTTP/x
        std::string req(buf, n);
        std::string method, path;
        {
            size_t sp1 = req.find(' ');
            size_t sp2 = (sp1 == std::string::npos) ? std::string::npos
                                                   : req.find(' ', sp1 + 1);
            if (sp1 != std::string::npos && sp2 != std::string::npos) {
                method = req.substr(0, sp1);
                path   = req.substr(sp1 + 1, sp2 - sp1 - 1);
            }
        }

        std::string body;
        int code = 200;
        std::string ctype = "text/plain; charset=utf-8";

        if (path == "/ack" || path.rfind("/ack?", 0) == 0 ||
            path.rfind("/ack/", 0) == 0) {
            std::lock_guard<std::mutex> lk(g_mu);
            bool did = handleAcknowledge();
            body = did ? "acknowledged\n" : "nothing to acknowledge\n";
        } else if (path == "/status" || path == "/") {
            std::lock_guard<std::mutex> lk(g_mu);
            std::ostringstream o;
            o << "events: " << g.events.size() << "\n"
              << "acked: "  << g.acked.size()  << "\n"
              << "active: " << g.active.size() << "\n";
            for (auto& a : g.active) {
                char ts[32];
                struct tm lt; localtime_r(&a.start, &lt);
                strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &lt);
                o << "  " << ts
                  << (a.urgent ? "  URGENT  " : "  warn    ")
                  << a.uid << "\n";
            }
            body = o.str();
        } else {
            code = 404;
            body = "not found\n";
        }

        std::ostringstream resp;
        resp << "HTTP/1.0 " << code << " "
             << (code == 200 ? "OK" : "Not Found") << "\r\n"
             << "Content-Type: " << ctype << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n\r\n"
             << body;
        std::string s_resp = resp.str();
        send(c, s_resp.data(), s_resp.size(), 0);
        close(c);
    }
    close(s);
}

// ---------- main ----------

static void onSig(int) { g_stop = true; g_cv.notify_all(); }

static void usage(const char* prog) {
    std::cerr <<
      "usage: " << prog << " [--config=PATH] [--KEY=VALUE ...]\n"
      "  Any config key may be overridden on the command line.\n"
      "  Example: --wled_host=wled.local --check_interval=600\n";
}

int main(int argc, char** argv) {
    std::string cfg_path = "/etc/avl-wled.conf";
    std::vector<std::pair<std::string,std::string>> overrides;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        if (a.rfind("--", 0) != 0) { usage(argv[0]); return 2; }
        std::string kv = a.substr(2);
        size_t eq = kv.find('=');
        std::string k, v;
        if (eq == std::string::npos) {
            k = kv;
            if (i + 1 >= argc) { usage(argv[0]); return 2; }
            v = argv[++i];
        } else {
            k = kv.substr(0, eq);
            v = kv.substr(eq + 1);
        }
        if (k == "config") cfg_path = v;
        else overrides.push_back({k, v});
    }

    loadConfigFile(g.cfg, cfg_path);
    for (auto& kv : overrides) {
        if (!applyKV(g.cfg, kv.first, kv.second))
            std::cerr << "cli: unknown key '" << kv.first << "'\n";
    }

    if (g.cfg.ical_url.empty() || g.cfg.wled_host.empty()) {
        std::cerr << "ical_url and wled_host are required\n";
        return 2;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    signal(SIGINT,  onSig);
    signal(SIGTERM, onSig);
    signal(SIGPIPE, SIG_IGN);

    g.acked  = loadAcked(g.cfg.state_file);
    g.events = parseIcal(g.cfg.ical_cache);

    std::thread http([&]{ httpServerLoop(g.cfg.http_port); });

    // main scheduler loop
    while (!g_stop) {
        time_t now = time(nullptr);
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (now - g.last_fetch >= g.cfg.fetch_interval ||
                g.events.empty()) {
                doFetch();
                g.last_fetch = now;
            }
            if (now - g.last_check >= g.cfg.check_interval ||
                g.last_check == 0) {
                doCheck();
                g.last_check = now;
            }
        }
        std::unique_lock<std::mutex> lk(g_mu);
        g_cv.wait_for(lk, std::chrono::seconds(30),
                      []{ return g_stop.load(); });
    }

    http.join();
    curl_global_cleanup();
    return 0;
}
