#include <portcls.h>
#include "stream.h"
#include "miniport.h"

// ---------------------------------------------------------------------------
// Timer callback — fired every PERIOD_MS by the high-resolution timer.
// Runs at DISPATCH_LEVEL; must only touch non-paged memory.
// ---------------------------------------------------------------------------
void
CMiniportWaveRTStream::TimerNotifyExt(
    _In_ PEX_TIMER Timer,
    _In_ PVOID Context
)
{
    UNREFERENCED_PARAMETER(Timer);
    if (Context)
        static_cast<CMiniportWaveRTStream*>(Context)->OnTimer();
}

void CMiniportWaveRTStream::UpdatePosition()
{
    KIRQL irql;
    KeAcquireSpinLock(&m_PositionLock, &irql);

    if (m_Running && m_BufferSize > 0) {
        LONGLONG now = KeQueryInterruptTime();
        LONGLONG elapsed = now - m_ullDmaTimeStamp + m_hnsElapsedTimeCarryForward;
        
        // ByteDisplacement = (elapsedHns * BytesPerSec) / 10,000,000
        LONGLONG rate = (LONGLONG)m_SampleRate * (LONGLONG)m_BytesPerFrame;
        LONGLONG ByteDisplacement = (elapsed * rate) / 10000000;
        
        m_hnsElapsedTimeCarryForward = elapsed - (ByteDisplacement * 10000000) / rate;
        m_ullLinearPosition += ByteDisplacement;
        m_ullDmaTimeStamp = now;
    }

    KeReleaseSpinLock(&m_PositionLock, irql);
}

// Runs at DISPATCH_LEVEL — no paged memory, no PAGED_CODE().
void CMiniportWaveRTStream::OnTimer()
{
    UpdatePosition();

    // Signal all registered notification events so loopback-capture clients wake.
    KIRQL irql;
    KeAcquireSpinLock(&m_NotifLock, &irql);
    for (ULONG i = 0; i < m_NotifCount; i++) {
        KeSetEvent(m_NotifEvents[i], 0, FALSE);
    }
    KeReleaseSpinLock(&m_NotifLock, irql);
}

// ---------------------------------------------------------------------------

CMiniportWaveRTStream::~CMiniportWaveRTStream()
{
    // Cancel timer before freeing.
    if (m_NotificationTimer) {
        ExCancelTimer(m_NotificationTimer, nullptr);
        ExDeleteTimer(m_NotificationTimer, TRUE, TRUE, nullptr);
        m_NotificationTimer = nullptr;
    }

    // Release notification event references.
    KIRQL irql;
    KeAcquireSpinLock(&m_NotifLock, &irql);
    for (ULONG i = 0; i < m_NotifCount; i++) {
        ObDereferenceObject(m_NotifEvents[i]);
        m_NotifEvents[i] = nullptr;
    }
    m_NotifCount = 0;
    KeReleaseSpinLock(&m_NotifLock, irql);

    if (m_AudioBufferMdl && m_PortStream) {
        m_PortStream->FreePagesFromMdl(m_AudioBufferMdl);
        m_AudioBufferMdl = nullptr;
    }
    if (m_PortStream) {
        m_PortStream->Release();
        m_PortStream = nullptr;
    }
}

#pragma code_seg("PAGE")

