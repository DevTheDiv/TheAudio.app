#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winstring.h>
#include <activation.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <sstream>
#include <map>
#include <set>
#include <regex>
#include <vector>
#include <chrono>
#include "routing.h"
#include "utils.h"
#include "PolicyConfig.h"

static std::map<DWORD, std::wstring>                          g_lastRouted;
static std::map<DWORD, std::chrono::steady_clock::time_point> g_lastRoutedTime;
static std::map<std::pair<DWORD,std::wstring>, int>           g_routeAttempts; // (pid,deviceId) → tries
static std::wstring                                           g_lastDefault;

// ── WinRT AudioPolicyConfig (native per-app routing) ─────────────────────────

typedef HRESULT (WINAPI* PFN_RoInitialize)(UINT32);
typedef HRESULT (WINAPI* PFN_RoGetActivationFactory)(HSTRING, REFIID, void**);
typedef HRESULT (WINAPI* PFN_WindowsCreateString)(PCNZWCH, UINT32, HSTRING*);
typedef HRESULT (WINAPI* PFN_WindowsDeleteString)(HSTRING);
typedef PCWSTR  (WINAPI* PFN_WindowsGetStringRawBuffer)(HSTRING, UINT32*);
// SetPersistedDefaultAudioEndpoint(processId, flow, role, deviceId)
// deviceId is HSTRING using the full device interface path (see WrapDeviceIdForAPC).
typedef HRESULT (__stdcall* PFN_SetImmersiveAudioEndpoint)(void*, UINT32, INT, INT, HSTRING);
// GetPersistedDefaultAudioEndpoint(processId, flow, role, deviceId*)
typedef HRESULT (__stdcall* PFN_GetImmersiveAudioEndpoint)(void*, UINT32, INT, INT, HSTRING*);

static HMODULE  g_hCombase                      = nullptr;
static void*    g_pAudioPolicyStatics            = nullptr;
static PFN_WindowsCreateString       g_CreateStr = nullptr;
static PFN_WindowsDeleteString       g_DeleteStr = nullptr;
static PFN_WindowsGetStringRawBuffer g_GetBuf    = nullptr;

// IAudioPolicyConfigFactoryVariantFor21H2 — Win11 21H2+ (build >=19044)
// Has 19 __incomplete__ stubs at slots 6-24; SetPersistedDefaultAudioEndpoint at slot 25.
static const GUID IID_APC_21H2 =
    {0xAB3D4648,0xE242,0x459F,{0xB0,0x2F,0x54,0x1C,0x70,0x30,0x63,0x24}};
// IAudioPolicyConfigFactoryVariantForDownlevel — older builds
// No stubs; SetPersistedDefaultAudioEndpoint at slot 6.
static const GUID IID_APC_Downlevel =
    {0xa45429a4,0xaa63,0x4480,{0xb7,0xf8,0x3f,0x25,0x52,0xda,0xee,0x93}};

static int g_slotSetPersisted = -1; // set on first successful factory acquisition

