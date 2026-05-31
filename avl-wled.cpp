// avl-wled: poll an AVL iCal feed and drive a WLED instance.
// Build: g++ -O2 -std=c++17 -o avl-wled avl-wled.cpp -lcurl -lpthread

#include "core.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <curl/curl.h>

using namespace avl;

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

// ---------- service core ----------

static std::atomic<bool> g_stop{false};
static std::mutex        g_mu;
static std::condition_variable g_cv;

struct State {
    Config              cfg;
    std::vector<Event>  events;
    std::set<std::string> acked;
    std::vector<ActiveCode> active;
    std::time_t         last_fetch = 0;
    std::time_t         last_check = 0;
};
static State g;

// Caller holds g_mu.
static void recomputeActive() {
    // GC acked UIDs whose event is gone from the feed.
    std::set<std::string> still;
    for (auto& e : g.events) still.insert(e.uid);
    std::set<std::string> trimmed;
    for (auto& u : g.acked) if (still.count(u)) trimmed.insert(u);
    if (trimmed.size() != g.acked.size()) {
        g.acked = std::move(trimmed);
        saveAcked(g.cfg.state_file, g.acked);
    }
    g.active = computeActive(g.cfg, g.events, g.acked, std::time(nullptr));
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
    if (g.events.empty())
        g.events = parseIcal(g.cfg.ical_cache);
    recomputeActive();
    pushWled();
}

static bool handleAcknowledge() {
    if (g.active.empty()) return false;
    const std::string uid = g.active.front().uid;
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
            auto nxt = nextByType(g.cfg, g.events, std::time(nullptr));
            o << "next per type: " << nxt.size() << "\n";
            for (auto& n : nxt) {
                char ts[32];
                struct tm lt; localtime_r(&n.event->start, &lt);
                strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &lt);
                o << "  " << ts
                  << "  " << (n.keyword.empty() ? "(unmapped)" : n.keyword)
                  << "  " << n.event->summary
                  << "\n";
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

    while (!g_stop) {
        std::time_t now = std::time(nullptr);
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
