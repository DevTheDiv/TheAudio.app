# Undocumented Windows Audio APIs

Research notes from building per-app audio routing in TheAudio.app. None of these APIs are in the Windows SDK or officially documented by Microsoft. All findings are from reverse engineering, community research (EarTrumpet source), and live testing on Windows 11.

---

## 1. `Windows.Media.Internal.AudioPolicyConfig`

The core WinRT class that powers per-app audio routing in Windows. This is what Windows Settings and EarTrumpet use internally when you change an app's output device.

### Activation

```cpp
RoGetActivationFactory(
    HSTRING("Windows.Media.Internal.AudioPolicyConfig"),
    IID_IActivationFactory,
    &factory
)
```

Then QI the factory for one of two version-specific IIDs:

| Interface | IID | Windows build | vtable slot for SetPersisted |
|---|---|---|---|
| `IAudioPolicyConfigFactoryVariantFor21H2` | `{AB3D4648-E242-459F-B02F-541C70306324}` | 21H2+ (≥19044) | 25 |
| `IAudioPolicyConfigFactoryVariantForDownlevel` | `{a45429a4-aa63-4480-b7f8-3f2552daee93}` | Pre-21H2 | 6 |

The 21H2+ variant has 19 stub/incomplete slots at positions 6–24 before `SetPersistedDefaultAudioEndpoint` at slot 25. The downlevel variant goes straight to it at slot 6.

### `SetPersistedDefaultAudioEndpoint` (vtable slot 25 / 6)

Sets a per-process audio endpoint override. Windows routes all audio from that process to the specified device, and persists the choice so it survives app restarts.

**Signature:**
```cpp
HRESULT SetPersistedDefaultAudioEndpoint(
    UINT32  processId,
    INT     flow,    // 0 = eRender, 1 = eCapture
    INT     role,    // 0 = eConsole, 1 = eMultimedia, 2 = eCommunications
    HSTRING deviceId // full device interface path, or nullptr to clear
);
```

Must be called for all three roles (0, 1, 2) for the change to take full effect.

Pass `nullptr` as `deviceId` to **clear** the override — the app reverts to following the system default device.

### `GetPersistedDefaultAudioEndpoint` (vtable slot 26 / 7)

Reads back the persisted routing for a process.

**Signature:**
```cpp
HRESULT GetPersistedDefaultAudioEndpoint(
    UINT32   processId,
    INT      flow,
    INT      role,
    HSTRING* deviceId  // out
);
```

**Important gotcha:** When no per-app override is set, this does NOT return null/empty. It returns the current system default device's full path. There is no way to distinguish "explicitly routed to Headphones" from "on Headphones because that's the system default" using this API alone — you must track your own routing state.

### Device ID format

This is the most common failure point. The function requires the **full device interface path**, not the plain MMDevice ID returned by `IMMDevice::GetId()`.

| Format | Example | Works? |
|---|---|---|
| Plain MMDevice ID | `{0.0.0.00000000}.{guid}` | ❌ `E_INVALIDARG` |
| Full interface path | `\\?\SWD#MMDEVAPI#{0.0.0.00000000}.{guid}#{e6327cad-dcec-4949-ae8a-991e976a79d2}` | ✅ |

Build the full path:
```cpp
std::wstring WrapDeviceIdForAPC(const std::wstring& mmId) {
    return L"\\\\?\\SWD#MMDEVAPI#" + mmId
         + L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
}
```

The suffix GUID `{e6327cad-dcec-4949-ae8a-991e976a79d2}` is the render device interface class.

### HSTRING requirement

The function takes `HSTRING`, not `LPCWSTR`. Passing a raw string pointer crashes or returns garbage results. Use `combase.dll` exports directly (no `#include` needed):

