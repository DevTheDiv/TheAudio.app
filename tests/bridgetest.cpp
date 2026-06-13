// bridgetest.cpp — standalone bridge diagnostic
//
// Runs the exact same WASAPI open/capture/render sequence as bridge.cpp and
// reports the HRESULT at every step so you can see exactly where it breaks.
//
// Usage (run as admin from core\x64\Release or any directory):
//   bridgetest.exe                        -- auto-pick first virtual + first physical
//   bridgetest.exe "TheAudio.app Games"   -- specific virtual channel
//   bridgetest.exe "TheAudio.app Games" "Speakers (VG259QM)"  -- specific pair
//
// Output goes to stdout; run in a terminal to read it.

#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ole32.lib")

// ── helpers ───────────────────────────────────────────────────────────────────

static void hr(const char* label, HRESULT r) {
    if (SUCCEEDED(r))
        printf("  [OK]   %s\n", label);
    else
        printf("  [FAIL] %s  HRESULT=0x%08X\n", label, (unsigned)r);
}

struct EP {
    std::wstring id;
    std::wstring name;
    bool isVirtual;
};

static std::vector<EP> EnumEndpoints(IMMDeviceEnumerator* de) {
    std::vector<EP> out;
    IMMDeviceCollection* col = nullptr;
    if (FAILED(de->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(col->Item(i, &dev))) continue;
        LPWSTR idStr = nullptr; dev->GetId(&idStr);
        std::wstring id = idStr ? idStr : L""; if (idStr) CoTaskMemFree(idStr);
        std::wstring name;
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                name = pv.pwszVal;
            PropVariantClear(&pv); ps->Release();
        }
        bool virt = name.rfind(L"TheAudio.app", 0) == 0;
        out.push_back({ id, name, virt });
        dev->Release();
    }
    col->Release();
    return out;
}

static IMMDevice* GetByName(IMMDeviceEnumerator* de, const std::wstring& name) {
    IMMDeviceCollection* col = nullptr;
    if (FAILED(de->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return nullptr;
    UINT n = 0; col->GetCount(&n);
    IMMDevice* found = nullptr;
    for (UINT i = 0; i < n && !found; i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(col->Item(i, &dev))) continue;
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
                std::wstring fn = pv.pwszVal;
                // exact match or the friendly name starts with the search string
                if (fn == name || fn.rfind(name, 0) == 0 || fn.find(name) != std::wstring::npos)
                    found = dev;
            }
            PropVariantClear(&pv); ps->Release();
        }
        if (!found) dev->Release();
    }
    col->Release();
    return found;
}

