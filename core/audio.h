#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <string>
#include <vector>
#include "types.h"

// Set by notification clients and consumed by the main loop.
// g_needsEndpointRefresh: device added/removed/state-changed — re-enumerate endpoints + sessions.
// g_needsSessionRefresh:  new audio session created — re-enumerate sessions only.
// g_sessionRetryUntilTick: GetTickCount64 deadline; main loop retries RefreshSessionCache every
//   tick until this passes, working around the race where OnSessionCreated fires before
//   GetSessionEnumerator includes the new session.
// All written from COM callback threads, so they must be atomic.
#include <atomic>
extern std::atomic<bool>     g_needsEndpointRefresh;
extern std::atomic<bool>     g_needsSessionRefresh;
extern std::atomic<uint64_t> g_sessionRetryUntilTick;
extern HANDLE                g_refreshEvent;

// ── Notification clients ──────────────────────────────────────────────────────

class DeviceNotificationClient : public IMMNotificationClient {
    volatile LONG m_refs = 1;
public:
    STDMETHODIMP QueryInterface(REFIID, void**) override;
    STDMETHODIMP_(ULONG) AddRef()  override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override;
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override;
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override;
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override;
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override;
};

class SessionNotificationClient : public IAudioSessionNotification {
    volatile LONG m_refs = 1;
public:
    STDMETHODIMP QueryInterface(REFIID, void**) override;
    STDMETHODIMP_(ULONG) AddRef()  override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP OnSessionCreated(IAudioSessionControl*) override;
};

// ── Audio management ──────────────────────────────────────────────────────────

void RegisterSessionNotifications(IMMDeviceEnumerator* enumerator);
void RefreshSessionCache(IMMDeviceEnumerator* enumerator,
                         const AppSettings& settings,
                         const std::vector<EndpointInfo>& endpoints);
void SyncEndpointsFromWindows();
void UpdateSessionsAndEmit(const AppSettings& settings,
                           const std::vector<EndpointInfo>& endpoints);
void SetSessionVolume(const std::wstring& name, float vol);
void SetSessionMute(const std::wstring& name, bool muted);
std::vector<EndpointInfo> EnumerateEndpoints(IMMDeviceEnumerator* enumerator);
void StartSilentStream(IMMDeviceEnumerator* enumerator);

// Returns the ID of the current default system output device.
std::wstring GetDefaultOutputId(IMMDeviceEnumerator* enumerator);

// ── System volume ─────────────────────────────────────────────────────────────

struct SystemInfo {
    float volume = 1.0f;
    bool  muted  = false;
    float peak   = 0.0f;
};

void UpdateSystemAndEmit(IMMDeviceEnumerator* enumerator);
void SetSystemVolume(IMMDeviceEnumerator* enumerator, float vol);
void SetSystemMute(IMMDeviceEnumerator* enumerator, bool muted);
void UpdateEndpointLevelsAndEmit();
void SetDeviceVolume(const std::wstring& deviceId, float vol);
void SetDeviceMute(const std::wstring& deviceId, bool muted);

// ── IPC emission ──────────────────────────────────────────────────────────────

void EmitStatus(IMMDeviceEnumerator* de);
void EmitSessions(const std::vector<SessionInfo>& sessions);
void EmitEndpoints(const std::vector<EndpointInfo>& endpoints);
