// latencytest.cpp — end-to-end bridge latency measurement.
//
// Opens an internal mini-bridge (virtual loopback → physical render) plus a
// test render on the virtual channel and a measurement loopback on the physical
// output.  Writes a click at T0, detects it in the physical loopback at T1.
//
// Usage:  latencytest.exe                                   (auto-pick)
//         latencytest.exe "TheAudio.app Games"              (prefix match on virtual)
//         latencytest.exe "TheAudio.app Games" "VG259QM"    (prefix match on both)
//
// For accurate readings: stop all other apps routing through the bridge so the
// physical loopback is quiet before the test click arrives.

#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "winmm.lib")
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cmath>

static const LONGLONG CAP_BUF_HNS = 200000;  // 20 ms for capture (virtual loopback + sil render)
static const LONGLONG REN_BUF_HNS = 1000000; // 100 ms for physical render — must fit >=2 capture packets

static IMMDeviceEnumerator* g_de = nullptr;

struct EP { std::wstring id, name; bool virt; };

static std::vector<EP> EnumRender() {
    std::vector<EP> v;
    IMMDeviceCollection* col = nullptr;
    if (FAILED(g_de->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return v;
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        IMMDevice* d = nullptr; if (FAILED(col->Item(i, &d))) continue;
        EP ep{}; ep.virt = false;
        LPWSTR id = nullptr; d->GetId(&id);
        if (id) { ep.id = id; CoTaskMemFree(id); }
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT fn, di;
            PropVariantInit(&fn); PropVariantInit(&di);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &fn)) && fn.vt == VT_LPWSTR)
                ep.name = fn.pwszVal;
            if (SUCCEEDED(ps->GetValue(PKEY_DeviceInterface_FriendlyName, &di)) && di.vt == VT_LPWSTR)
                ep.virt = (wcsstr(di.pwszVal, L"TheAudio.app") != nullptr);
            PropVariantClear(&fn); PropVariantClear(&di); ps->Release();
        }
        v.push_back(ep); d->Release();
    }
    col->Release();
    return v;
}

// Prefix or exact match (case-sensitive). "VG259QM" matches "VG259QM (NVIDIA ...)".
static bool NameMatch(const std::wstring& epName, const std::wstring& arg) {
    if (epName == arg) return true;
    if (epName.size() >= arg.size() && epName.substr(0, arg.size()) == arg) return true;
    return false;
}

static bool OpenRenderCl(IMMDevice* dev, LONGLONG bufHns,
                         IAudioClient** pCl, IAudioRenderClient** pRc,
                         WAVEFORMATEX** pFmt, UINT32* pBuf)
{
    IAudioClient* cl = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cl)))
        return false;
    WAVEFORMATEX* fmt = nullptr; cl->GetMixFormat(&fmt);
    if (FAILED(cl->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufHns, 0, fmt, nullptr)))
        { CoTaskMemFree(fmt); cl->Release(); return false; }
    IAudioRenderClient* rc = nullptr;
    if (FAILED(cl->GetService(__uuidof(IAudioRenderClient), (void**)&rc)))
        { CoTaskMemFree(fmt); cl->Release(); return false; }
    UINT32 buf = 0; cl->GetBufferSize(&buf);
    *pCl = cl; *pRc = rc; *pFmt = fmt; *pBuf = buf;
    return true;
}

static bool OpenLoopbackCl(IMMDevice* dev, LONGLONG bufHns,
                           IAudioClient** pCl, IAudioCaptureClient** pCc,
                           WAVEFORMATEX** pFmt)
{
    IAudioClient* cl = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cl)))
        return false;
    WAVEFORMATEX* fmt = nullptr; cl->GetMixFormat(&fmt);
    if (FAILED(cl->Initialize(AUDCLNT_SHAREMODE_SHARED,
                              AUDCLNT_STREAMFLAGS_LOOPBACK, bufHns, 0, fmt, nullptr)))
        { CoTaskMemFree(fmt); cl->Release(); return false; }
    IAudioCaptureClient* cc = nullptr;
    if (FAILED(cl->GetService(__uuidof(IAudioCaptureClient), (void**)&cc)))
        { CoTaskMemFree(fmt); cl->Release(); return false; }
    *pCl = cl; *pCc = cc; *pFmt = fmt;
    return true;
}

