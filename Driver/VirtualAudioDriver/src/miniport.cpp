#include <portcls.h>
#include "miniport.h"
#include "stream.h"

// Float32 data range — listed first so the Audio Engine picks it for shared mode.
// WASAPI shared mode requires the device to accept IEEE_FLOAT; without this entry
// Initialize(AUDCLNT_SHAREMODE_SHARED, ...) returns AUDCLNT_E_UNSUPPORTED_FORMAT.
static KSDATARANGE_AUDIO FloatStreamRange = {
    {
        sizeof(KSDATARANGE_AUDIO),
        0,    // Flags
        0,    // SampleSize
        0,    // Reserved
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
    },
    2,      // MaximumChannels
    32,     // MinimumBitsPerSample
    32,     // MaximumBitsPerSample
    48000,  // MinimumSampleFrequency
    48000   // MaximumSampleFrequency
};

// PCM fallback range for exclusive-mode clients.
static KSDATARANGE_AUDIO RenderStreamRange = {
    {
        sizeof(KSDATARANGE_AUDIO),
        0,    // Flags
        0,    // SampleSize
        0,    // Reserved
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
    },
    2,      // MaximumChannels
    16,     // MinimumBitsPerSample
    32,     // MaximumBitsPerSample
    48000,  // MinimumSampleFrequency
    48000   // MaximumSampleFrequency
};

static PKSDATARANGE RenderStreamRangePtr[] = {
    (PKSDATARANGE)&FloatStreamRange,   // float32 first — Audio Engine prefers it
    (PKSDATARANGE)&RenderStreamRange
};

// Bridge pin data range: analog, no specifier (matches simpleaudiosample pattern)
static KSDATARANGE BridgeRange = {
    sizeof(KSDATARANGE),
    0, 0, 0,
    STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
    STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};
static PKSDATARANGE BridgeRangePtr[] = { &BridgeRange };

// Inline all KSPIN_DESCRIPTOR fields to avoid CRT static initializers
static PCPIN_DESCRIPTOR WavePins[] = {
    // Pin 0: streaming render SINK -- client writes PCM here
    {
        1, 1, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(RenderStreamRangePtr), RenderStreamRangePtr,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            nullptr, 0
        }
    },
    // Pin 1: wave-to-topo bridge output -- KSPIN_COMMUNICATION_NONE per simpleaudiosample
    {
        0, 0, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            SIZEOF_ARRAY(BridgeRangePtr), BridgeRangePtr,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            nullptr, 0
        }
    }
};

// Internal signal flow: streaming pin -> bridge pin
static PCCONNECTION_DESCRIPTOR WaveConnections[] = {
    { PCFILTER_NODE, 0, PCFILTER_NODE, 1 }
};

static PCFILTER_DESCRIPTOR FilterDesc = {
    0, nullptr,
    sizeof(PCPIN_DESCRIPTOR), SIZEOF_ARRAY(WavePins), WavePins,
    sizeof(PCNODE_DESCRIPTOR), 0, nullptr,
    SIZEOF_ARRAY(WaveConnections), WaveConnections,
    0, nullptr
};

#pragma code_seg("PAGE")

