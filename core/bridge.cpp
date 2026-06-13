// bridge.cpp — per-channel audio forwarding with decoupled capture / render threads.
//
// Architecture:
//   CapThread  — wakes on virtual device EVENTCALLBACK, drains loopback →
//                converts to float32 → writes to SPSC ring buffer
//   RenThread  — wakes on physical device EVENTCALLBACK, reads ring buffer →
//                converts to physical format → writes to render client
//
// The ring buffer decouples the two device clocks.  If the ring underruns
// (virtual device behind), RenThread outputs silence instead of glitching.
// If the ring overflows (capture getting ahead), the oldest frames are
// discarded silently.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define INITGUID
#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "bridge.h"
#include "channels.h"

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "winmm.lib")

static IMMDeviceEnumerator* g_de = nullptr;

// ── SPSC ring buffer ──────────────────────────────────────────────────────────
// Internal format: interleaved float32, up to RING_MAXCH channels.
// Producer = CapThread (writes wp), Consumer = RenThread (writes rp).

static const int RING_FRAMES = 8192; // power of 2  (~170 ms @ 48 kHz stereo)
static const int RING_MAXCH  = 8;

struct RingBuf {
    float             data[RING_FRAMES * RING_MAXCH];
    std::atomic<int>  ch{2};
    std::atomic<int>  wp{0};   // written by producer only
    std::atomic<int>  rp{0};   // written by consumer only

    void reset(int channels) {
        ch.store(channels, std::memory_order_relaxed);
        wp.store(0,        std::memory_order_relaxed);
        rp.store(0,        std::memory_order_release);
    }

    int readable() const {
        return wp.load(std::memory_order_acquire) -
               rp.load(std::memory_order_relaxed);
    }
    int writable() const { return RING_FRAMES - readable(); }

    // Write up to `frames` interleaved float32 frames (srcCh channels).
    // Drops tail silently if ring is full.
    void write(const float* src, int frames, int srcCh) {
        int space = writable();
        if (frames > space) frames = space;
        if (frames <= 0) return;
        int nch = ch.load(std::memory_order_relaxed);
        int w   = wp.load(std::memory_order_relaxed) & (RING_FRAMES - 1);
        int cap = std::min(srcCh, nch);
        for (int i = 0; i < frames; i++) {
            float* dst = data + ((w + i) & (RING_FRAMES - 1)) * nch;
            const float* s = src + i * srcCh;
            for (int c = 0; c < cap;  c++) dst[c] = s[c];
            for (int c = cap; c < nch; c++) dst[c] = 0.f;
        }
        wp.fetch_add(frames, std::memory_order_release);
    }

    // Read `frames` interleaved float32 frames (dstCh channels) into dst.
    // Performs linear resampling if ratio != 1.0.
    // ratio = srcRate / dstRate.  remainder tracks fractional src position.
    void read(float* dst, int frames, int dstCh, double ratio, double& remainder) {
        int nch = ch.load(std::memory_order_relaxed);
        int r   = rp.load(std::memory_order_relaxed);
        int w   = wp.load(std::memory_order_acquire);
        int cap = std::min(dstCh, nch);

        int totalConsumed = 0;
        for (int i = 0; i < frames; i++) {
            double srcPos = i * ratio + remainder;
            int    idx0   = (int)floor(srcPos);
            int    idx1   = idx0 + 1;
            float  frac   = (float)(srcPos - idx0);

            if (r + idx1 < w) {
                const float* s0 = data + ((r + idx0) & (RING_FRAMES - 1)) * nch;
                const float* s1 = data + ((r + idx1) & (RING_FRAMES - 1)) * nch;
                for (int c = 0; c < cap;   c++) dst[c] = s0[c] * (1.f - frac) + s1[c] * frac;
                for (int c = cap; c < dstCh; c++) dst[c] = 0.f;
                totalConsumed = idx1; 
            } else {
                memset(dst, 0, dstCh * sizeof(float));
            }
            dst += dstCh;
        }

        // Advance read pointer only by what we actually had available
        rp.fetch_add(totalConsumed, std::memory_order_release);
        
        // Update remainder based on actual consumption
        double finalPos = frames * ratio + remainder;
        if (r + (int)floor(finalPos) + 1 >= w) {
            // We underrun. Reset remainder for next time.
            remainder = 0.0;
        } else {
            remainder = finalPos - floor(finalPos);
        }
    }

