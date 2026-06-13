#pragma once

#include <portcls.h>
#include <stdunk.h>

class CMiniportWaveRT;

#define MAX_NOTIFICATION_EVENTS 8

// Period the DPC timer fires — 10 ms is standard for Windows audio.
// Loopback-capture latency tracks this directly.
#define PERIOD_MS       10
#define PERIOD_100NS    (PERIOD_MS * 10000LL)

// IMiniportWaveRTStreamNotification inherits IMiniportWaveRTStream, so we only
// need the one base interface.
class CMiniportWaveRTStream
    : public IMiniportWaveRTStreamNotification
    , public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWaveRTStream);
    ~CMiniportWaveRTStream();

    // IMiniportWaveRTStream
    IMP_IMiniportWaveRTStream;

    // IMiniportWaveRTStreamNotification extras
    STDMETHODIMP_(NTSTATUS) AllocateBufferWithNotification(
        _In_  ULONG               NotificationCount,
        _In_  ULONG               RequestedSize,
        _Out_ PMDL*               AudioBufferMdl,
        _Out_ ULONG*              ActualSize,
        _Out_ ULONG*              OffsetFromFirstPage,
        _Out_ MEMORY_CACHING_TYPE* CacheType);
    STDMETHODIMP_(void) FreeBufferWithNotification(
        _In_ PMDL  AudioBufferMdl,
        _In_ ULONG BufferSize);
    STDMETHODIMP_(NTSTATUS) RegisterNotificationEvent(_In_ PKEVENT NotificationEvent);
    STDMETHODIMP_(NTSTATUS) UnregisterNotificationEvent(_In_ PKEVENT NotificationEvent);

    NTSTATUS Init(_In_ CMiniportWaveRT* Miniport, _In_ PPORTWAVERTSTREAM PortStream);

    // Called from the DPC — advances play position and signals notification events.
    void UpdatePosition();
    void OnTimer();

private:
    CMiniportWaveRT*    m_Miniport;
    PPORTWAVERTSTREAM   m_PortStream;
    PMDL                m_AudioBufferMdl;
    ULONG               m_BufferSize;

    // Clock / position state (updated in DPC, must stay in non-paged memory)
    LONGLONG            m_ullLinearPosition; // total bytes processed (non-wrapping)
    LONGLONG            m_ullDmaTimeStamp;   // KeQueryInterruptTime at last update
    LONGLONG            m_hnsElapsedTimeCarryForward; // remainder of 100ns units
    
    ULONG               m_SampleRate;       // frames per second
    ULONG               m_BytesPerFrame;    // nChannels * nBitsPerSample / 8
    LONG                m_Running;          // 1 while KSSTATE_RUN
    ULONG               m_NotificationCount;// requested interrupts per buffer cycle

    PEX_TIMER           m_NotificationTimer;

    static EXT_CALLBACK TimerNotifyExt;

    // Notification events signalled once per period to wake loopback-capture clients.
    PKEVENT             m_NotifEvents[MAX_NOTIFICATION_EVENTS];
    ULONG               m_NotifCount;
    KSPIN_LOCK          m_NotifLock;
    KSPIN_LOCK          m_PositionLock;
};
