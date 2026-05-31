// avl-wled: pure helpers (no syscalls / no network).
// Included by avl-wled.cpp (the service) and tests/unit.cpp (the test suite).

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <istream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace avl {

struct Config {
    std::string ical_url;
    std::string wled_host;
    std::string state_file   = "/var/lib/avl-wled/acked";
    std::string ical_cache   = "/var/lib/avl-wled/calendar.ics";
    int fetch_interval       = 7 * 24 * 3600;
    int check_interval       = 3600;
    int urgent_window        = 3 * 3600;
    int warn_window          = 24 * 3600;
    int night_start          = 22;
    int night_end            = 7;
    int http_port            = 8765;
    int led_count            = 60;
    int wled_brightness      = 128;
    int max_segments         = 8;
    std::string urgent_color = "FF0000";
    std::string normal_color = "00FF00";
    std::map<std::string, std::string> category_colors;
};

struct Event {
    std::string uid;
    std::string summary;
    std::time_t start = 0;
};

struct RGB { int r=0, g=0, b=0; };

struct ActiveCode {
    std::string uid;
    std::time_t start = 0;
    bool        urgent = false;
    RGB         color;
};

inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}

inline std::string toLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline bool applyKV(Config& c, const std::string& key, const std::string& val) {
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

inline void loadConfigFile(Config& c, const std::string& path) {
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

inline std::vector<std::string> unfold(std::istream& in) {
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

inline std::time_t parseIcalDt(const std::string& v) {
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

inline std::vector<Event> parseIcalStream(std::istream& in) {
    std::vector<Event> out;
    auto lines = unfold(in);
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

inline std::vector<Event> parseIcal(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    return parseIcalStream(f);
}

inline RGB parseHex(const std::string& in) {
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

inline bool isNightTime(const Config& cfg, std::time_t t) {
    struct tm lt;
    localtime_r(&t, &lt);
    int h = lt.tm_hour;
    if (cfg.night_start == cfg.night_end) return false;
    if (cfg.night_start < cfg.night_end)
        return h >= cfg.night_start && h < cfg.night_end;
    return h >= cfg.night_start || h < cfg.night_end;
}

// Returns the matching keyword (or empty if no map entry matched).
inline std::string matchKeyword(const Config& cfg, const std::string& summary) {
    std::string s = toLower(summary);
    for (auto& kv : cfg.category_colors) {
        if (s.find(kv.first) != std::string::npos)
            return kv.first;
    }
    return "";
}

inline RGB colorFor(const Config& cfg, const std::string& summary) {
    std::string kw = matchKeyword(cfg, summary);
    if (!kw.empty()) {
        auto it = cfg.category_colors.find(kw);
        if (it != cfg.category_colors.end()) return parseHex(it->second);
    }
    return parseHex(cfg.normal_color);
}

// Pure: compute active codes from (cfg, events, acked, now).
// Does NOT mutate `acked` (caller can GC stale entries separately).
inline std::vector<ActiveCode> computeActive(
        const Config& cfg,
        const std::vector<Event>& events,
        const std::set<std::string>& acked,
        std::time_t now) {
    std::vector<ActiveCode> out;
    bool night = isNightTime(cfg, now);
    for (auto& e : events) {
        if (e.start <= now) continue;
        if (acked.count(e.uid)) continue;
        long delta = (long)(e.start - now);
        bool urgent = delta <= cfg.urgent_window;
        bool warn   = delta <= cfg.warn_window;
        if (!urgent && !warn) continue;
        if (!urgent && night) continue;
        ActiveCode a;
        a.uid    = e.uid;
        a.start  = e.start;
        a.urgent = urgent;
        a.color  = colorFor(cfg, e.summary);
        out.push_back(a);
    }
    std::sort(out.begin(), out.end(),
              [](const ActiveCode& a, const ActiveCode& b){
                  return a.start < b.start;
              });
    return out;
}

struct NextByType {
    std::string keyword;    // matched keyword, or "" for unmapped
    const Event* event;     // earliest future event for that type
};

// Pure: for each type (matched keyword or "" = unmapped), return the
// soonest future event. Result is sorted by event time.
inline std::vector<NextByType> nextByType(
        const Config& cfg,
        const std::vector<Event>& events,
        std::time_t now) {
    std::map<std::string, const Event*> best;
    for (auto& e : events) {
        if (e.start <= now) continue;
        std::string kw = matchKeyword(cfg, e.summary);
        auto it = best.find(kw);
        if (it == best.end() || e.start < it->second->start)
            best[kw] = &e;
    }
    std::vector<NextByType> out;
    out.reserve(best.size());
    for (auto& kv : best) out.push_back({kv.first, kv.second});
    std::sort(out.begin(), out.end(),
              [](const NextByType& a, const NextByType& b){
                  return a.event->start < b.event->start;
              });
    return out;
}

inline std::string buildWledJson(const Config& cfg,
                                 const std::vector<ActiveCode>& codes) {
    std::ostringstream o;
    o << "{\"on\":";
    if (codes.empty()) {
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

    emit(0, pos, pos + leds_per, status);
    pos += leds_per;

    for (int i = 0; i < seg_count - 1; ++i) {
        int stop = (i == seg_count - 2) ? cfg.led_count : (pos + leds_per);
        emit(i + 1, pos, stop, codes[i].color);
        pos = stop;
    }
    for (int i = seg_count; i < cfg.max_segments; ++i) {
        o << ",{\"id\":" << i << ",\"stop\":0}";
    }
    o << "]}";
    return o.str();
}

inline std::set<std::string> loadAcked(const std::string& path) {
    std::set<std::string> s;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (!line.empty()) s.insert(line);
    }
    return s;
}

inline void saveAcked(const std::string& path,
                      const std::set<std::string>& s) {
    std::ofstream f(path, std::ios::trunc);
    for (auto& u : s) f << u << "\n";
}

} // namespace avl
