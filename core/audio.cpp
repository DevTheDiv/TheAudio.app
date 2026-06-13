#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <psapi.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <iostream>
#include <sstream>
#include <mutex>
#include <thread>
#include "audio.h"
#include "routing.h"
#include "utils.h"
#include "channels.h"

// ── Shared globals ────────────────────────────────────────────────────────────
std::atomic<bool>     g_needsEndpointRefresh{ true };
std::atomic<bool>     g_needsSessionRefresh{ false };
std::atomic<uint64_t> g_sessionRetryUntilTick{ 0 };
HANDLE                g_refreshEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

// ── File-private state ────────────────────────────────────────────────────────
static std::vector<CachedSession>              g_sessionCache;
static std::mutex                              g_cacheMutex;
static SessionNotificationClient*              g_sessNotify = new SessionNotificationClient();
static std::map<std::wstring, IAudioMeterInformation*> g_endpointMeters;
static std::map<std::wstring, IAudioEndpointVolume*>   g_endpointVolumes;
static std::mutex                                      g_meterMutex;


static std::wstring GetExePath(DWORD pid) {
    if (pid == 0) return L"";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[MAX_PATH] = {}; DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) { CloseHandle(h); return std::wstring(buf); }
    CloseHandle(h); return L"";
}

static std::wstring GetProcessName(DWORD pid) {
    if (pid == 0) return L"System Sounds";
    std::wstring p = GetExePath(pid);
    if (p.empty()) return L"Unknown";
    auto pos = p.rfind(L'\\');
    return pos != std::wstring::npos ? p.substr(pos + 1) : p;
}

// ── Notification client implementations ──────────────────────────────────────

STDMETHODIMP DeviceNotificationClient::QueryInterface(REFIID riid, void** ppv) {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient))
        { *ppv = this; AddRef(); return S_OK; }
    *ppv = nullptr; return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) DeviceNotificationClient::AddRef()  { return InterlockedIncrement(&m_refs); }
STDMETHODIMP_(ULONG) DeviceNotificationClient::Release() { LONG r = InterlockedDecrement(&m_refs); if (!r) delete this; return r; }
STDMETHODIMP DeviceNotificationClient::OnDeviceStateChanged(LPCWSTR, DWORD)              { g_needsEndpointRefresh = true; SetEvent(g_refreshEvent); return S_OK; }
STDMETHODIMP DeviceNotificationClient::OnDeviceAdded(LPCWSTR)                            { g_needsEndpointRefresh = true; SetEvent(g_refreshEvent); return S_OK; }
STDMETHODIMP DeviceNotificationClient::OnDeviceRemoved(LPCWSTR)                          { g_needsEndpointRefresh = true; SetEvent(g_refreshEvent); return S_OK; }
STDMETHODIMP DeviceNotificationClient::OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) { g_needsEndpointRefresh = true; SetEvent(g_refreshEvent); return S_OK; }
STDMETHODIMP DeviceNotificationClient::OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) { return S_OK; }

STDMETHODIMP SessionNotificationClient::QueryInterface(REFIID riid, void** ppv) {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionNotification))
        { *ppv = this; AddRef(); return S_OK; }
    *ppv = nullptr; return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) SessionNotificationClient::AddRef()  { return InterlockedIncrement(&m_refs); }
STDMETHODIMP_(ULONG) SessionNotificationClient::Release() { LONG r = InterlockedDecrement(&m_refs); if (!r) delete this; return r; }
STDMETHODIMP SessionNotificationClient::OnSessionCreated(IAudioSessionControl*) {
    // OnSessionCreated fires before GetSessionEnumerator includes the new session.
    // Set a 2-second retry window so the main loop keeps calling RefreshSessionCache
    // until the session actually shows up in the enumerator.
    g_sessionRetryUntilTick = GetTickCount64() + 2000;
    g_needsSessionRefresh = true;
    SetEvent(g_refreshEvent);
    return S_OK;
}

// ── Audio management ──────────────────────────────────────────────────────────

