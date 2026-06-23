#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../core.h"

#include <sstream>

using namespace avl;

// ---------- parseHex ----------

TEST_CASE("parseHex basic") {
    auto r = parseHex("FF0000");
    CHECK(r.r == 255); CHECK(r.g == 0); CHECK(r.b == 0);

    r = parseHex("#00ff00");
    CHECK(r.r == 0); CHECK(r.g == 255); CHECK(r.b == 0);

    r = parseHex("123");           // too short
    CHECK(r.r == 0); CHECK(r.g == 0); CHECK(r.b == 0);

    // Non-hex digits decode as 0 per nibble: "!!!!!!" -> all zero.
    r = parseHex("!!!!!!");
    CHECK(r.r == 0); CHECK(r.g == 0); CHECK(r.b == 0);
}

// ---------- parseIcalDt ----------

TEST_CASE("parseIcalDt") {
    CHECK(parseIcalDt("") == 0);
    CHECK(parseIcalDt("nope") == 0);

    auto t_date = parseIcalDt("20260605");
    CHECK(t_date > 0);
    struct tm lt; localtime_r(&t_date, &lt);
    CHECK(lt.tm_year == 2026 - 1900);
    CHECK(lt.tm_mon  == 5);   // June
    CHECK(lt.tm_mday == 5);

    auto t_utc = parseIcalDt("20260605T080000Z");
    CHECK(t_utc > 0);
    struct tm ug; gmtime_r(&t_utc, &ug);
    CHECK(ug.tm_hour == 8);
    CHECK(ug.tm_min  == 0);

    auto t_loc = parseIcalDt("20260605T080000");
    CHECK(t_loc > 0);
    struct tm lo; localtime_r(&t_loc, &lo);
    CHECK(lo.tm_hour == 8);
}

// ---------- unfold ----------

TEST_CASE("unfold space and tab continuations") {
    std::istringstream in(
        "FOO:hello\n"
        " world\n"
        "BAR:line2\n"
        "\tcont\n"
        "BAZ:plain\r\n");
    auto out = unfold(in);
    REQUIRE(out.size() == 3);
    CHECK(out[0] == "FOO:helloworld");
    CHECK(out[1] == "BAR:line2cont");
    CHECK(out[2] == "BAZ:plain");
}

// ---------- parseIcal ----------

TEST_CASE("parseIcal single event") {
    auto evs = parseIcal("tests/data/single.ics");
    REQUIRE(evs.size() == 1);
    CHECK(evs[0].uid == "event-1@example");
    CHECK(evs[0].summary == "Restmuellbehaelter");
    CHECK(evs[0].start > 0);
}

TEST_CASE("parseIcal multi event + missing UID synthesized") {
    auto evs = parseIcal("tests/data/multi.ics");
    REQUIRE(evs.size() == 4);
    CHECK(evs[0].uid == "a@x");
    CHECK(evs[3].uid != "");                 // synthesized
    CHECK(evs[3].summary == "Schadstoffmobil");
}

TEST_CASE("parseIcal folded summary") {
    auto evs = parseIcal("tests/data/folded.ics");
    REQUIRE(evs.size() == 1);
    CHECK(evs[0].summary == "Gelbe Tonne folded summary");
}

TEST_CASE("parseIcal empty / missing file") {
    auto evs = parseIcal("tests/data/does-not-exist.ics");
    CHECK(evs.empty());
}

// ---------- isNightTime ----------

static std::time_t makeLocal(int Y, int M, int D, int h) {
    struct tm tm = {};
    tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
    tm.tm_hour = h; tm.tm_isdst = -1;
    return mktime(&tm);
}

TEST_CASE("isNightTime wrapping 22..7") {
    Config c;
    c.night_start = 22; c.night_end = 7;
    CHECK(isNightTime(c, makeLocal(2026,6,1,23)));
    CHECK(isNightTime(c, makeLocal(2026,6,1,3)));
    CHECK(!isNightTime(c, makeLocal(2026,6,1,7)));
    CHECK(!isNightTime(c, makeLocal(2026,6,1,12)));
}

TEST_CASE("isNightTime non-wrapping 1..5") {
    Config c;
    c.night_start = 1; c.night_end = 5;
    CHECK(isNightTime(c, makeLocal(2026,6,1,2)));
    CHECK(!isNightTime(c, makeLocal(2026,6,1,5)));
    CHECK(!isNightTime(c, makeLocal(2026,6,1,23)));
}

TEST_CASE("isNightTime equal -> always false") {
    Config c;
    c.night_start = 3; c.night_end = 3;
    CHECK(!isNightTime(c, makeLocal(2026,6,1,3)));
}

// ---------- colorFor / matchKeyword ----------

TEST_CASE("colorFor / matchKeyword") {
    Config c;
    c.normal_color = "00FF00";
    c.category_colors["restmuell"] = "202020";
    c.category_colors["bio"]       = "663300";

    CHECK(matchKeyword(c, "Restmuellbehaelter") == "restmuell");
    CHECK(matchKeyword(c, "biotonne") == "bio");
    CHECK(matchKeyword(c, "Glas").empty());

    auto col = colorFor(c, "Restmuellbehaelter");
    CHECK(col.r == 0x20); CHECK(col.g == 0x20); CHECK(col.b == 0x20);

    col = colorFor(c, "Unmapped");           // -> normal_color
    CHECK(col.r == 0); CHECK(col.g == 255); CHECK(col.b == 0);
}