NTSTATUS
CMiniportWaveRTStream::Init(
    _In_ CMiniportWaveRT*  Miniport,
    _In_ PPORTWAVERTSTREAM PortStream
)
{
    PAGED_CODE();
    m_Miniport   = Miniport;
    m_PortStream = PortStream;
    m_PortStream->AddRef();

    m_BufferSize     = 0;
    m_AudioBufferMdl = nullptr;
    m_ullLinearPosition = 0;
    m_ullDmaTimeStamp   = 0;
    m_hnsElapsedTimeCarryForward = 0;
    m_Running        = 0;
    m_NotifCount     = 0;

    // Default format: 48 kHz stereo 16-bit
    m_SampleRate     = 48000;
    m_BytesPerFrame  = 4;
    m_NotificationCount = 0;

    m_NotificationTimer = ExAllocateTimer(TimerNotifyExt, this, EX_TIMER_HIGH_RESOLUTION);
    KeInitializeSpinLock(&m_NotifLock);
    KeInitializeSpinLock(&m_PositionLock);

    RtlZeroMemory(m_NotifEvents, sizeof(m_NotifEvents));
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::AllocateAudioBuffer(
    _In_  ULONG                RequestedSize,
    _Out_ PMDL*                AudioBufferMdl,
    _Out_ ULONG*               ActualSize,
    _Out_ ULONG*               OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE* CacheType
)
{
    PAGED_CODE();

    ULONG size = max(RequestedSize, (ULONG)PAGE_SIZE);
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    PHYSICAL_ADDRESS highAddr;
    highAddr.QuadPart = MAXLONGLONG;
    PMDL mdl = m_PortStream->AllocatePagesForMdl(highAddr, size);
    if (!mdl) return STATUS_INSUFFICIENT_RESOURCES;

    m_AudioBufferMdl = mdl;
    m_BufferSize     = size;

    *AudioBufferMdl      = mdl;
    *ActualSize          = size;
    *OffsetFromFirstPage = 0;
    *CacheType           = MmCached;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(void)
CMiniportWaveRTStream::FreeAudioBuffer(
    _In_ PMDL  AudioBufferMdl,
    _In_ ULONG BufferSize
)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(BufferSize);
    m_PortStream->FreePagesFromMdl(AudioBufferMdl);
    m_AudioBufferMdl = nullptr;
    m_BufferSize     = 0;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::SetFormat(_In_ PKSDATAFORMAT Format)
{
    PAGED_CODE();

    if (Format->FormatSize >= sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX)) {
        auto wfx = reinterpret_cast<PWAVEFORMATEX>(Format + 1);
        m_SampleRate    = wfx->nSamplesPerSec;
        m_BytesPerFrame = wfx->nBlockAlign;
    }
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::SetState(_In_ KSSTATE State)
{
    PAGED_CODE();

    if (State == KSSTATE_RUN) {
        if (InterlockedExchange(&m_Running, 1) == 0) {
            m_ullLinearPosition = 0;
            m_ullDmaTimeStamp = KeQueryInterruptTime();
            m_hnsElapsedTimeCarryForward = 0;
            
            if (m_NotificationTimer) {
                LARGE_INTEGER due;
                due.QuadPart = -PERIOD_100NS; // relative 10ms
                ExSetTimer(m_NotificationTimer, due.QuadPart, PERIOD_100NS, nullptr);
            }
        }
    } else {
        if (InterlockedExchange(&m_Running, 0) != 0) {
            if (m_NotificationTimer) {
                ExCancelTimer(m_NotificationTimer, nullptr);
            }
        }
    }
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::GetClockRegister(_Out_ PKSRTAUDIO_HWREGISTER Register)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_IMPLEMENTED;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::GetPositionRegister(_Out_ PKSRTAUDIO_HWREGISTER Register)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_IMPLEMENTED;
}

STDMETHODIMP_(void)
CMiniportWaveRTStream::GetHWLatency(_Out_ PKSRTAUDIO_HWLATENCY hwLatency)
{
    PAGED_CODE();
    hwLatency->FifoSize     = 0;
    hwLatency->ChipsetDelay = 0;
    hwLatency->CodecDelay   = 0;
}

// ---------------------------------------------------------------------------
// IMiniportWaveRTStreamNotification — buffer allocation variants
// ---------------------------------------------------------------------------

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::AllocateBufferWithNotification(
    _In_  ULONG                NotificationCount,
    _In_  ULONG                RequestedSize,
    _Out_ PMDL*                AudioBufferMdl,
    _Out_ ULONG*               ActualSize,
    _Out_ ULONG*               OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE* CacheType
)
{
    PAGED_CODE();
    m_NotificationCount = NotificationCount;
    
    return AllocateAudioBuffer(
        RequestedSize, AudioBufferMdl, ActualSize, OffsetFromFirstPage, CacheType);
}

STDMETHODIMP_(void)
CMiniportWaveRTStream::FreeBufferWithNotification(
    _In_ PMDL  AudioBufferMdl,
    _In_ ULONG BufferSize
)
{
    PAGED_CODE();
    FreeAudioBuffer(AudioBufferMdl, BufferSize);
}

// ---------------------------------------------------------------------------
// Notification event registration
// The audio engine passes a PKEVENT (already referenced by the kernel).
// We take our own reference so the object stays alive until our DPC runs.
// ---------------------------------------------------------------------------

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::RegisterNotificationEvent(_In_ PKEVENT NotificationEvent)
{
    PAGED_CODE();

    ObReferenceObject(NotificationEvent);

    KIRQL irql;
    NTSTATUS status;
    KeAcquireSpinLock(&m_NotifLock, &irql);
    if (m_NotifCount < MAX_NOTIFICATION_EVENTS) {
        m_NotifEvents[m_NotifCount++] = NotificationEvent;
        status = STATUS_SUCCESS;
    } else {
        status = STATUS_INSUFFICIENT_RESOURCES;
    }
    KeReleaseSpinLock(&m_NotifLock, irql);

    if (!NT_SUCCESS(status))
        ObDereferenceObject(NotificationEvent);

    return status;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::UnregisterNotificationEvent(_In_ PKEVENT NotificationEvent)
{
    PAGED_CODE();

    KIRQL irql;
    KeAcquireSpinLock(&m_NotifLock, &irql);
    for (ULONG i = 0; i < m_NotifCount; i++) {
        if (m_NotifEvents[i] == NotificationEvent) {
            ObDereferenceObject(m_NotifEvents[i]);
            m_NotifEvents[i] = m_NotifEvents[--m_NotifCount];
            m_NotifEvents[m_NotifCount] = nullptr;
            break;
        }
    }
    KeReleaseSpinLock(&m_NotifLock, irql);

    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::NonDelegatingQueryInterface(REFIID iid, PVOID* ppv)
{
    PAGED_CODE();
    if (IsEqualGUID(iid, IID_IUnknown)) {
        *ppv = PVOID(PUNKNOWN(PMINIPORTWAVERTSTREAM(this)));
    } else if (IsEqualGUID(iid, IID_IMiniportWaveRTStream)) {
        *ppv = PVOID(PMINIPORTWAVERTSTREAM(this));
    } else if (IsEqualGUID(iid, IID_IMiniportWaveRTStreamNotification)) {
        *ppv = PVOID(PMINIPORTWAVERTSTREAMNOTIFICATION(this));
    } else {
        *ppv = nullptr;
        return STATUS_NOINTERFACE;
    }
    PUNKNOWN(*ppv)->AddRef();
    return STATUS_SUCCESS;
}

#pragma code_seg()

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::GetPosition(_Out_ PKSAUDIO_POSITION Position)
{
    if (m_Running) {
        UpdatePosition();
    }

    ULONG play = (ULONG)(m_ullLinearPosition % m_BufferSize);
    // WriteOffset is one period ahead of PlayOffset so GetCurrentPadding()
    // reflects actual pending data rather than always returning zero.
    ULONG periodBytes = (m_SampleRate / 100) * m_BytesPerFrame; // 10 ms worth
    if (periodBytes == 0 || m_BufferSize == 0) periodBytes = PAGE_SIZE;
    ULONG write = (play + periodBytes) % m_BufferSize;

    Position->PlayOffset  = play;
    Position->WriteOffset = write;
    return STATUS_SUCCESS;
}