    // Consumer: discard all pending data (e.g. on output device restart).
    void drain() {
        rp.store(wp.load(std::memory_order_acquire), std::memory_order_release);
    }
};

// ── format helpers ────────────────────────────────────────────────────────────

static bool IsFloat(const WAVEFORMATEX* fmt) {
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    }
    return false;
}

// Convert raw WASAPI capture bytes → float32 → ring buffer.
static void CaptureBytesToRing(const BYTE* src, UINT32 frames,
                                const WAVEFORMATEX* fmt, RingBuf& ring)
{
    int ch  = fmt->nChannels;
    int bps = fmt->wBitsPerSample;
    static const int CHUNK = 512;
    float tmp[CHUNK * RING_MAXCH];

    if (IsFloat(fmt) && bps == 32) {
        const float* f = reinterpret_cast<const float*>(src);
        while (frames > 0) {
            int n = std::min((int)frames, CHUNK);
            ring.write(f, n, ch);
            f += n * ch; frames -= n;
        }
    } else if (!IsFloat(fmt) && bps == 16) {
        const int16_t* s = reinterpret_cast<const int16_t*>(src);
        while (frames > 0) {
            int n = std::min((int)frames, CHUNK);
            for (int i = 0; i < n * ch; i++) tmp[i] = s[i] * (1.0f / 32768.0f);
            ring.write(tmp, n, ch);
            s += n * ch; frames -= n;
        }
    } else if (!IsFloat(fmt) && bps == 32) {
        const int32_t* s = reinterpret_cast<const int32_t*>(src);
        while (frames > 0) {
            int n = std::min((int)frames, CHUNK);
            for (int i = 0; i < n * ch; i++) tmp[i] = s[i] * (1.0f / 2147483648.0f);
            ring.write(tmp, n, ch);
            s += n * ch; frames -= n;
        }
    }
    // else: unsupported format — frames are silently dropped
}

// Convert ring buffer float32 → physical render bytes.
static void RingToRenderBytes(BYTE* dst, UINT32 frames,
                               const WAVEFORMATEX* fmt, RingBuf& ring,
                               double ratio, double& remainder)
{
    int ch  = fmt->nChannels;
    int bps = fmt->wBitsPerSample;
    static const int CHUNK = 512;
    float tmp[CHUNK * RING_MAXCH];

    if (IsFloat(fmt) && bps == 32) {
        ring.read(reinterpret_cast<float*>(dst), frames, ch, ratio, remainder);
    } else if (!IsFloat(fmt) && bps == 16) {
        int16_t* d = reinterpret_cast<int16_t*>(dst);
        while (frames > 0) {
            int n = std::min((int)frames, CHUNK);
            ring.read(tmp, n, ch, ratio, remainder);
            for (int i = 0; i < n * ch; i++) {
                float v = tmp[i] < -1.f ? -1.f : (tmp[i] > 1.f ? 1.f : tmp[i]);
                d[i] = (int16_t)(v * 32767.f);
            }
            d += n * ch; frames -= n;
        }
    } else if (!IsFloat(fmt) && bps == 32) {
        int32_t* d = reinterpret_cast<int32_t*>(dst);
        while (frames > 0) {
            int n = std::min((int)frames, CHUNK);
            ring.read(tmp, n, ch, ratio, remainder);
            for (int i = 0; i < n * ch; i++) {
                float v = tmp[i] < -1.f ? -1.f : (tmp[i] > 1.f ? 1.f : tmp[i]);
                d[i] = (int32_t)(v * 2147483647.f);
            }
            d += n * ch; frames -= n;
        }
    } else {
        // Unsupported format — write silence, discard ring data
        memset(dst, 0, (size_t)frames * fmt->nBlockAlign);
        ring.drain();
    }
}

// ── Bridge ────────────────────────────────────────────────────────────────────