// SEH-isolated call — no C++ objects in __try scope
static HRESULT CallSetImmersive(void* obj, UINT32 pid, INT flow, INT role, HSTRING hDev) {
    if (g_slotSetPersisted < 0) return E_FAIL;
    void** vtbl = *(void***)obj;
    auto fn = (PFN_SetImmersiveAudioEndpoint)vtbl[g_slotSetPersisted];
    __try { return fn(obj, pid, flow, role, hDev); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
}

// Wrap MMDevice ID into the full device interface path that AudioPolicyConfig expects.
// EarTrumpet: "\\?\SWD#MMDEVAPI#" + mmId + "#{e6327cad-dcec-4949-ae8a-991e976a79d2}"
static std::wstring WrapDeviceIdForAPC(const std::wstring& mmId) {
    return L"\\\\?\\SWD#MMDEVAPI#" + mmId + L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
}

static void* GetAudioPolicyStatics() {
    if (g_pAudioPolicyStatics) return g_pAudioPolicyStatics;

    if (!g_hCombase) {
        g_hCombase = LoadLibraryW(L"combase.dll");
        if (!g_hCombase) return nullptr;
        auto roInit = (PFN_RoInitialize)GetProcAddress(g_hCombase, "RoInitialize");
        g_CreateStr = (PFN_WindowsCreateString)GetProcAddress(g_hCombase, "WindowsCreateString");
        g_DeleteStr = (PFN_WindowsDeleteString)GetProcAddress(g_hCombase, "WindowsDeleteString");
        g_GetBuf    = (PFN_WindowsGetStringRawBuffer)GetProcAddress(g_hCombase, "WindowsGetStringRawBuffer");
        if (roInit) roInit(1); // RO_INIT_MULTITHREADED — S_FALSE if already init'd, that's fine
    }

    auto roGetFactory = (PFN_RoGetActivationFactory)GetProcAddress(g_hCombase, "RoGetActivationFactory");
    if (!roGetFactory || !g_CreateStr || !g_DeleteStr) return nullptr;

    // Activate: get IActivationFactory first (always works)
    const wchar_t* cn = L"Windows.Media.Internal.AudioPolicyConfig";
    HSTRING hCls = nullptr;
    g_CreateStr(cn, (UINT32)wcslen(cn), &hCls);
    IActivationFactory* factory = nullptr;
    HRESULT hr = roGetFactory(hCls, IID_IActivationFactory, (void**)&factory);
    g_DeleteStr(hCls);
    if (FAILED(hr) || !factory) return nullptr;

    // Try version-specific IIDs. The slot for SetPersistedDefaultAudioEndpoint differs:
    //   21H2+ variant: 19 __incomplete__ stubs at slots 6-24 → SetPersisted at slot 25
    //   Downlevel variant: SetPersisted at slot 6
    void* pStatics = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_APC_21H2, (void**)&pStatics)) && pStatics) {
        g_slotSetPersisted = 25;
        fprintf(stderr, "[routing] WinRT: 21H2 interface, slot 25\n");
    } else if (SUCCEEDED(factory->QueryInterface(IID_APC_Downlevel, (void**)&pStatics)) && pStatics) {
        g_slotSetPersisted = 6;
        fprintf(stderr, "[routing] WinRT: downlevel interface, slot 6\n");
    }

    // Fallback: scan GetIids() for future build compatibility
    if (!pStatics) {
        ULONG count = 0; IID* pIids = nullptr;
        if (SUCCEEDED(factory->GetIids(&count, &pIids))) {
            for (ULONG i = 0; i < count && !pStatics; i++) {
                void* p = nullptr;
                if (SUCCEEDED(factory->QueryInterface(pIids[i], (void**)&p)) && p) {
                    if ((void*)p != (void*)factory) {
                        pStatics = p;
                        g_slotSetPersisted = 25; // assume 21H2+ layout for unknown future builds
                        fprintf(stderr, "[routing] WinRT: fallback interface IID scan, assuming slot 25\n");
                    } else ((IUnknown*)p)->Release();
                }
            }
            CoTaskMemFree(pIids);
        }
    }

    factory->Release();
    g_pAudioPolicyStatics = pStatics;
    return pStatics;
}

// Route a live running session to a new endpoint via WinRT AudioPolicyConfig.
// deviceId="" clears the per-app override. Returns true on success.
static bool SetImmersiveEndpoint(DWORD pid, const std::wstring& deviceId) {
    void* statics = GetAudioPolicyStatics();
    if (!statics || !g_CreateStr || !g_DeleteStr) {
        fprintf(stderr, "[routing] SetImmersiveEndpoint: no statics (pid=%u)\n", pid);
        return false;
    }

    HSTRING hDev = nullptr;
    if (!deviceId.empty()) {
        std::wstring wrapped = WrapDeviceIdForAPC(deviceId);
        g_CreateStr(wrapped.c_str(), (UINT32)wrapped.size(), &hDev);
    }
    // nullptr hDev = clear per-app override (always succeeds)

    bool ok = false;
    for (int role : {0, 1, 2}) {
        HRESULT hr = CallSetImmersive(statics, (UINT32)pid, 0 /*eRender*/, role, hDev);
        if (SUCCEEDED(hr)) ok = true;
        else fprintf(stderr, "[routing] WinRT role=%d hr=0x%08X pid=%u\n", role, (unsigned)hr, pid);
    }
    if (hDev) g_DeleteStr(hDev);
    fprintf(stderr, "[routing] WinRT pid=%u ok=%d\n", pid, (int)ok);
    return ok;
}

// SEH wrapper for GetPersistedDefaultAudioEndpoint — no C++ objects in __try scope
static HRESULT CallGetImmersive(void* obj, int slot, UINT32 pid, HSTRING* hOut) {
    void** vtbl = *(void***)obj;
    __try { return ((PFN_GetImmersiveAudioEndpoint)vtbl[slot])(obj, pid, 0, 1, hOut); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
}

// Query Windows' persisted routing for a PID. Returns plain MMDevice ID, or "" if none.
// Strips \\?\SWD#MMDEVAPI# prefix and #{e6327cad-...} suffix (inverse of WrapDeviceIdForAPC).
std::wstring GetImmersiveEndpoint(DWORD pid) {
    void* statics = GetAudioPolicyStatics();
    if (!statics || g_slotSetPersisted < 0 || !g_DeleteStr || !g_GetBuf) return L"";

    HSTRING hOut = nullptr;
    HRESULT hr = CallGetImmersive(statics, g_slotSetPersisted + 1, (UINT32)pid, &hOut);
    if (FAILED(hr) || !hOut) return L"";

    UINT32 len = 0;
    PCWSTR buf = g_GetBuf(hOut, &len);
    std::wstring s = (buf && len > 0) ? std::wstring(buf, len) : L"";
    g_DeleteStr(hOut);
    if (s.empty()) return L"";

    // Strip prefix
    static const std::wstring pfx = L"\\\\?\\SWD#MMDEVAPI#";
    if (s.rfind(pfx, 0) == 0) s = s.substr(pfx.size());
    // Strip render interface suffix
    static const std::wstring sfx = L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
    auto pos = s.rfind(sfx);
    if (pos != std::wstring::npos) s = s.substr(0, pos);
    return s;
}

// ── Public API ────────────────────────────────────────────────────────────────

void SetDefaultDevice(const std::wstring& id) {
    IPolicyConfig* pPC = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr, CLSCTX_ALL,
                                   __uuidof(IPolicyConfig), (void**)&pPC)) && pPC) {
        for (ERole r : {eConsole, eMultimedia, eCommunications}) {
            HRESULT hr = pPC->SetDefaultEndpoint(id.c_str(), r);
            fprintf(stderr, "[routing] SetDefaultEndpoint role=%d -> 0x%08X\n", (int)r, (unsigned)hr);
        }
        pPC->Release();
    }
}