// Max absolute value treating buffer as float32. Works for any 32-bit format:
// for float32 silent audio returns ~0.0; for any real audio (float or PCM int32)
// returns a clearly non-zero value we can threshold against.
static float MaxAmp(const BYTE* data, UINT32 frames, UINT32 channels) {
    float m = 0.f;
    const float* f = reinterpret_cast<const float*>(data);
    for (UINT32 i = 0; i < frames * channels; i++) {
        float a = fabsf(f[i]);
        if (a > m) m = a;
    }
    return m;
}

static void DrainCapture(IAudioCaptureClient* cc) {
    UINT32 pkt = 0;
    while (SUCCEEDED(cc->GetNextPacketSize(&pkt)) && pkt > 0) {
        BYTE* p = nullptr; UINT32 n = 0; DWORD fl = 0;
        if (FAILED(cc->GetBuffer(&p, &n, &fl, nullptr, nullptr))) break;
        cc->ReleaseBuffer(n);
    }
}

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator), (void**)&g_de);

    auto eps = EnumRender();
    printf("=== Endpoints ===\n");
    for (auto& e : eps)
        printf("  [%s] %S\n", e.virt ? "virtual " : "physical", e.name.c_str());
    printf("\n");

    EP* vEP = nullptr; EP* pEP = nullptr;
    if (argc >= 2) {
        std::wstring w(argv[1], argv[1] + strlen(argv[1]));
        for (auto& e : eps) if (e.virt  && NameMatch(e.name, w)) { vEP = &e; break; }
    }
    if (argc >= 3) {
        std::wstring w(argv[2], argv[2] + strlen(argv[2]));
        for (auto& e : eps) if (!e.virt && NameMatch(e.name, w)) { pEP = &e; break; }
    }
    if (!vEP) for (auto& e : eps) if (e.virt)  { vEP = &e; break; }
    if (!pEP) for (auto& e : eps) if (!e.virt) { pEP = &e; break; }

    if (!vEP || !pEP) { printf("ERROR: no endpoint pair found.\n"); return 1; }
    printf("Virtual  : %S\n", vEP->name.c_str());
    printf("Physical : %S\n\n", pEP->name.c_str());

    IMMDevice* vDev = nullptr; g_de->GetDevice(vEP->id.c_str(), &vDev);
    IMMDevice* pDev = nullptr; g_de->GetDevice(pEP->id.c_str(), &pDev);

    timeBeginPeriod(1);
    DWORD mmTaskIdx = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmTaskIdx);

    // Clients:
    // silCl  — keep-alive render on virtual with EVENTCALLBACK for precise timing
    // tstCl  — test signal render on virtual (we write clicks here)
    // capCl  — loopback capture from virtual  (bridge source)
    // renCl  — render to physical             (bridge destination)
    // meaCl  — loopback capture from physical (measurement point)
    IAudioClient* silCl = nullptr; IAudioRenderClient* silRc = nullptr;
    WAVEFORMATEX* silFmt = nullptr; UINT32 silBuf = 0;
    HANDLE silEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Open silCl with EVENTCALLBACK — same pattern as bridge.cpp
    if (!silEvent ||
        FAILED(vDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&silCl)))
        { printf("ERROR: sil activate\n"); return 1; }
    silCl->GetMixFormat(&silFmt);
    if (FAILED(silCl->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 CAP_BUF_HNS, 0, silFmt, nullptr)) ||
        FAILED(silCl->SetEventHandle(silEvent)) ||
        FAILED(silCl->GetService(__uuidof(IAudioRenderClient), (void**)&silRc)))
        { printf("ERROR: sil init\n"); return 1; }
    silCl->GetBufferSize(&silBuf);

    IAudioClient* tstCl = nullptr; IAudioRenderClient* tstRc = nullptr;
    WAVEFORMATEX* tstFmt = nullptr; UINT32 tstBuf = 0;
    IAudioClient* capCl = nullptr; IAudioCaptureClient* capCc = nullptr;
    WAVEFORMATEX* capFmt = nullptr;
    IAudioClient* renCl = nullptr; IAudioRenderClient* renRc = nullptr;
    WAVEFORMATEX* renFmt = nullptr; UINT32 renBuf = 0;
    IAudioClient* meaCl = nullptr; IAudioCaptureClient* meaCc = nullptr;
    WAVEFORMATEX* meaFmt = nullptr;

    if (!OpenRenderCl  (vDev, CAP_BUF_HNS, &tstCl, &tstRc, &tstFmt, &tstBuf)) { printf("ERROR: tst\n");  return 1; }
    if (!OpenLoopbackCl(vDev, CAP_BUF_HNS, &capCl, &capCc, &capFmt))           { printf("ERROR: cap\n");  return 1; }
    if (!OpenRenderCl  (pDev, REN_BUF_HNS, &renCl, &renRc, &renFmt, &renBuf)) { printf("ERROR: ren\n");  return 1; }
    if (!OpenLoopbackCl(pDev, CAP_BUF_HNS, &meaCl, &meaCc, &meaFmt))           { printf("ERROR: mea\n");  return 1; }

    UINT32 blockAlign = capFmt->nBlockAlign;
    UINT32 tCh        = tstFmt->nChannels;
    UINT32 tRate      = tstFmt->nSamplesPerSec;
    UINT32 mCh        = meaFmt->nChannels;

    printf("Virtual fmt  : %u-bit, %uch, %uHz  blockAlign=%u\n",
           tstFmt->wBitsPerSample, tCh, tRate, tstFmt->nBlockAlign);
    printf("Physical fmt : %u-bit, %uch, %uHz  blockAlign=%u\n\n",
           renFmt->wBitsPerSample, renFmt->nChannels, renFmt->nSamplesPerSec, renFmt->nBlockAlign);

    silCl->Start(); tstCl->Start(); capCl->Start(); renCl->Start(); meaCl->Start();

    const int TRIALS    = 5;
    const int PRIME_MS  = 400;
    const int CLICK_MS  = 20;    // length of the test click
    const int TIMEOUT   = 3000;

    // Detection threshold: 0.05f works for float32 audio (click is 0.9f).
    // For PCM int32 loopback, any real audio will reinterpret to huge values >> 0.05f.
    const float THRESHOLD = 0.05f;

    LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER t0 = {}, t1 = {};

    enum { PRIMING, DETECTING } state = PRIMING;
    DWORD stateStart = GetTickCount();

    double results[TRIALS] = {};
    int trial = 0;

    // Per-trial diagnostics
    UINT64 diagBridgePkts = 0, diagBridgeFrames = 0;
    UINT64 diagBridgeNonSilent = 0, diagBridgeSkipped = 0;
    UINT64 diagMeaPkts = 0, diagMeaSilent = 0;
    float  diagMaxAmp = 0.f;

    printf("Buffer sizes: silBuf=%u tstBuf=%u renBuf=%u\n\n", silBuf, tstBuf, renBuf);

    printf("--- Latency measurements (cap=%lld ms  ren=%lld ms) ---\n",
           CAP_BUF_HNS / 10000, REN_BUF_HNS / 10000);
    printf("    Close any app routing through the bridge for clean readings.\n\n");

    while (trial < TRIALS) {
        DWORD now = GetTickCount();

        // ── Silent keep-alive on virtual ──────────────────────────────────────
        {
            UINT32 pad = 0; silCl->GetCurrentPadding(&pad);
            UINT32 avail = silBuf - pad;
            if (avail > 0) {
                BYTE* d = nullptr;
                if (SUCCEEDED(silRc->GetBuffer(avail, &d)))
                    silRc->ReleaseBuffer(avail, AUDCLNT_BUFFERFLAGS_SILENT);
            }
        }

        // ── Internal bridge: virtual loopback → physical render ───────────────
        {
            UINT32 pkt = 0;
            while (SUCCEEDED(capCc->GetNextPacketSize(&pkt)) && pkt > 0) {
                BYTE* src = nullptr; UINT32 frames = 0; DWORD flags = 0;
                if (FAILED(capCc->GetBuffer(&src, &frames, &flags, nullptr, nullptr))) break;
                diagBridgePkts++; diagBridgeFrames += frames;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) diagBridgeNonSilent++;
                UINT32 pad = 0; renCl->GetCurrentPadding(&pad);
                if (renBuf - pad >= frames) {
                    BYTE* dst = nullptr;
                    if (SUCCEEDED(renRc->GetBuffer(frames, &dst))) {
                        if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                            memset(dst, 0, frames * blockAlign);
                        else
                            memcpy(dst, src, frames * blockAlign);
                        renRc->ReleaseBuffer(frames, 0);
                    }
                } else {
                    diagBridgeSkipped++;
                }
                capCc->ReleaseBuffer(frames);
            }
        }

        // ── State machine ─────────────────────────────────────────────────────
        if (state == PRIMING) {
            // Do NOT fill tstRc during priming — let it underflow (engine inserts silence).
            // silCl keeps the virtual device alive. tstRc stays empty so the click
            // we write at transition time lands at position 0 with no queued delay.

            if ((int)(now - stateStart) >= PRIME_MS) {
                // Drain accumulated capCc packets (priming silence) so the click is
                // the first real data the bridge sees after transition.
                DrainCapture(capCc);
                // Flush stale physical loopback data.
                DrainCapture(meaCc);

                // Reset diagnostics for this detection window
                diagBridgePkts = diagBridgeFrames = 0;
                diagBridgeNonSilent = diagBridgeSkipped = 0;
                diagMeaPkts = diagMeaSilent = 0;
                diagMaxAmp = 0.f;

                // Write the click to the empty tstRc buffer.
                UINT32 clickFrames = (tRate * CLICK_MS) / 1000;
                UINT32 pad = 0; tstCl->GetCurrentPadding(&pad);
                UINT32 avail = tstBuf - pad;
                UINT32 write = (avail < clickFrames) ? avail : clickFrames;
                if (write > 0) {
                    BYTE* d = nullptr;
                    if (SUCCEEDED(tstRc->GetBuffer(write, &d))) {
                        float* f = reinterpret_cast<float*>(d);
                        for (UINT32 i = 0; i < write * tCh; i++) f[i] = 0.9f;
                        QueryPerformanceCounter(&t0);
                        tstRc->ReleaseBuffer(write, 0);
                    }
                }
                state = DETECTING;
                stateStart = now;
            }

        } else { // DETECTING
            UINT32 pkt = 0;
            while (SUCCEEDED(meaCc->GetNextPacketSize(&pkt)) && pkt > 0) {
                BYTE* src = nullptr; UINT32 frames = 0; DWORD flags = 0;
                if (FAILED(meaCc->GetBuffer(&src, &frames, &flags, nullptr, nullptr))) break;
                diagMeaPkts++;
                bool hit = false;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    diagMeaSilent++;
                } else {
                    float amp = MaxAmp(src, frames, mCh);
                    if (amp > diagMaxAmp) diagMaxAmp = amp;
                    if (amp > THRESHOLD) {
                        QueryPerformanceCounter(&t1);
                        hit = true;
                    }
                }
                meaCc->ReleaseBuffer(frames);
                if (hit) {
                    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / qpf.QuadPart;
                    printf("Trial %d/%d: %5.1f ms\n", trial + 1, TRIALS, ms);
                    results[trial++] = ms;
                    stateStart = GetTickCount();
                    state = PRIMING;
                    break;
                }
            }

            if (state == DETECTING && (int)(now - stateStart) > TIMEOUT) {
                printf("Trial %d/%d: TIMEOUT — click not detected in %d ms\n",
                       trial + 1, TRIALS, TIMEOUT);
                printf("  diag: bridge=%llu pkts / %llu nonSilent / %llu skipped  mea=%llu pkts (%llu silent) maxAmp=%.5f\n",
                       diagBridgePkts, diagBridgeNonSilent, diagBridgeSkipped,
                       diagMeaPkts, diagMeaSilent, diagMaxAmp);
                results[trial++] = -1.0;
                stateStart = GetTickCount();
                state = PRIMING;
            }
        }

        WaitForSingleObject(silEvent, 50);
    }

    // ── Summary ───────────────────────────────────────────────────────────────
    printf("\n--- Summary ---\n");
    double mn = 1e9, mx = 0, sum = 0; int valid = 0;
    for (int i = 0; i < TRIALS; i++) {
        if (results[i] > 0) {
            if (results[i] < mn) mn = results[i];
            if (results[i] > mx) mx = results[i];
            sum += results[i]; valid++;
        }
    }
    if (valid > 0)
        printf("  Min %.1f ms   Max %.1f ms   Avg %.1f ms\n", mn, mx, sum / valid);
    else
        printf("  No valid trials — check bridge is running and devices are correct.\n");

    silCl->Stop(); tstCl->Stop(); capCl->Stop(); renCl->Stop(); meaCl->Stop();
    silRc->Release(); silCl->Release(); CoTaskMemFree(silFmt); CloseHandle(silEvent);
    tstRc->Release(); tstCl->Release(); CoTaskMemFree(tstFmt);
    capCc->Release(); capCl->Release(); CoTaskMemFree(capFmt);
    renRc->Release(); renCl->Release(); CoTaskMemFree(renFmt);
    meaCc->Release(); meaCl->Release(); CoTaskMemFree(meaFmt);
    vDev->Release(); pDev->Release(); g_de->Release();
    if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
    timeEndPeriod(1);
    CoUninitialize();
    return 0;
}
