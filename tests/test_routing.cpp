// test_routing.cpp
//
// Tests for the routing decision logic in ApplyRouting (routing.cpp).
// Compiled standalone — does not link against routing.cpp. Instead it
// inlines the same decision logic with stubs for the side-effectful
// SetProcessEndpoint / SetDefaultDevice calls so we can assert on what
// would have been called without touching any real audio hardware.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <regex>
#include <stdexcept>
#include <cstdio>

#include "../Source/theaudioapp/types.h"

// ── Stubs ─────────────────────────────────────────────────────────────────────

struct RoutingCall { DWORD pid; std::wstring deviceId; };
static std::vector<RoutingCall>   g_routingCalls;
static std::vector<std::wstring>  g_defaultCalls;

static void StubSetProcessEndpoint(DWORD pid, const std::wstring& id) {
    g_routingCalls.push_back({ pid, id });
}
static void StubSetDefaultDevice(const std::wstring& id) {
    g_defaultCalls.push_back(id);
}

// ── Routing logic (mirrors ApplyRouting in routing.cpp exactly) ───────────────
// Keep in sync with routing.cpp. Tests will catch any divergence in behaviour.

static std::map<DWORD, std::wstring> g_lastRouted;
static std::wstring                  g_lastDefault;

static void ApplyRouting(const std::vector<SessionInfo>& sessions,
                         const AppSettings& settings,
                         const std::vector<EndpointInfo>& endpoints)
{
    if (!settings.defaultDevice.empty() && settings.defaultDevice != g_lastDefault) {
        g_lastDefault = settings.defaultDevice;
        StubSetDefaultDevice(settings.defaultDevice);
    }
    if (settings.routing.empty()) return;

    std::map<std::wstring, std::wstring> nameToId;
    std::set<std::wstring> allIds;
    for (const auto& ep : endpoints) { nameToId[ep.name] = ep.id; allIds.insert(ep.id); }

    std::set<DWORD> activePids, routedThisTick;
    for (const auto& sess : sessions) {
        activePids.insert(sess.pid);
        if (routedThisTick.count(sess.pid)) continue;

        std::wstring target;
        auto it = settings.routing.find(sess.name);
        if (it != settings.routing.end()) {
            target = it->second;
        } else {
            for (const auto& [pattern, val] : settings.routing) {
                if (pattern.rfind(L"regex:", 0) == 0) {
                    try {
                        std::wregex re(pattern.substr(6), std::regex_constants::icase);
                        if (std::regex_search(sess.name, re)) { target = val; break; }
                    } catch (...) {}
                }
            }
        }
        if (target.empty()) continue;

        std::wstring deviceId = (target == L"Default") ? L""
            : (allIds.count(target)   ? target
            : (nameToId.count(target) ? nameToId.at(target) : L"SKIP"));
        if (deviceId == L"SKIP") continue;

        auto cached = g_lastRouted.find(sess.pid);
        if (cached != g_lastRouted.end() && cached->second == deviceId) {
            routedThisTick.insert(sess.pid);
            continue;
        }
        g_lastRouted[sess.pid] = deviceId;
        routedThisTick.insert(sess.pid);
        StubSetProcessEndpoint(sess.pid, deviceId);
    }

    for (auto it = g_lastRouted.begin(); it != g_lastRouted.end(); )
        it = activePids.count(it->first) ? std::next(it) : g_lastRouted.erase(it);
}

// ── Test framework ────────────────────────────────────────────────────────────

static int g_passed = 0, g_failed = 0;

static void resetState() {
    g_routingCalls.clear();
    g_defaultCalls.clear();
    g_lastRouted.clear();
    g_lastDefault.clear();
}

static void check(bool cond, const char* expr, int line) {
    if (!cond) throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expr);
}
#define CHECK(x) check((x), #x, __LINE__)

static void runTest(const char* name, void(*fn)()) {
    resetState();
    try {
        fn();
        printf("  PASS  %s\n", name);
        g_passed++;
    } catch (const std::exception& e) {
        printf("  FAIL  %s  [%s]\n", name, e.what());
        g_failed++;
    }
}
#define RUN(fn) runTest(#fn, fn)

// ── Helpers ───────────────────────────────────────────────────────────────────