// ---------- computeActive ----------

TEST_CASE("computeActive urgent / warn / night / past / acked") {
    Config c;
    c.urgent_window = 3 * 3600;
    c.warn_window   = 24 * 3600;
    c.night_start = 22; c.night_end = 7;

    std::time_t now = makeLocal(2026, 6, 1, 12);   // midday -> not night

    std::vector<Event> evs;
    evs.push_back({"past",   "x", now - 3600});
    evs.push_back({"urgent", "x", now + 30 * 60});
    evs.push_back({"warn",   "x", now + 12 * 3600});
    evs.push_back({"far",    "x", now + 48 * 3600});
    evs.push_back({"acked",  "x", now + 60 * 60});

    std::set<std::string> acked = {"acked"};
    auto out = computeActive(c, evs, acked, now);

    REQUIRE(out.size() == 2);
    CHECK(out[0].uid == "urgent");
    CHECK(out[0].urgent);
    CHECK(out[1].uid == "warn");
    CHECK(!out[1].urgent);

    // At 3 AM: night suppresses everything, even urgent -> device off.
    std::time_t night = makeLocal(2026, 6, 1, 3);
    std::vector<Event> evs2;
    evs2.push_back({"u", "x", night + 30 * 60});
    evs2.push_back({"w", "x", night + 12 * 3600});
    auto out2 = computeActive(c, evs2, {}, night);
    CHECK(out2.empty());
}

// ---------- nextByType ----------

TEST_CASE("nextByType groups by matched keyword") {
    Config c;
    c.category_colors["restmuell"] = "202020";
    c.category_colors["bio"]       = "663300";

    std::time_t now = makeLocal(2026, 6, 1, 12);

    std::vector<Event> evs;
    evs.push_back({"r1", "Restmuellbehaelter", now + 4 * 86400});
    evs.push_back({"r2", "Restmuellbehaelter", now + 1 * 86400}); // earliest restmuell
    evs.push_back({"b1", "Biomuellbehaelter",  now + 2 * 86400});
    evs.push_back({"g1", "Glas",               now + 3 * 86400}); // unmapped
    evs.push_back({"past","Restmuellbehaelter",now - 86400});     // ignored

    auto out = nextByType(c, evs, now);
    REQUIRE(out.size() == 3);
    // sorted by event time
    CHECK(out[0].keyword == "restmuell");
    CHECK(out[0].event->uid == "r2");
    CHECK(out[1].keyword == "bio");
    CHECK(out[1].event->uid == "b1");
    CHECK(out[2].keyword == "");                 // unmapped
    CHECK(out[2].event->uid == "g1");
}

// ---------- buildWledJson ----------

TEST_CASE("buildWledJson empty active -> off and segments cleared") {
    Config c;
    c.led_count = 30; c.max_segments = 4;
    auto j = buildWledJson(c, {});
    CHECK(j.find("\"on\":false") != std::string::npos);
    // four cleared segments, ids 0..3
    CHECK(j.find("\"id\":0,\"stop\":0") != std::string::npos);
    CHECK(j.find("\"id\":3,\"stop\":0") != std::string::npos);
    CHECK(j.find("\"bri\"") == std::string::npos);
}

TEST_CASE("buildWledJson one urgent -> status red + 1 event seg, rest cleared") {
    Config c;
    c.led_count = 30; c.max_segments = 4;
    c.urgent_color = "FF0000"; c.normal_color = "00FF00";

    ActiveCode a; a.uid="x"; a.start=0; a.urgent=true;
    a.color = parseHex("0040FF");
    std::vector<ActiveCode> v = {a};

    auto j = buildWledJson(c, v);
    CHECK(j.find("\"on\":true") != std::string::npos);
    CHECK(j.find("\"bri\":")   != std::string::npos);
    // status segment red ([255,0,0])
    CHECK(j.find("[[255,0,0]]") != std::string::npos);
    // event segment blue ([0,64,255])
    CHECK(j.find("[[0,64,255]]") != std::string::npos);
    // segments 2 and 3 cleared
    CHECK(j.find("\"id\":2,\"stop\":0") != std::string::npos);
    CHECK(j.find("\"id\":3,\"stop\":0") != std::string::npos);
}

TEST_CASE("buildWledJson capped at max_segments") {
    Config c;
    c.led_count = 60; c.max_segments = 3;       // 1 status + at most 2 events
    std::vector<ActiveCode> v;
    for (int i = 0; i < 5; ++i) {
        ActiveCode a; a.uid="u"; a.urgent=false; a.color={1,2,3};
        v.push_back(a);
    }
    auto j = buildWledJson(c, v);
    // no "id":3 segment should appear
    CHECK(j.find("\"id\":3") == std::string::npos);
    CHECK(j.find("\"id\":2") != std::string::npos);
}

// ---------- applyKV ----------

TEST_CASE("applyKV recognises color_<kw> and ints") {
    Config c;
    CHECK(applyKV(c, "ical_url", "http://x"));
    CHECK(c.ical_url == "http://x");
    CHECK(applyKV(c, "check_interval", "42"));
    CHECK(c.check_interval == 42);
    CHECK(applyKV(c, "color_glas", "E0E0E0"));
    CHECK(c.category_colors["glas"] == "E0E0E0");
    CHECK(!applyKV(c, "unknown_key", "x"));
    CHECK(!applyKV(c, "check_interval", "not-a-number"));
}