struct Bridge {
    const wchar_t*    channelName;
    std::wstring      key;
    std::wstring      outputId;   // guarded by mu
    std::mutex        mu;
    std::atomic<bool> dirty{false}; // set by SetBridgeOutput → RenThread restarts
    std::atomic<bool> alive{true};
    RingBuf           ring;
    std::atomic<int>  capRate{48000};
    double            resampleRemainder{0.0};
    std::thread       capTh;
    std::thread       renTh;
};

static std::vector<Bridge*> g_bridges;
static std::mutex            g_bridgesMu;

// ── helpers ───────────────────────────────────────────────────────────────────

static std::wstring FindEndpointByName(const wchar_t* name) {
    IMMDeviceCollection* col = nullptr;
    if (FAILED(g_de->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return L"";
    UINT n = 0; col->GetCount(&n);
    std::wstring result;
    for (UINT i = 0; i < n && result.empty(); i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(col->Item(i, &dev))) continue;
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                if (wcscmp(pv.pwszVal, name) == 0) {
                    LPWSTR id = nullptr; dev->GetId(&id);
                    if (id) { result = id; CoTaskMemFree(id); }
                }
            PropVariantClear(&pv); ps->Release();
        }
        dev->Release();
    }
    col->Release();
    return result;
}

// ── Capture thread ────────────────────────────────────────────────────────────
// Paced by the virtual device's EVENTCALLBACK.
// Converts loopback audio to float32 and feeds the ring buffer.