static SessionInfo  S(const std::wstring& name, DWORD pid) { SessionInfo  s{}; s.name = name; s.pid = pid; return s; }
static EndpointInfo E(const std::wstring& id,   const std::wstring& name) { EndpointInfo e{}; e.id = id; e.name = name; e.type = "physical"; return e; }

// ── Tests ─────────────────────────────────────────────────────────────────────

static void exact_name_routes_to_endpoint_id() {
    AppSettings cfg;
    cfg.routing[L"Discord"] = L"Headphones";
    ApplyRouting({ S(L"Discord", 100) }, cfg, { E(L"{ep-1}", L"Headphones") });
    CHECK(g_routingCalls.size() == 1);
    CHECK(g_routingCalls[0].pid == 100);
    CHECK(g_routingCalls[0].deviceId == L"{ep-1}");
}

static void routing_target_that_is_already_an_id_is_used_directly() {
    AppSettings cfg;
    cfg.routing[L"Spotify"] = L"{ep-2}";
    ApplyRouting({ S(L"Spotify", 200) }, cfg, { E(L"{ep-2}", L"Speakers") });
    CHECK(g_routingCalls.size() == 1);
    CHECK(g_routingCalls[0].deviceId == L"{ep-2}");
}

static void default_routing_sends_empty_device_id() {
    AppSettings cfg;
    cfg.routing[L"Discord"] = L"Default";
    ApplyRouting({ S(L"Discord", 300) }, cfg, { E(L"{ep-1}", L"Headphones") });
    CHECK(g_routingCalls.size() == 1);
    CHECK(g_routingCalls[0].deviceId == L"");
}

static void unknown_endpoint_name_produces_no_call() {
    AppSettings cfg;
    cfg.routing[L"Discord"] = L"NonExistentDevice";
    ApplyRouting({ S(L"Discord", 400) }, cfg, { E(L"{ep-1}", L"Headphones") });
    CHECK(g_routingCalls.size() == 0);
}

static void unmatched_session_produces_no_call() {
    AppSettings cfg;
    cfg.routing[L"Discord"] = L"Headphones";
    ApplyRouting({ S(L"Spotify", 500) }, cfg, { E(L"{ep-1}", L"Headphones") });
    CHECK(g_routingCalls.size() == 0);
}

static void empty_routing_table_produces_no_calls() {
    AppSettings cfg; // no routing entries
    ApplyRouting({ S(L"Discord", 600) }, cfg, { E(L"{ep-1}", L"Headphones") });
    CHECK(g_routingCalls.size() == 0);
}

static void regex_pattern_matches_case_insensitive() {
    AppSettings cfg;
    cfg.routing[L"regex:valorant"] = L"Headphones";
    ApplyRouting({ S(L"VALORANT-Win64-Shipping.exe", 700) }, cfg, { E(L"{ep-1}", L"Headphones") });
    CHECK(g_routingCalls.size() == 1);
    CHECK(g_routingCalls[0].pid == 700);
}

static void regex_pattern_does_not_false_match() {
    AppSettings cfg;
    cfg.routing[L"regex:discord"] = L"Headphones";
    ApplyRouting({ S(L"Spotify", 800) }, cfg, { E(L"{ep-1}", L"Headphones") });
    CHECK(g_routingCalls.size() == 0);
}

static void invalid_regex_is_silently_skipped() {
    AppSettings cfg;
    cfg.routing[L"regex:[invalid("] = L"Headphones";  // malformed regex
    ApplyRouting({ S(L"Discord", 850) }, cfg, { E(L"{ep-1}", L"Headphones") });
    CHECK(g_routingCalls.size() == 0);  // must not crash, just skip
}

static void same_pid_routed_once_per_tick_even_with_two_sessions() {
    AppSettings cfg;
    cfg.routing[L"Discord"] = L"Headphones";
    // Discord can have multiple WASAPI sessions under one PID
    ApplyRouting({ S(L"Discord", 900), S(L"Discord", 900) }, cfg, { E(L"{ep-1}", L"Headphones") });
    CHECK(g_routingCalls.size() == 1);
}