void RegisterSessionNotifications(IMMDeviceEnumerator* enumerator) {
    IMMDeviceCollection* col = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return;
    UINT count = 0; col->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(col->Item(i, &dev))) {
            IAudioSessionManager2* mgr = nullptr;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)))
                { mgr->RegisterSessionNotification(g_sessNotify); mgr->Release(); }
            dev->Release();
        }
    }
    col->Release();
}


void RefreshSessionCache(IMMDeviceEnumerator* enumerator,
                         const AppSettings& settings,
                         const std::vector<EndpointInfo>& endpoints)
{
    std::vector<CachedSession> next;
    std::set<std::wstring> seenIds;
    IMMDeviceCollection* col = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return;
    UINT devCount = 0; col->GetCount(&devCount);

    for (UINT d = 0; d < devCount; d++) {
        IMMDevice* device = nullptr;
        if (FAILED(col->Item(d, &device))) continue;

        LPWSTR devIdStr = nullptr; device->GetId(&devIdStr);
        std::wstring deviceId = devIdStr ? devIdStr : L"";
        if (devIdStr) CoTaskMemFree(devIdStr);

        std::wstring devName = L"Unknown";
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                devName = pv.pwszVal;
            PropVariantClear(&pv);
            props->Release();
        }

        IAudioSessionManager2* mgr = nullptr;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr))) {
            IAudioSessionEnumerator* sessEnum = nullptr;
            if (SUCCEEDED(mgr->GetSessionEnumerator(&sessEnum))) {
                int countSess = 0; sessEnum->GetCount(&countSess);
                for (int i = 0; i < countSess; i++) {
                    IAudioSessionControl* ctrl = nullptr;
                    if (FAILED(sessEnum->GetSession(i, &ctrl))) continue;
                    IAudioSessionControl2* ctrl2 = nullptr;
                    ctrl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&ctrl2);
                    ctrl->Release();
                    if (!ctrl2) continue;

                    LPWSTR idStr = nullptr; ctrl2->GetSessionIdentifier(&idStr);
                    std::wstring id = idStr ? idStr : L"";
                    if (idStr) CoTaskMemFree(idStr);
                    if (!id.empty() && seenIds.count(id)) { ctrl2->Release(); continue; }
                    if (!id.empty()) seenIds.insert(id);

                    DWORD pid = 0; ctrl2->GetProcessId(&pid);
                    if (pid == GetCurrentProcessId()) { ctrl2->Release(); continue; }
                    ISimpleAudioVolume*     vol   = nullptr; ctrl2->QueryInterface(__uuidof(ISimpleAudioVolume),     (void**)&vol);
                    IAudioMeterInformation* meter = nullptr; ctrl2->QueryInterface(__uuidof(IAudioMeterInformation), (void**)&meter);

                    SessionInfo info{};
                    info.id       = id;
                    info.exePath  = GetExePath(pid);
                    info.name     = GetProcessName(pid);
                    info.pid       = pid;
                    info.parentPid = GetParentPid(pid);
                    info.endpoint  = devName;
                    info.endpointId = deviceId; // WASAPI: where session currently plays
                    // Override with Windows' persisted preference if set — reflects external changes
                    std::wstring winrtId = GetImmersiveEndpoint(pid);
                    if (!winrtId.empty()) info.endpointId = winrtId;
                    if (vol) { vol->GetMasterVolume(&info.volume); BOOL m = FALSE; vol->GetMute(&m); info.muted = (m != FALSE); }
                    next.push_back({ ctrl2, vol, meter, info, true });
                }
                sessEnum->Release();
            }
            mgr->Release();
        }
        device->Release();
    }
    col->Release();

    std::lock_guard<std::mutex> lk(g_cacheMutex);
    for (auto& s : g_sessionCache) {
        if (s.ctrl)  s.ctrl->Release();
        if (s.vol)   s.vol->Release();
        if (s.meter) s.meter->Release();
    }
    g_sessionCache = std::move(next);
}

