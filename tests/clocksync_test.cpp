#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <vector>
#include <string>
#include <chrono>

#pragma comment(lib, "ole32.lib")

static void hr(const char* label, HRESULT r) {
    if (FAILED(r)) printf("  [FAIL] %s (0x%08X)\n", label, (unsigned)r);
}

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* de = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&de);

    IMMDevice* capDev = nullptr;
    IMMDevice* renDev = nullptr;

    // Pick "TheAudio.app Games" (render) and a physical default output
    IMMDeviceCollection* col = nullptr;
    de->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    UINT count = 0; col->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        IMMDevice* dev = nullptr;
        col->Item(i, &dev);
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
                std::wstring name = pv.pwszVal;
                if (!capDev && name.find(L"TheAudio.app Games") != std::wstring::npos) {
                    capDev = dev;
                    printf("Found Capture Device: %S\n", name.c_str());
                } else if (!renDev && name.find(L"TheAudio.app") == std::wstring::npos) {
                    renDev = dev;
                    printf("Found Physical Render Device: %S\n", name.c_str());
                }
            }
            PropVariantClear(&pv); ps->Release();
        }
        if (capDev && renDev) break;
    }

    if (!capDev || !renDev) {
        printf("Missing devices. Cap: %p, Ren: %p\n", capDev, renDev);
        return 1;
    }

    IAudioClient *capCl = nullptr, *renCl = nullptr;
    capDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&capCl);
    renDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&renCl);

    WAVEFORMATEX *capFmt = nullptr, *renFmt = nullptr;
    capCl->GetMixFormat(&capFmt);
    renCl->GetMixFormat(&renFmt);

    printf("Capture Format: %u Hz, %u ch, %u bits, %u align\n", capFmt->nSamplesPerSec, capFmt->nChannels, capFmt->wBitsPerSample, capFmt->nBlockAlign);
    printf("Render  Format: %u Hz, %u ch, %u bits, %u align\n", renFmt->nSamplesPerSec, renFmt->nChannels, renFmt->wBitsPerSample, renFmt->nBlockAlign);

    HRESULT hr1 = capCl->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 100000, 0, capFmt, nullptr);
    HRESULT hr2 = renCl->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 100000, 0, renFmt, nullptr);
    hr("Init Cap", hr1);
    hr("Init Ren", hr2);

    IAudioCaptureClient* capCc = nullptr;
    IAudioRenderClient* renRc = nullptr;
    capCl->GetService(__uuidof(IAudioCaptureClient), (void**)&capCc);
    renCl->GetService(__uuidof(IAudioRenderClient), (void**)&renRc);

    // Keep virtual alive with a silent stream
    IAudioClient* silCl = nullptr;
    capDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&silCl);
    silCl->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 100000, 0, capFmt, nullptr);
    IAudioRenderClient* silRc = nullptr;
    silCl->GetService(__uuidof(IAudioRenderClient), (void**)&silRc);

    printf("\nStarting 10-second measurement...\n");
    silCl->Start(); capCl->Start(); renCl->Start();

    auto start = std::chrono::steady_clock::now();
    UINT64 capFrames = 0, renFrames = 0;
    
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
        // Keep alive
        UINT32 pad = 0, sz = 0;
        silCl->GetCurrentPadding(&pad); silCl->GetBufferSize(&sz);
        if (sz - pad > 0) {
            BYTE* d = nullptr;
            if (SUCCEEDED(silRc->GetBuffer(sz - pad, &d))) silRc->ReleaseBuffer(sz - pad, AUDCLNT_BUFFERFLAGS_SILENT);
        }

        // Capture
        UINT32 pkt = 0;
        while (SUCCEEDED(capCc->GetNextPacketSize(&pkt)) && pkt > 0) {
            BYTE* src = nullptr; UINT32 f = 0; DWORD fl = 0;
            if (SUCCEEDED(capCc->GetBuffer(&src, &f, &fl, nullptr, nullptr))) {
                capFrames += f;
                capCc->ReleaseBuffer(f);
            }
        }

        // Render (just to keep it paced)
        renCl->GetCurrentPadding(&pad); renCl->GetBufferSize(&sz);
        if (sz - pad > 0) {
            BYTE* d = nullptr;
            if (SUCCEEDED(renRc->GetBuffer(sz - pad, &d))) {
                renFrames += (sz - pad);
                renRc->ReleaseBuffer(sz - pad, AUDCLNT_BUFFERFLAGS_SILENT);
            }
        }
        Sleep(1);
    }

    auto end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end - start).count();

    printf("\nResults:\n");
    printf("Elapsed time: %.4f s\n", elapsed);
    printf("Captured: %llu frames (%.2f Hz)\n", capFrames, capFrames / elapsed);
    printf("Rendered: %llu frames (%.2f Hz)\n", renFrames, renFrames / elapsed);
    
    double capRatio = (capFrames / elapsed) / capFmt->nSamplesPerSec;
    double renRatio = (renFrames / elapsed) / renFmt->nSamplesPerSec;
    printf("Capture Clock Ratio: %.6f\n", capRatio);
    printf("Render  Clock Ratio: %.6f\n", renRatio);
    printf("Relative Drift: %.6f\n", capRatio / renRatio);

    silCl->Stop(); capCl->Stop(); renCl->Stop();
    CoUninitialize();
    return 0;
}