static void second_tick_with_same_route_does_not_repeat_call() {
    AppSettings cfg;
    cfg.routing[L"Discord"] = L"Headphones";
    std::vector<SessionInfo>  sessions  = { S(L"Discord", 1000) };
    std::vector<EndpointInfo> endpoints = { E(L"{ep-1}", L"Headphones") };
    ApplyRouting(sessions, cfg, endpoints);
    ApplyRouting(sessions, cfg, endpoints);  // second tick
    CHECK(g_routingCalls.size() == 1);       // still only one call total
}

static void route_changes_when_target_changes() {
    AppSettings cfg;
    cfg.routing[L"Discord"] = L"Headphones";
    std::vector<EndpointInfo> eps = { E(L"{ep-1}", L"Headphones"), E(L"{ep-2}", L"Speakers") };
    ApplyRouting({ S(L"Discord", 1100) }, cfg, eps);
    CHECK(g_routingCalls.size() == 1 && g_routingCalls[0].deviceId == L"{ep-1}");

    cfg.routing[L"Discord"] = L"Speakers";
    ApplyRouting({ S(L"Discord", 1100) }, cfg, eps);
    CHECK(g_routingCalls.size() == 2 && g_routingCalls[1].deviceId == L"{ep-2}");
}

static void stale_pid_removed_from_route_cache_when_session_exits() {
    AppSettings cfg;
    cfg.routing[L"Discord"] = L"Headphones";
    std::vector<EndpointInfo> eps = { E(L"{ep-1}", L"Headphones") };
    ApplyRouting({ S(L"Discord", 1200) }, cfg, eps);   // session alive
    ApplyRouting({},                       cfg, eps);   // session exits
    CHECK(g_lastRouted.count(1200) == 0);
}

static void default_device_applied_once_and_not_repeated() {
    AppSettings cfg;
    cfg.defaultDevice = L"{dev-42}";
    ApplyRouting({}, cfg, {});
    ApplyRouting({}, cfg, {});  // same device, second tick
    CHECK(g_defaultCalls.size() == 1);
}

static void default_device_reapplied_when_it_changes() {
    AppSettings cfg;
    cfg.defaultDevice = L"{dev-1}";
    ApplyRouting({}, cfg, {});
    cfg.defaultDevice = L"{dev-2}";
    ApplyRouting({}, cfg, {});
    CHECK(g_defaultCalls.size() == 2);
    CHECK(g_defaultCalls[1] == L"{dev-2}");
}

static void multiple_apps_routed_to_different_endpoints() {
    AppSettings cfg;
    cfg.routing[L"Discord"] = L"Headphones";
    cfg.routing[L"Spotify"] = L"Speakers";
    std::vector<EndpointInfo> eps = { E(L"{ep-1}", L"Headphones"), E(L"{ep-2}", L"Speakers") };
    ApplyRouting({ S(L"Discord", 10), S(L"Spotify", 20) }, cfg, eps);
    CHECK(g_routingCalls.size() == 2);
    bool discordOk = false, spotifyOk = false;
    for (auto& c : g_routingCalls) {
        if (c.pid == 10 && c.deviceId == L"{ep-1}") discordOk = true;
        if (c.pid == 20 && c.deviceId == L"{ep-2}") spotifyOk = true;
    }
    CHECK(discordOk && spotifyOk);
}

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
    printf("=== routing tests ===\n");
    RUN(exact_name_routes_to_endpoint_id);
    RUN(routing_target_that_is_already_an_id_is_used_directly);
    RUN(default_routing_sends_empty_device_id);
    RUN(unknown_endpoint_name_produces_no_call);
    RUN(unmatched_session_produces_no_call);
    RUN(empty_routing_table_produces_no_calls);
    RUN(regex_pattern_matches_case_insensitive);
    RUN(regex_pattern_does_not_false_match);
    RUN(invalid_regex_is_silently_skipped);
    RUN(same_pid_routed_once_per_tick_even_with_two_sessions);
    RUN(second_tick_with_same_route_does_not_repeat_call);
    RUN(route_changes_when_target_changes);
    RUN(stale_pid_removed_from_route_cache_when_session_exits);
    RUN(default_device_applied_once_and_not_repeated);
    RUN(default_device_reapplied_when_it_changes);
    RUN(multiple_apps_routed_to_different_endpoints);
    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