void UpdateSessionsAndEmit(const AppSettings& settings, const std::vector<EndpointInfo>& endpoints) {
    std::vector<SessionInfo> toEmit;
    {
        std::lock_guard<std::mutex> lk(g_cacheMutex);
        for (auto& s : g_sessionCache) {
            if (!s.ctrl) continue;
            AudioSessionState state;
            if (FAILED(s.ctrl->GetState(&state)) || state == AudioSessionStateExpired)
                { s.valid = false; continue; }
            if (s.vol) {
                s.vol->GetMasterVolume(&s.info.volume);
                BOOL m = FALSE; s.vol->GetMute(&m); s.info.muted = (m != FALSE);
            }
            if (s.meter) s.meter->GetPeakValue(&s.info.peak);
            toEmit.push_back(s.info);
        }
    }
    // Clear endpointId for sessions with no explicit routing rule or "Default" routing.
    // GetPersistedDefaultAudioEndpoint returns the resolved system default device ID when no
    // override is set — we use settings.routing as the source of truth instead.
    for (auto& s : toEmit) {
        auto it = settings.routing.find(s.name);
        if (it == settings.routing.end() || it->second.empty() || it->second == L"Default")
            s.endpointId = L"";
    }
    ApplyRouting(toEmit, settings, endpoints);
    EmitSessions(toEmit);
}

void SetSessionVolume(const std::wstring& name, float vol) {
    std::lock_guard<std::mutex> lk(g_cacheMutex);
    for (auto& s : g_sessionCache)
        if (s.valid && s.vol && s.info.name == name)
            s.vol->SetMasterVolume(vol, nullptr);
}

void SetSessionMute(const std::wstring& name, bool muted) {
    std::lock_guard<std::mutex> lk(g_cacheMutex);
    for (auto& s : g_sessionCache)
        if (s.valid && s.vol && s.info.name == name)
            s.vol->SetMute(muted ? TRUE : FALSE, nullptr);
}

// Re-query WinRT persisted routing for all cached sessions. Cheap — no WASAPI enum.
// Call periodically to pick up changes made by Windows Settings or other apps.
void SyncEndpointsFromWindows() {
    std::lock_guard<std::mutex> lk(g_cacheMutex);
    for (auto& s : g_sessionCache) {
        if (!s.valid || !s.ctrl || s.info.pid == 0) continue;
        std::wstring winrtId = GetImmersiveEndpoint(s.info.pid);
        if (!winrtId.empty()) s.info.endpointId = winrtId;
    }
}

std::vector<EndpointInfo> EnumerateEndpoints(IMMDeviceEnumerator* enumerator) {
    std::vector<EndpointInfo> out;
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT count = 0; col->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice* dev = nullptr;
            if (FAILED(col->Item(i, &dev))) continue;
            LPWSTR idStr = nullptr; dev->GetId(&idStr);
            std::wstring id = idStr ? idStr : L"";
            if (idStr) CoTaskMemFree(idStr);

            IPropertyStore* props = nullptr;
            std::wstring name;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT pv; PropVariantInit(&pv);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) name = pv.pwszVal;
                PropVariantClear(&pv);
                props->Release();
            }
            std::wstring n = name.empty() ? L"Unknown" : name;
            std::wstring key;
            std::string type = "physical";
            if (IsVirtualEndpoint(n.c_str())) {
                type = "virtual";
                for (const auto& vc : VIRTUAL_CHANNELS) {
                    if (n.find(vc.name) != std::wstring::npos) {
                        key = vc.key;
                        break;
                    }
                }
            }
            out.push_back({ id, n, type, key });

            // Cache peak meter and volume control for this endpoint
            IAudioMeterInformation* meter = nullptr;
            IAudioEndpointVolume*   epVol = nullptr;
            dev->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, (void**)&meter);
            dev->Activate(__uuidof(IAudioEndpointVolume),   CLSCTX_ALL, nullptr, (void**)&epVol);
            {
                std::lock_guard<std::mutex> lk(g_meterMutex);
                auto it = g_endpointMeters.find(id);
                if (it != g_endpointMeters.end() && it->second) it->second->Release();
                g_endpointMeters[id] = meter;
                auto iv = g_endpointVolumes.find(id);
                if (iv != g_endpointVolumes.end() && iv->second) iv->second->Release();
                g_endpointVolumes[id] = epVol;
            }

            dev->Release();
        }
        col->Release();
    }
    return out;
}