void SetProcessEndpoint(DWORD pid, const std::wstring& deviceId, const std::wstring& exeName) {
    fprintf(stderr, "[routing] -> pid=%u exe=%S dev=...%S\n",
        pid, exeName.c_str(),
        deviceId.size() > 8 ? deviceId.c_str() + deviceId.size() - 8 : deviceId.c_str());

    SetImmersiveEndpoint(pid, deviceId); // SetPersistedDefaultAudioEndpoint: moves live session + persists for restart
}

void ApplyRouting(const std::vector<SessionInfo>& sessions, const AppSettings& settings, const std::vector<EndpointInfo>& endpoints) {
    if (!settings.defaultDevice.empty() && settings.defaultDevice != g_lastDefault) {
        g_lastDefault = settings.defaultDevice;
        SetDefaultDevice(settings.defaultDevice);
    }
    if (settings.routing.empty()) return;

    std::map<std::wstring, std::wstring> nameToId;
    std::set<std::wstring> allIds;
    for (const auto& ep : endpoints) { nameToId[ep.name] = ep.id; allIds.insert(ep.id); }

    auto now = std::chrono::steady_clock::now();
    std::set<DWORD> activePids, routedThisTick;

    for (const auto& sess : sessions) {
        if (sess.pid == 0) continue; // System Sounds has no per-app audio policy
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
        if (target == L"Default") {
            // Clear any persisted WinRT override so the app returns to system default.
            // Only call once per PID — g_lastRouted[""] marks "already cleared".
            auto it = g_lastRouted.find(sess.pid);
            if (it == g_lastRouted.end() || !it->second.empty()) {
                g_lastRouted[sess.pid]     = L"";
                g_lastRoutedTime[sess.pid] = now;
                SetImmersiveEndpoint(sess.pid, L"");
            }
            routedThisTick.insert(sess.pid);
            continue;
        }

        std::wstring deviceId = (allIds.count(target) ? target : (nameToId.count(target) ? nameToId.at(target) : L"SKIP"));
        if (deviceId == L"SKIP") continue;

        auto cached = g_lastRouted.find(sess.pid);
        if (cached != g_lastRouted.end() && cached->second == deviceId) {
            bool onTarget = sess.endpointId.empty() || sess.endpointId == deviceId;
            if (onTarget) { routedThisTick.insert(sess.pid); continue; }

            // Session drifted. Retry at most once (after 30s), then give up.
            // Routing can't force live WASAPI sessions to move; policy is set for next open.
            auto key = std::make_pair(sess.pid, deviceId);
            if (g_routeAttempts[key] >= 2) {
                routedThisTick.insert(sess.pid); // gave up — policy set, wait for app restart
                continue;
            }
            if (now - g_lastRoutedTime[sess.pid] < std::chrono::seconds(30)) {
                routedThisTick.insert(sess.pid); continue;
            }
            fprintf(stderr, "[routing] drift pid=%u attempt=%d\n", sess.pid, g_routeAttempts[key] + 1);
            g_lastRouted.erase(sess.pid); // fall through to re-route
        }

        g_lastRouted[sess.pid]     = deviceId;
        g_lastRoutedTime[sess.pid] = now;
        g_routeAttempts[{sess.pid, deviceId}]++;
        routedThisTick.insert(sess.pid);
        SetProcessEndpoint(sess.pid, deviceId, sess.name);
    }

    for (auto it = g_lastRouted.begin(); it != g_lastRouted.end(); ) {
        if (!activePids.count(it->first)) {
            g_lastRoutedTime.erase(it->first);
            it = g_lastRouted.erase(it);
        } else { ++it; }
    }
    for (auto it = g_routeAttempts.begin(); it != g_routeAttempts.end(); ) {
        if (!activePids.count(it->first.first)) it = g_routeAttempts.erase(it);
        else ++it;
    }
}
