#include <portcls.h>
#include "topo.h"

// Bridge pins use KSPIN_COMMUNICATION_NONE and analog data ranges,
// matching the simpleaudiosample pattern (speakertoptable.h / speakerwavtable.h).
// KSPIN_COMMUNICATION_BRIDGE is wrong here and causes STATUS_RANGE_NOT_FOUND
// when IPortWaveRT::Init processes the connected wave filter.

static KSDATARANGE TopoBridgeRange = {
    sizeof(KSDATARANGE),
    0, 0, 0,
    STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
    STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};
static PKSDATARANGE TopoBridgeRangePtr[] = { &TopoBridgeRange };

// Inline KSPIN_DESCRIPTOR to avoid CRT static initializers
static PCPIN_DESCRIPTOR TopoPins[] = {
    // Pin 0: topo source IN -- receives audio from wave filter's bridge-out pin
    {
        0, 0, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            SIZEOF_ARRAY(TopoBridgeRangePtr), TopoBridgeRangePtr,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            nullptr, 0
        }
    },
    // Pin 1: topo lineout DEST -- exposes physical output to OS
    {
        0, 0, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            SIZEOF_ARRAY(TopoBridgeRangePtr), TopoBridgeRangePtr,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_SPEAKER,
            nullptr, 0
        }
    }
};

static PCCONNECTION_DESCRIPTOR TopoConnections[] = {
    { PCFILTER_NODE, 0, PCFILTER_NODE, 1 }
};

static PCFILTER_DESCRIPTOR TopoFilterDesc = {
    0, nullptr,
    sizeof(PCPIN_DESCRIPTOR), SIZEOF_ARRAY(TopoPins), TopoPins,
    sizeof(PCNODE_DESCRIPTOR), 0, nullptr,
    SIZEOF_ARRAY(TopoConnections), TopoConnections,
    0, nullptr
};

#pragma code_seg("PAGE")

CMiniportTopology::~CMiniportTopology()
{
    PAGED_CODE();
    if (m_Port) { m_Port->Release(); m_Port = nullptr; }
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopology::Init(
    _In_ PUNKNOWN      UnknownAdapter,
    _In_ PRESOURCELIST ResourceList,
    _In_ PPORTTOPOLOGY Port
)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    m_Port = Port;
    m_Port->AddRef();
    m_FilterDesc = &TopoFilterDesc;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopology::GetDescription(_Out_ PPCFILTER_DESCRIPTOR* OutDesc)
{
    PAGED_CODE();
    *OutDesc = m_FilterDesc;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopology::DataRangeIntersection(
    _In_  ULONG        PinId,
    _In_  PKSDATARANGE ClientRange,
    _In_  PKSDATARANGE MyRange,
    _In_  ULONG        OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
          PVOID        ResultantFormat,
    _Out_ PULONG       ResultantFormatLength
)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(ClientRange);
    UNREFERENCED_PARAMETER(MyRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);
    return STATUS_NOT_IMPLEMENTED;
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopology::NonDelegatingQueryInterface(REFIID iid, PVOID* ppv)
{
    PAGED_CODE();
    if (IsEqualGUID(iid, IID_IUnknown)) {
        *ppv = PVOID(PUNKNOWN(PMINIPORT(this)));
    } else if (IsEqualGUID(iid, IID_IMiniport)) {
        *ppv = PVOID(PMINIPORT(this));
    } else if (IsEqualGUID(iid, IID_IMiniportTopology)) {
        *ppv = PVOID(PMINIPORTTOPOLOGY(this));
    } else {
        *ppv = nullptr;
        return STATUS_NOINTERFACE;
    }
    PUNKNOWN(*ppv)->AddRef();
    return STATUS_SUCCESS;
}

NTSTATUS
CreateMiniportTopology(
    _Out_ PUNKNOWN* Unknown,
    _In_  REFCLSID,
    _In_  PUNKNOWN  UnknownOuter,
    _In_  POOL_FLAGS PoolFlags
)
{
    UNREFERENCED_PARAMETER(PoolFlags);
    CMiniportTopology* obj = new(NonPagedPoolNx, DRIVER_TAG) CMiniportTopology(UnknownOuter);
    if (!obj) return STATUS_INSUFFICIENT_RESOURCES;
    obj->AddRef();
    *Unknown = reinterpret_cast<PUNKNOWN>(obj);
    return STATUS_SUCCESS;
}