void StartSilentStream(IMMDeviceEnumerator* de) {
    std::thread([de]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDevice* device = nullptr;
        if (SUCCEEDED(de->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
            IAudioClient* client = nullptr;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) {
                WAVEFORMATEX* fmt = nullptr;
                client->GetMixFormat(&fmt);
                HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (hEvent && fmt && SUCCEEDED(client->Initialize(
                        AUDCLNT_SHAREMODE_SHARED,
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                        0, 0, fmt, nullptr))) {
                    client->SetEventHandle(hEvent);
                    IAudioSessionControl* sess = nullptr;
                    if (SUCCEEDED(client->GetService(__uuidof(IAudioSessionControl), (void**)&sess)))
                        { sess->SetDisplayName(L"TheAudio.app", nullptr); sess->Release(); }
                    IAudioRenderClient* render = nullptr;
                    if (SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), (void**)&render))) {
                        client->Start();
                        while (WaitForSingleObject(hEvent, 2000) == WAIT_OBJECT_0) {
                            UINT32 pad = 0, sz = 0;
                            client->GetCurrentPadding(&pad);
                            client->GetBufferSize(&sz);
                            UINT32 fr = sz - pad;
                            if (fr > 0) {
                                BYTE* d = nullptr;
                                if (SUCCEEDED(render->GetBuffer(fr, &d)))
                                    render->ReleaseBuffer(fr, AUDCLNT_BUFFERFLAGS_SILENT);
                            }
                        }
                        render->Release();
                    }
                }
                if (hEvent) CloseHandle(hEvent);
                if (fmt) CoTaskMemFree(fmt);
                client->Release();
            }
            device->Release();
        }
        CoUninitialize();
    }).detach();
}

std::wstring GetDefaultOutputId(IMMDeviceEnumerator* de) {
    IMMDevice* dev = nullptr;
    if (FAILED(de->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return L"";
    LPWSTR idStr = nullptr; dev->GetId(&idStr);
    std::wstring out = idStr ? idStr : L"";
    if (idStr) CoTaskMemFree(idStr);
    dev->Release();
    return out;
}

void UpdateSystemAndEmit(IMMDeviceEnumerator* de) {
    IMMDevice* device = nullptr;
    if (FAILED(de->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return;

    SystemInfo info;
    IAudioEndpointVolume* epVol = nullptr;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&epVol))) {
        epVol->GetMasterVolumeLevelScalar(&info.volume);
        BOOL m = FALSE; epVol->GetMute(&m); info.muted = (m != FALSE);
        epVol->Release();
    }

    IAudioMeterInformation* meter = nullptr;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, (void**)&meter))) {
        meter->GetPeakValue(&info.peak);
        meter->Release();
    }

    device->Release();

    std::ostringstream j;
    j << "{\"type\":\"system\",\"volume\":" << info.volume
      << ",\"muted\":"  << (info.muted ? "true" : "false")
      << ",\"peak\":"   << info.peak << "}\n";
    std::cout << j.str() << std::flush;
}

void SetSystemVolume(IMMDeviceEnumerator* de, float vol) {
    IMMDevice* device = nullptr;
    if (FAILED(de->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return;
    IAudioEndpointVolume* epVol = nullptr;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&epVol))) {
        epVol->SetMasterVolumeLevelScalar(vol, nullptr);
        epVol->Release();
    }
    device->Release();
}

void SetSystemMute(IMMDeviceEnumerator* de, bool muted) {
    IMMDevice* device = nullptr;
    if (FAILED(de->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return;
    IAudioEndpointVolume* epVol = nullptr;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&epVol))) {
        epVol->SetMute(muted ? TRUE : FALSE, nullptr);
        epVol->Release();
    }
    device->Release();
}