static void CapThread(Bridge* b) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);
    DWORD mmIdx = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmIdx);

    while (b->alive) {
        std::wstring capId = FindEndpointByName(b->channelName);
        if (capId.empty()) { Sleep(2000); continue; }

        IMMDevice* capDev = nullptr;
        g_de->GetDevice(capId.c_str(), &capDev);
        if (!capDev) { Sleep(2000); continue; }

        // Silent keep-alive render with EVENTCALLBACK — drives loop timing.
        IAudioClient*       silCl    = nullptr;
        IAudioRenderClient* silRc    = nullptr;
        WAVEFORMATEX*       silFmt   = nullptr;
        UINT32              silBuf   = 0;
        HANDLE              silEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        bool ok = (silEvent != nullptr);

        if (ok && SUCCEEDED(capDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                             nullptr, (void**)&silCl)))
        {
            silCl->GetMixFormat(&silFmt);
            // Request 3ms buffer for lower latency
            ok = SUCCEEDED(silCl->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                             30000, 0, silFmt, nullptr)) &&
                 SUCCEEDED(silCl->SetEventHandle(silEvent)) &&
                 SUCCEEDED(silCl->GetService(__uuidof(IAudioRenderClient), (void**)&silRc));
            if (ok) silCl->GetBufferSize(&silBuf);
        } else {
            ok = false;
        }

        // Loopback capture from the same virtual endpoint.
        IAudioClient*        capCl  = nullptr;
        IAudioCaptureClient* capCc  = nullptr;
        WAVEFORMATEX*        capFmt = nullptr;
        if (ok) {
            IAudioClient* cl = nullptr;
            if (SUCCEEDED(capDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                           nullptr, (void**)&cl)))
            {
                WAVEFORMATEX* fmt = nullptr;
                cl->GetMixFormat(&fmt);
                IAudioCaptureClient* cc = nullptr;
                // Request 3ms buffer for lower latency
                if (SUCCEEDED(cl->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                             AUDCLNT_STREAMFLAGS_LOOPBACK,
                                             30000, 0, fmt, nullptr)) &&
                    SUCCEEDED(cl->GetService(__uuidof(IAudioCaptureClient), (void**)&cc)))
                {
                    capCl = cl; capCc = cc; capFmt = fmt;
                } else {
                    CoTaskMemFree(fmt); cl->Release(); ok = false;
                }
            } else {
                ok = false;
            }
        }

        if (!ok) {
            if (silRc) silRc->Release();
            if (silCl) { silCl->Release(); CoTaskMemFree(silFmt); }
            if (silEvent) CloseHandle(silEvent);
            capDev->Release();
            Sleep(2000);
            continue;
        }

        b->capRate = capFmt->nSamplesPerSec;
        b->ring.reset(capFmt->nChannels);
        silCl->Start();
        capCl->Start();
        fprintf(stderr, "[cap] %S started (%u-bit %uch %uHz)\n",
                b->channelName, capFmt->wBitsPerSample,
                capFmt->nChannels, capFmt->nSamplesPerSec);

        // Dynamic capture-rate measurement: counts actual frames captured per
        // wall-clock second so RenThread can compute the correct resample ratio
        // regardless of any driver clock anomalies.
        LARGE_INTEGER qpf, tMeas;
        QueryPerformanceFrequency(&qpf);
        QueryPerformanceCounter(&tMeas);
        LONGLONG measFrames = 0;

        while (b->alive) {
            WaitForSingleObject(silEvent, 50);

            // Keep virtual device alive
            UINT32 pad = 0; silCl->GetCurrentPadding(&pad);
            UINT32 avail = silBuf - pad;
            if (avail > 0) {
                BYTE* d = nullptr;
                if (SUCCEEDED(silRc->GetBuffer(avail, &d)))
                    silRc->ReleaseBuffer(avail, AUDCLNT_BUFFERFLAGS_SILENT);
            }

            // Drain loopback → ring
            UINT32 pktSize = 0;
            while (SUCCEEDED(capCc->GetNextPacketSize(&pktSize)) && pktSize > 0) {
                BYTE* src = nullptr; UINT32 frames = 0; DWORD flags = 0;
                if (FAILED(capCc->GetBuffer(&src, &frames, &flags, nullptr, nullptr))) break;

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    // Write silence into ring — keep fill level consistent
                    static const float zeros[512 * RING_MAXCH] = {};
                    int rem = (int)frames;
                    while (rem > 0) {
                        int n = std::min(rem, 512);
                        b->ring.write(zeros, n, capFmt->nChannels);
                        rem -= n;
                    }
                } else {
                    CaptureBytesToRing(src, frames, capFmt, b->ring);
                }
                capCc->ReleaseBuffer(frames);
                measFrames += frames;
            }

            // Update measured capture rate every ~1.5 seconds.
            LARGE_INTEGER tNow; QueryPerformanceCounter(&tNow);
            double measSec = (double)(tNow.QuadPart - tMeas.QuadPart) / qpf.QuadPart;
            if (measSec >= 1.5 && measFrames > 0) {
                int measured = (int)(measFrames / measSec);
                // Clamp to ±50 % of the nominal rate to ignore measurement noise.
                int nominal = capFmt->nSamplesPerSec;
                if (measured >= nominal / 2 && measured <= nominal * 3)
                    b->capRate.store(measured);
                measFrames = 0;
                tMeas = tNow;
            }
        }

        silCl->Stop(); capCl->Stop();
        silRc->Release(); silCl->Release(); CoTaskMemFree(silFmt);
        capCc->Release(); capCl->Release(); CoTaskMemFree(capFmt);
        CloseHandle(silEvent);
        capDev->Release();
    }

    if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
    timeEndPeriod(1);
    CoUninitialize();
}

// ── Render thread ─────────────────────────────────────────────────────────────
// Paced by the physical device's EVENTCALLBACK.
// Reads from the ring buffer (silence on underrun) → writes to render client.