CMiniportWaveRT::~CMiniportWaveRT()
{
    PAGED_CODE();
    if (m_Port) { m_Port->Release(); m_Port = nullptr; }
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::Init(
    _In_ PUNKNOWN      UnknownAdapter,
    _In_ PRESOURCELIST ResourceList,
    _In_ PPORTWAVERT   Port
)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    m_Port = Port;
    m_Port->AddRef();
    m_FilterDesc = &FilterDesc;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::GetDescription(_Out_ PPCFILTER_DESCRIPTOR* OutFilterDescriptor)
{
    PAGED_CODE();
    *OutFilterDescriptor = m_FilterDesc;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::DataRangeIntersection(
    _In_        ULONG           PinId,
    _In_        PKSDATARANGE    ClientDataRange,
    _In_        PKSDATARANGE    MyDataRange,
    _In_        ULONG           OutputBufferLength,
    _Out_opt_   PVOID           ResultantFormat,
    _Out_       PULONG          ResultantFormatLength
)
{
    PAGED_CODE();

    if (!ClientDataRange || !MyDataRange || !ResultantFormatLength)
        return STATUS_INVALID_PARAMETER;

    if (PinId != 0) // Only pin 0 is streaming
        return STATUS_NOT_SUPPORTED;

    // We only support WAVEFORMATEX specifier for streaming
    if (!IsEqualGUID(ClientDataRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
        return STATUS_NOT_SUPPORTED;

    auto clientRange = reinterpret_cast<PKSDATARANGE_AUDIO>(ClientDataRange);
    auto myRange     = reinterpret_cast<PKSDATARANGE_AUDIO>(MyDataRange);

    // Basic validation that it's audio and matches our subtype (Float or PCM)
    if (!IsEqualGUID(clientRange->DataRange.SubFormat, myRange->DataRange.SubFormat))
        return STATUS_NO_MATCH;

    // Check channels, bits, and sample rate overlap
    if (clientRange->MaximumChannels < myRange->MaximumChannels) return STATUS_NO_MATCH;
    if (clientRange->MinimumSampleFrequency > myRange->MaximumSampleFrequency) return STATUS_NO_MATCH;
    if (clientRange->MaximumSampleFrequency < myRange->MinimumSampleFrequency) return STATUS_NO_MATCH;

    ULONG formatSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    *ResultantFormatLength = formatSize;

    if (OutputBufferLength == 0) return STATUS_BUFFER_OVERFLOW;
    if (OutputBufferLength < formatSize) return STATUS_BUFFER_TOO_SMALL;
    if (!ResultantFormat) return STATUS_INVALID_PARAMETER;

    // Construct the intersection format. 
    // We prefer 48kHz stereo 32-bit float or 16-bit PCM as defined in our ranges.
    auto fmt = reinterpret_cast<PKSDATAFORMAT_WAVEFORMATEXTENSIBLE>(ResultantFormat);
    RtlZeroMemory(fmt, formatSize);

    fmt->DataFormat.FormatSize = formatSize;
    fmt->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    fmt->DataFormat.SubFormat = myRange->DataRange.SubFormat;
    fmt->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

    auto wfx = &fmt->WaveFormatExt;
    wfx->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx->Format.nChannels = 2;
    wfx->Format.nSamplesPerSec = 48000;
    wfx->Format.wBitsPerSample = (IsEqualGUID(myRange->DataRange.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) ? 32 : 16;
    wfx->Format.nBlockAlign = (wfx->Format.nChannels * wfx->Format.wBitsPerSample) / 8;
    wfx->Format.nAvgBytesPerSec = wfx->Format.nSamplesPerSec * wfx->Format.nBlockAlign;
    wfx->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    wfx->Samples.wValidBitsPerSample = wfx->Format.wBitsPerSample;
    wfx->dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    wfx->SubFormat = myRange->DataRange.SubFormat;

    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::NewStream(
    _Out_ PMINIPORTWAVERTSTREAM* Stream,
    _In_  PPORTWAVERTSTREAM      PortStream,
    _In_  ULONG                  Pin,
    _In_  BOOLEAN                Capture,
    _In_  PKSDATAFORMAT          DataFormat
)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Pin);
    UNREFERENCED_PARAMETER(Capture);
    UNREFERENCED_PARAMETER(DataFormat);

    CMiniportWaveRTStream* stream =
        new(NonPagedPoolNx, DRIVER_TAG) CMiniportWaveRTStream(nullptr);
    if (!stream) return STATUS_INSUFFICIENT_RESOURCES;

    NTSTATUS status = stream->Init(this, PortStream);
    if (!NT_SUCCESS(status)) { stream->Release(); return status; }

    *Stream = stream;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::NonDelegatingQueryInterface(REFIID iid, PVOID* ppv)
{
    if (IsEqualGUID(iid, IID_IUnknown)) {
        *ppv = PVOID(PUNKNOWN(PMINIPORT(this)));
    } else if (IsEqualGUID(iid, IID_IMiniport)) {
        *ppv = PVOID(PMINIPORT(this));
    } else if (IsEqualGUID(iid, IID_IMiniportWaveRT)) {
        *ppv = PVOID(PMINIPORTWAVERT(this));
    } else {
        *ppv = nullptr;
        return STATUS_NOINTERFACE;
    }
    PUNKNOWN(*ppv)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::GetDeviceDescription(_Out_ PDEVICE_DESCRIPTION OutDesc)
{
    PAGED_CODE();
    RtlZeroMemory(OutDesc, sizeof(DEVICE_DESCRIPTION));
    OutDesc->Version = DEVICE_DESCRIPTION_VERSION;
    return STATUS_SUCCESS;
}

NTSTATUS
CreateMiniportWaveRT(
    _Out_ PUNKNOWN* Unknown,
    _In_  REFCLSID,
    _In_  PUNKNOWN  UnknownOuter,
    _In_  POOL_FLAGS PoolFlags
)
{
    UNREFERENCED_PARAMETER(PoolFlags);
    CMiniportWaveRT* obj = new(NonPagedPoolNx, DRIVER_TAG) CMiniportWaveRT(UnknownOuter);
    if (!obj) return STATUS_INSUFFICIENT_RESOURCES;
    obj->AddRef();
    *Unknown = reinterpret_cast<PUNKNOWN>(obj);
    return STATUS_SUCCESS;
}