void UpdateEndpointLevelsAndEmit() {
    std::ostringstream j;
    j << "{\"type\":\"endpointLevels\",\"levels\":{";
    bool first = true;
    {
        std::lock_guard<std::mutex> lk(g_meterMutex);
        for (auto& [id, meter] : g_endpointMeters) {
            float peak = 0.0f, vol = 1.0f;
            BOOL  muted = FALSE;
            if (meter) meter->GetPeakValue(&peak);
            auto iv = g_endpointVolumes.find(id);
            if (iv != g_endpointVolumes.end() && iv->second) {
                iv->second->GetMasterVolumeLevelScalar(&vol);
                iv->second->GetMute(&muted);
            }
            if (!first) j << ",";
            j << JsonStr(WstrToUtf8(id))
              << ":{\"peak\":" << peak
              << ",\"volume\":"  << vol
              << ",\"muted\":"   << (muted ? "true" : "false") << "}";
            first = false;
        }
    }
    j << "}}\n";
    std::cout << j.str() << std::flush;
}

void SetDeviceVolume(const std::wstring& deviceId, float vol) {
    std::lock_guard<std::mutex> lk(g_meterMutex);
    auto it = g_endpointVolumes.find(deviceId);
    if (it != g_endpointVolumes.end() && it->second)
        it->second->SetMasterVolumeLevelScalar(vol, nullptr);
}

void SetDeviceMute(const std::wstring& deviceId, bool muted) {
    std::lock_guard<std::mutex> lk(g_meterMutex);
    auto it = g_endpointVolumes.find(deviceId);
    if (it != g_endpointVolumes.end() && it->second)
        it->second->SetMute(muted ? TRUE : FALSE, nullptr);
}

void EmitStatus(IMMDeviceEnumerator* de) {
    bool driverInstalled = false;
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(de->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0; col->GetCount(&n);
        for (UINT i = 0; i < n && !driverInstalled; i++) {
            IMMDevice* dev = nullptr;
            if (FAILED(col->Item(i, &dev))) continue;
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT pv; PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                    driverInstalled = IsVirtualEndpoint(pv.pwszVal);
                PropVariantClear(&pv); ps->Release();
            }
            dev->Release();
        }
        col->Release();
    }
    std::cout << "{\"type\":\"status\",\"running\":true,\"driverInstalled\":"
              << (driverInstalled ? "true" : "false") << "}\n" << std::flush;
}

void EmitSessions(const std::vector<SessionInfo>& sessions) {
    std::ostringstream j; j << "{\"type\":\"sessions\",\"sessions\":[";
    for (size_t i = 0; i < sessions.size(); i++) {
        const auto& s = sessions[i]; if (i) j << ",";
        j << "{\"id\":" << JsonStr(WstrToUtf8(s.id)) << ",\"name\":" << JsonStr(WstrToUtf8(s.name)) << ",\"exePath\":" << JsonStr(WstrToUtf8(s.exePath)) << ",\"pid\":" << s.pid << ",\"parentPid\":" << s.parentPid << ",\"volume\":" << s.volume << ",\"muted\":" << (s.muted ? "true" : "false") << ",\"peak\":" << s.peak << ",\"endpoint\":" << JsonStr(WstrToUtf8(s.endpoint)) << ",\"endpointId\":" << JsonStr(WstrToUtf8(s.endpointId)) << "}";
    }
    j << "]}\n"; std::cout << j.str() << std::flush;
}

void EmitEndpoints(const std::vector<EndpointInfo>& endpoints) {
    std::ostringstream j; j << "{\"type\":\"endpoints\",\"endpoints\":[";
    for (size_t i = 0; i < endpoints.size(); i++) {
        const auto& e = endpoints[i]; if (i) j << ",";
        j << "{\"id\":" << JsonStr(WstrToUtf8(e.id)) 
          << ",\"name\":" << JsonStr(WstrToUtf8(e.name)) 
          << ",\"type\":\"" << e.type << "\""
          << ",\"key\":" << JsonStr(WstrToUtf8(e.key)) << "}";
    }
    j << "]}\n"; std::cout << j.str() << std::flush;
}