static void RenThread(Bridge* b) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);
    DWORD mmIdx = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmIdx);

    while (b->alive) {
        std::wstring outputId;
        { std::lock_guard<std::mutex> lk(b->mu); outputId = b->outputId; }
        b->dirty = false;

        if (outputId.empty() || outputId == L"none") {
            // No output configured — drain ring and idle.
            while (b->alive && !b->dirty) {
                b->ring.drain();
                Sleep(50);
            }
            continue;
        }

        // Resolve physical output device.
        IMMDevice* renDev = nullptr;
        if (outputId == L"Default") {
            g_de->GetDefaultAudioEndpoint(eRender, eConsole, &renDev);
        } else {
            g_de->GetDevice(outputId.c_str(), &renDev);
            if (!renDev) {
                std::wstring byName = FindEndpointByName(outputId.c_str());
                if (!byName.empty()) g_de->GetDevice(byName.c_str(), &renDev);
            }
        }
        if (!renDev) { Sleep(2000); continue; }

        // Render client with EVENTCALLBACK on physical device.
        IAudioClient*       renCl    = nullptr;
        IAudioRenderClient* renRc    = nullptr;
        WAVEFORMATEX*       renFmt   = nullptr;
        UINT32              renBuf   = 0;
        HANDLE              renEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        bool ok = (renEvent != nullptr);

        if (ok && SUCCEEDED(renDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                             nullptr, (void**)&renCl)))
        {
            renCl->GetMixFormat(&renFmt);
            // Request 3ms buffer for lower latency
            ok = SUCCEEDED(renCl->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                             30000, 0, renFmt, nullptr)) &&
                 SUCCEEDED(renCl->SetEventHandle(renEvent)) &&
                 SUCCEEDED(renCl->GetService(__uuidof(IAudioRenderClient), (void**)&renRc));
            if (ok) renCl->GetBufferSize(&renBuf);
        } else {
            ok = false;
        }

        if (!ok) {
            if (renRc) renRc->Release();
            if (renCl) { renCl->Release(); CoTaskMemFree(renFmt); }
            if (renEvent) CloseHandle(renEvent);
            renDev->Release();
            Sleep(2000);
            continue;
        }

        // Discard any stale ring data before starting playback.
        b->ring.drain();
        b->resampleRemainder = 0.0;

        renCl->Start();
        fprintf(stderr, "[ren] %S -> %S (%u-bit %uch %uHz renBuf=%u)\n",
                b->channelName, outputId.c_str(),
                renFmt->wBitsPerSample, renFmt->nChannels,
                renFmt->nSamplesPerSec, renBuf);

        while (b->alive && !b->dirty) {
            DWORD wr = WaitForSingleObject(renEvent, 50);
            if (wr == WAIT_TIMEOUT) continue;

            UINT32 pad = 0; renCl->GetCurrentPadding(&pad);
            UINT32 frames = renBuf - pad;
            if (frames == 0) continue;

            BYTE* dst = nullptr;
            if (FAILED(renRc->GetBuffer(frames, &dst))) continue;

            double ratio = (double)b->capRate.load() / renFmt->nSamplesPerSec;

            // RingToRenderBytes outputs silence on underrun — no glitch.
            RingToRenderBytes(dst, frames, renFmt, b->ring, ratio, b->resampleRemainder);
            renRc->ReleaseBuffer(frames, 0);
        }

        renCl->Stop();
        renRc->Release(); renCl->Release(); CoTaskMemFree(renFmt);
        CloseHandle(renEvent);
        renDev->Release();
    }

    if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
    timeEndPeriod(1);
    CoUninitialize();
}

// ── public API ────────────────────────────────────────────────────────────────

void StartBridges(IMMDeviceEnumerator* de,
                  const std::map<std::wstring, std::wstring>& channelOutputs)
{
    std::lock_guard<std::mutex> lk(g_bridgesMu);
    if (!g_bridges.empty()) return;
    g_de = de;

    for (int i = 0; i < VIRTUAL_CHANNEL_COUNT; i++) {
        Bridge* b  = new Bridge{};
        b->channelName = VIRTUAL_CHANNELS[i].name;
        b->key         = VIRTUAL_CHANNELS[i].key;
        auto it = channelOutputs.find(b->key);
        if (it != channelOutputs.end()) b->outputId = it->second;
        b->capTh = std::thread(CapThread, b);
        b->renTh = std::thread(RenThread, b);
        g_bridges.push_back(b);
    }
}

void SetBridgeOutput(const std::wstring& key, const std::wstring& deviceId) {
    std::lock_guard<std::mutex> lk(g_bridgesMu);
    for (auto* b : g_bridges) {
        if (b->key == key) {
            { std::lock_guard<std::mutex> bl(b->mu); b->outputId = deviceId; }
            b->dirty = true;
            return;
        }
    }
}

void StopBridges() {
    std::lock_guard<std::mutex> lk(g_bridgesMu);
    for (auto* b : g_bridges) {
        b->alive = false;
        b->capTh.join();
        b->renTh.join();
        delete b;
    }
    g_bridges.clear();
}