static std::wstring WFmtStr(WAVEFORMATEX* f) {
    if (!f) return L"null";
    wchar_t buf[128];
    bool isFloat = false;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE* x = (WAVEFORMATEXTENSIBLE*)f;
        GUID ieee = { 0x00000003, 0x0000, 0x0010,
            {0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71} };
        isFloat = memcmp(&x->SubFormat, &ieee, sizeof(GUID)) == 0;
    } else {
        isFloat = f->wFormatTag == 3;
    }
    swprintf_s(buf, L"%u-bit %s, %uch, %uHz, blockAlign=%u",
        f->wBitsPerSample, isFloat ? L"float" : L"PCM",
        f->nChannels, f->nSamplesPerSec, f->nBlockAlign);
    return buf;
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Parse args
    std::wstring wantCap, wantRen;
    if (argc >= 2) {
        int n = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
        wantCap.resize(n); MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, &wantCap[0], n);
        wantCap.pop_back();
    }
    if (argc >= 3) {
        int n = MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, nullptr, 0);
        wantRen.resize(n); MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, &wantRen[0], n);
        wantRen.pop_back();
    }

    printf("=== Stage 1: Enumerate endpoints ===\n");
    IMMDeviceEnumerator* de = nullptr;
    HRESULT hCO = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void**)&de);
    hr("CoCreateInstance(MMDeviceEnumerator)", hCO);
    if (!de) { CoUninitialize(); return 1; }

    auto eps = EnumEndpoints(de);
    printf("  Found %u active render endpoints:\n", (unsigned)eps.size());
    for (auto& e : eps)
        printf("    [%s] %S\n", e.isVirtual ? "virtual " : "physical", e.name.c_str());

    // Also enumerate capture endpoints — virtual drivers often expose a paired capture side
    printf("\n  Capture endpoints (eCapture):\n");
    {
        IMMDeviceCollection* cap = nullptr;
        if (SUCCEEDED(de->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &cap))) {
            UINT cn = 0; cap->GetCount(&cn);
            if (cn == 0) printf("    (none)\n");
            for (UINT i = 0; i < cn; i++) {
                IMMDevice* d = nullptr; if (FAILED(cap->Item(i, &d))) continue;
                IPropertyStore* ps = nullptr; std::wstring nm;
                if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                    PROPVARIANT pv; PropVariantInit(&pv);
                    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) nm = pv.pwszVal;
                    PropVariantClear(&pv); ps->Release();
                }
                bool virt = nm.rfind(L"TheAudio.app", 0) == 0;
                printf("    [%s] %S\n", virt ? "virtual " : "physical", nm.c_str());
                d->Release();
            }
            cap->Release();
        }
    }
    printf("\n");

    // Pick capture device (virtual)
    IMMDevice* capDev = nullptr;
    std::wstring capName;
    if (!wantCap.empty()) {
        capDev = GetByName(de, wantCap);
        capName = wantCap;
    } else {
        for (auto& e : eps) {
            if (e.isVirtual) {
                de->GetDevice(e.id.c_str(), &capDev);
                capName = e.name; break;
            }
        }
    }
    if (!capDev) {
        printf("  [FAIL] No virtual channel found. Is the driver installed?\n");
        CoUninitialize(); return 1;
    }
    printf("  Capture target : %S\n", capName.c_str());

    // Pick render device (physical)
    IMMDevice* renDev = nullptr;
    std::wstring renName;
    if (!wantRen.empty()) {
        // Try as device ID first, then friendly name
        de->GetDevice(wantRen.c_str(), &renDev);
        if (!renDev) renDev = GetByName(de, wantRen);
        renName = wantRen;
    } else {
        for (auto& e : eps) {
            if (!e.isVirtual) {
                de->GetDevice(e.id.c_str(), &renDev);
                renName = e.name; break;
            }
        }
    }
    if (!renDev) {
        printf("  [FAIL] No physical output found.\n");
        capDev->Release(); CoUninitialize(); return 1;
    }
    printf("  Render  target : %S\n\n", renName.c_str());

    // Driver-native format: 16-bit PCM stereo 48kHz (from miniport.cpp data ranges)
    WAVEFORMATEX pcm16 = {};
    pcm16.wFormatTag      = WAVE_FORMAT_PCM;
    pcm16.nChannels       = 2;
    pcm16.nSamplesPerSec  = 48000;
    pcm16.wBitsPerSample  = 16;
    pcm16.nBlockAlign     = 4;
    pcm16.nAvgBytesPerSec = 48000 * 4;

    // ── Stage 2: Silent render on virtual (keep-alive) ────────────────────────
    printf("=== Stage 2: Silent render on virtual channel (keep-alive) ===\n");
    IAudioClient* silCl = nullptr; IAudioRenderClient* silRc = nullptr;
    WAVEFORMATEX* silFmt = nullptr;
    hr("Activate IAudioClient (silent)", capDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&silCl));
    if (silCl) {
        silCl->GetMixFormat(&silFmt);
        printf("  GetMixFormat: %S\n", WFmtStr(silFmt).c_str());
        HRESULT h = silCl->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, silFmt, nullptr);
        hr("Initialize SHARED (mixFmt)", h);
        if (FAILED(h)) {
            printf("  Retrying with 16-bit PCM 48kHz...\n");
            silCl->Release(); silCl = nullptr;
            capDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&silCl);
            if (silCl) hr("Initialize SHARED (pcm16)", silCl->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, &pcm16, nullptr));
        }
        if (silCl) hr("GetService IAudioRenderClient", silCl->GetService(__uuidof(IAudioRenderClient), (void**)&silRc));
    }
    printf("\n");

    // ── Stage 3: Loopback capture from virtual channel ────────────────────────
    printf("=== Stage 3: Loopback capture from virtual channel ===\n");
    IAudioClient* capCl = nullptr; IAudioCaptureClient* capCc = nullptr;
    WAVEFORMATEX* capFmt = nullptr;
    hr("Activate IAudioClient (capture)", capDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&capCl));
    if (capCl) {
        capCl->GetMixFormat(&capFmt);
        printf("  GetMixFormat: %S\n", WFmtStr(capFmt).c_str());
        HRESULT h = capCl->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, capFmt, nullptr);
        hr("Initialize LOOPBACK (mixFmt)", h);
        if (FAILED(h)) {
            printf("  Retrying with 16-bit PCM 48kHz...\n");
            capCl->Release(); capCl = nullptr; CoTaskMemFree(capFmt); capFmt = nullptr;
            capDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&capCl);
            if (capCl) {
                capFmt = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
                *capFmt = pcm16;
                hr("Initialize LOOPBACK (pcm16)", capCl->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, capFmt, nullptr));
            }
        }
        if (capCl) hr("GetService IAudioCaptureClient", capCl->GetService(__uuidof(IAudioCaptureClient), (void**)&capCc));
    }
    printf("\n");

    // ── Stage 3b: Try direct capture (not loopback) on paired capture endpoint ─
    printf("=== Stage 3b: Direct capture on paired virtual capture endpoint ===\n");
    {
        // Find capture endpoint with matching name
        IMMDeviceCollection* capCol = nullptr;
        IMMDevice* pairedCapDev = nullptr;
        if (SUCCEEDED(de->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &capCol))) {
            UINT cn = 0; capCol->GetCount(&cn);
            for (UINT i = 0; i < cn; i++) {
                IMMDevice* d = nullptr; if (FAILED(capCol->Item(i, &d))) continue;
                IPropertyStore* ps = nullptr; std::wstring nm;
                if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                    PROPVARIANT pv; PropVariantInit(&pv);
                    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) nm = pv.pwszVal;
                    PropVariantClear(&pv); ps->Release();
                }
                // Match on the channel name prefix
                if (nm.rfind(capName.substr(0, capName.find(L' ', 13)), 0) != std::wstring::npos) {
                    pairedCapDev = d; printf("  Paired capture endpoint: %S\n", nm.c_str());
                } else { d->Release(); }
            }
            capCol->Release();
        }

        if (pairedCapDev) {
            IAudioClient* cl2 = nullptr; IAudioCaptureClient* cc2 = nullptr;
            WAVEFORMATEX* fmt2 = nullptr;
            hr("Activate IAudioClient (direct capture)", pairedCapDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cl2));
            if (cl2) {
                cl2->GetMixFormat(&fmt2);
                printf("  Mix format: %S\n", WFmtStr(fmt2).c_str());
                hr("Initialize SHARED (direct capture)", cl2->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, fmt2, nullptr));
                hr("GetService IAudioCaptureClient", cl2->GetService(__uuidof(IAudioCaptureClient), (void**)&cc2));
                if (cc2) { printf("  [OK] Direct capture opened successfully — bridge should use this path\n"); cc2->Release(); }
                if (cl2)  cl2->Release();
                if (fmt2) CoTaskMemFree(fmt2);
            }
            pairedCapDev->Release();
        } else {
            printf("  No paired capture endpoint found for %S\n", capName.c_str());
            printf("  (driver may only expose render endpoints — loopback is the only option)\n");
        }
    }
    printf("\n");

    // ── Stage 4: Shared render on physical output ─────────────────────────────
    printf("=== Stage 4: Shared render on physical output ===\n");
    IAudioClient* renCl = nullptr; IAudioRenderClient* renRc = nullptr;
    WAVEFORMATEX* renFmt = nullptr;
    hr("Activate IAudioClient (render)", renDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&renCl));
    if (renCl && capFmt) {
        printf("  Trying Initialize with capture format (%S)...\n", WFmtStr(capFmt).c_str());
        HRESULT hInit = renCl->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, capFmt, nullptr);
        hr("Initialize SHARED (capFmt)", hInit);
        if (SUCCEEDED(hInit)) {
            hr("GetService IAudioRenderClient", renCl->GetService(__uuidof(IAudioRenderClient), (void**)&renRc));
        } else {
            printf("  Falling back to device mix format...\n");
            renCl->Release(); renCl = nullptr;
            renDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&renCl);
            if (renCl) {
                renCl->GetMixFormat(&renFmt);
                printf("  Device mix format: %S\n", WFmtStr(renFmt).c_str());
                hr("Initialize SHARED (mixFmt)", renCl->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, renFmt, nullptr));
                hr("GetService IAudioRenderClient", renCl->GetService(__uuidof(IAudioRenderClient), (void**)&renRc));
            }
        }
    }
    printf("\n");

    bool ok = silRc && capCc && renRc;
    if (!ok) {
        printf("=== ABORTED: one or more stages failed — cannot run capture test ===\n");
        goto cleanup;
    }

    // ── Stage 5: 5-second capture/render loop ────────────────────────────────
    {
        printf("=== Stage 5: 5-second capture/render test ===\n");
        printf("  Play audio through %S now...\n\n", capName.c_str());

        UINT32 silBuf = 0; silCl->GetBufferSize(&silBuf);
        UINT32 renBuf = 0; renCl->GetBufferSize(&renBuf);
        UINT32 blockAlign = capFmt->nBlockAlign;

        silCl->Start(); capCl->Start(); renCl->Start();

        UINT64 totalCap = 0, totalRen = 0, silentPkts = 0, audioPkts = 0;

        for (int tick = 0; tick < 500; tick++) {    // 500 * 10ms = 5s
            // Feed silence to keep virtual alive
            UINT32 pad = 0; silCl->GetCurrentPadding(&pad);
            UINT32 avail = silBuf - pad;
            if (avail > 0) {
                BYTE* d = nullptr;
                if (SUCCEEDED(silRc->GetBuffer(avail, &d)))
                    silRc->ReleaseBuffer(avail, AUDCLNT_BUFFERFLAGS_SILENT);
            }

            // Capture
            UINT32 pkt = 0;
            while (SUCCEEDED(capCc->GetNextPacketSize(&pkt)) && pkt > 0) {
                BYTE* src = nullptr; UINT32 frames = 0; DWORD flags = 0;
                if (FAILED(capCc->GetBuffer(&src, &frames, &flags, nullptr, nullptr))) break;
                totalCap += frames;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) silentPkts++;
                else                                    audioPkts++;

                // Forward to render
                UINT32 rpad = 0; renCl->GetCurrentPadding(&rpad);
                if (renBuf - rpad >= frames) {
                    BYTE* dst = nullptr;
                    if (SUCCEEDED(renRc->GetBuffer(frames, &dst))) {
                        if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                            memset(dst, 0, frames * blockAlign);
                        else
                            memcpy(dst, src, frames * blockAlign);
                        renRc->ReleaseBuffer(frames, 0);
                        totalRen += frames;
                    }
                }
                capCc->ReleaseBuffer(frames);
            }

            if ((tick + 1) % 100 == 0) {
                int sec = (tick + 1) / 100;
                printf("  t=%ds: captured=%llu frames  rendered=%llu frames"
                       "  audio_pkts=%llu  silent_pkts=%llu\n",
                       sec, totalCap, totalRen, audioPkts, silentPkts);
            }
            Sleep(10);
        }

        silCl->Stop(); capCl->Stop(); renCl->Stop();

        printf("\n");
        if (totalCap == 0)
            printf("  [WARN] Zero frames captured — virtual channel may not be receiving audio.\n");
        else if (audioPkts == 0)
            printf("  [WARN] Captured %llu frames but all were SILENT — audio not reaching virtual channel.\n", totalCap);
        else
            printf("  [OK]   Captured %llu non-silent frames and forwarded %llu to render client.\n", totalCap, totalRen);

        printf("\n=== Done ===\n");
    }

cleanup:
    if (silRc) silRc->Release(); if (silCl) silCl->Release(); if (silFmt) CoTaskMemFree(silFmt);
    if (capCc) capCc->Release(); if (capCl) capCl->Release(); if (capFmt) CoTaskMemFree(capFmt);
    if (renRc) renRc->Release(); if (renCl) renCl->Release(); if (renFmt) CoTaskMemFree(renFmt);
    capDev->Release();
    renDev->Release();
    de->Release();
    CoUninitialize();
    return 0;
}