```cpp
// Load from combase.dll at runtime
PFN_WindowsCreateString       WindowsCreateString;
PFN_WindowsDeleteString       WindowsDeleteString;
PFN_WindowsGetStringRawBuffer  WindowsGetStringRawBuffer;

// Create
HSTRING hDev = nullptr;
WindowsCreateString(wrapped.c_str(), (UINT32)wrapped.size(), &hDev);

// Use
CallSetImmersive(statics, pid, 0, role, hDev);

// Free
WindowsDeleteString(hDev);
```

### SEH wrapper requirement

The vtable call must be wrapped in `__try` / `__except` to survive bad slots or future ABI changes. However, `__try` cannot appear in a function that has C++ objects with destructors (compiler error C2712). Extract the raw call into a plain C function:

```cpp
// Plain C function — no C++ objects, __try is allowed
static HRESULT CallSetImmersive(void* obj, UINT32 pid, INT flow, INT role, HSTRING hDev) {
    void** vtbl = *(void***)obj;
    auto fn = (PFN_SetImmersiveAudioEndpoint)vtbl[g_slotSetPersisted];
    __try { return fn(obj, pid, flow, role, hDev); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
}
```

### Limitations

- **PID 0 (System Sounds)** — `SetPersistedDefaultAudioEndpoint` always returns `E_INVALIDARG` for PID 0. System Sounds cannot be routed per-process via this API.
- **Live session movement** — The API sets a policy that takes effect immediately for most apps, but some apps (especially those with custom audio engines) only pick up the change on restart. Audio does move live for standard WASAPI clients.
- **No capture support tested** — Only `eRender` (flow=0) was tested.

---

## 2. `IPolicyConfig` / `CPolicyConfigClient`

Undocumented COM interface for changing the **system-wide default audio endpoint** — the equivalent of going to Sound Settings and selecting a new default device.

### CLSID and IID

From `PolicyConfig.h` (community-sourced header):

```cpp
// {870AF99C-171D-4F9E-AF0D-E63DF40C2BC9}
DEFINE_GUID(CLSID_CPolicyConfigClient, ...);

interface IPolicyConfig : IUnknown {
    HRESULT SetDefaultEndpoint(LPCWSTR deviceId, ERole role);
    // ... other methods
};
```

### Usage

```cpp
IPolicyConfig* pPC = nullptr;
CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr, CLSCTX_ALL,
                 __uuidof(IPolicyConfig), (void**)&pPC);

for (ERole r : {eConsole, eMultimedia, eCommunications})
    pPC->SetDefaultEndpoint(deviceId, r);

pPC->Release();
```

Takes the plain MMDevice ID string (NOT the wrapped interface path). This is in contrast to `AudioPolicyConfig` which requires the full path.

---

## 3. Key differences between the two APIs

| | `AudioPolicyConfig` | `IPolicyConfig` |
|---|---|---|
| Scope | Per-process override | System-wide default |
| Device ID format | Full interface path `\\?\SWD#MMDEVAPI#...` | Plain MMDevice ID |
| Persistence | Survives app restart | Survives reboot |
| SDK header | None — vtable hack | Community `PolicyConfig.h` |
| Works on PID 0 | No | N/A |

---

## 4. How EarTrumpet uses these APIs

EarTrumpet (open source) was the primary reference. Key observations:

- Uses `SetPersistedDefaultAudioEndpoint` exclusively for per-app routing — no registry writes, no session hijacking
- Calls `GenerateDeviceId()` to build the wrapped interface path from a plain MMDevice ID
- Calls all three roles on every route change
- Does NOT call `GetPersistedDefaultAudioEndpoint` to verify — just fires and forgets
- Uses `RoGetActivationFactory` + QI pattern identical to what's documented here

---

## 5. References

- EarTrumpet source: `AudioPolicyConfigService.cs` — C# P/Invoke wrapper for the same vtable slots
- Windows build `{e6327cad-dcec-4949-ae8a-991e976a79d2}` GUID: render device interface class, found in Windows registry under `HKLM\SYSTEM\CurrentControlSet\Control\DeviceClasses`
- `PolicyConfig.h`: widely circulated community header, originally reverse engineered around Windows Vista
