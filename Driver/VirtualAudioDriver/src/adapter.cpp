#include <portcls.h>
#include <wdmguid.h>
#include "driver.h"
#include "topo.h"

#define CHANNEL_COUNT 3

static WCHAR g_WaveName0[] = L"WAVE0", g_WaveName1[] = L"WAVE1", g_WaveName2[] = L"WAVE2";
static WCHAR g_TopoName0[] = L"TOPO0", g_TopoName1[] = L"TOPO1", g_TopoName2[] = L"TOPO2";
static PWSTR g_WaveNames[CHANNEL_COUNT] = { g_WaveName0, g_WaveName1, g_WaveName2 };
static PWSTR g_TopoNames[CHANNEL_COUNT] = { g_TopoName0, g_TopoName1, g_TopoName2 };

static const PCWSTR g_ChannelNames[CHANNEL_COUNT] = {
    L"TheAudio.app Games",
    L"TheAudio.app Media",
    L"TheAudio.app Voice",
};

// Proxy CLSID for KS device interfaces (required for audio endpoint enumeration)
static const WCHAR g_ProxyClsid[] = L"{17CCA71B-ECD7-11D0-B908-00A0C9223196}";

// Store PDO for use in SetEndpointName
static PDEVICE_OBJECT g_Pdo = nullptr;

#pragma code_seg("PAGE")

// Write the channel name to the topology device interface registry key using
// the same technique as SynchronousAudioRouter: ZwSetValueKey from kernel mode
// bypasses the usermode ACL restrictions on the MMDEVAPI property store.
//
// Writes:
//  <interface key>\FriendlyName = name
//  <interface key>\CLSID       = proxy clsid
//  <interface key>\MSEP\0\{a45c254e...},2 = name   (AudioEndpointBuilder reads this
//                                                    as the Sound Settings display name)
//  <interface key>\MSEP\0\{1da5d803...},7 = 1       (supports event-mode streaming)
static NTSTATUS
SetEndpointName(
    _In_ PCWSTR TopoRefString,   // e.g. L"TOPO0"
    _In_ PCWSTR Name
)
{
    PAGED_CODE();

    PWSTR  interfaces = nullptr;
    NTSTATUS status;

    // Enumerate AUDIO category interfaces for our device (PDO-filtered).
    // AudioEndpointBuilder reads MSEP from the AUDIO category interface key, not
    // the TOPOLOGY category key — this matches the SAR IoGetDeviceInterfaceAlias approach.
    GUID audioCategory = KSCATEGORY_AUDIO;
    status = IoGetDeviceInterfaces(&audioCategory, g_Pdo, 0, &interfaces);
    if (!NT_SUCCESS(status)) return status;

    status = STATUS_NOT_FOUND;

    for (PWSTR p = interfaces; *p; p += wcslen(p) + 1) {
        // Match by trailing reference string suffix: "...#TOPO0"
        SIZE_T len   = wcslen(p);
        SIZE_T tlen  = wcslen(TopoRefString);
        if (len <= tlen || p[len - tlen - 1] != L'#') continue;
        if (_wcsnicmp(p + len - tlen, TopoRefString, tlen) != 0) continue;

        UNICODE_STRING symLink;
        RtlInitUnicodeString(&symLink, p);

        HANDLE ifKey = nullptr;
        status = IoOpenDeviceInterfaceRegistryKey(&symLink, KEY_ALL_ACCESS, &ifKey);
        if (!NT_SUCCESS(status)) break;

        ULONG nameBytes = (ULONG)((wcslen(Name) + 1) * sizeof(WCHAR));

        // FriendlyName on the interface key itself
        UNICODE_STRING val;
        RtlInitUnicodeString(&val, L"FriendlyName");
        ZwSetValueKey(ifKey, &val, 0, REG_SZ, (PVOID)Name, nameBytes);

        // CLSID (KS proxy — required for audio to work)
        RtlInitUnicodeString(&val, L"CLSID");
        ZwSetValueKey(ifKey, &val, 0, REG_SZ, (PVOID)g_ProxyClsid, sizeof(g_ProxyClsid));

        // Create MSEP\0 subkey — AudioEndpointBuilder reads it when processing
        // the interface-enabled notification and promotes values into the
        // MMDEVAPI endpoint property store.
        HANDLE msepKey = nullptr, zeroKey = nullptr;
        UNICODE_STRING msepName, zeroName;
        OBJECT_ATTRIBUTES oa;

        RtlInitUnicodeString(&msepName, L"MSEP");
        InitializeObjectAttributes(&oa, &msepName, OBJ_KERNEL_HANDLE, ifKey, nullptr);
        if (NT_SUCCESS(ZwCreateKey(&msepKey, KEY_ALL_ACCESS, &oa, 0, nullptr, 0, nullptr))) {

            RtlInitUnicodeString(&zeroName, L"0");
            InitializeObjectAttributes(&oa, &zeroName, OBJ_KERNEL_HANDLE, msepKey, nullptr);
            if (NT_SUCCESS(ZwCreateKey(&zeroKey, KEY_ALL_ACCESS, &oa, 0, nullptr, 0, nullptr))) {

                // {a45c254e-df1c-4efd-8020-67d146a850e0},2 = PKEY_Device_DeviceDesc
                // AudioEndpointBuilder uses this as the Sound Settings display name
                // (overrides the generic "Speakers" derived from KSNODETYPE)
                RtlInitUnicodeString(&val, L"{a45c254e-df1c-4efd-8020-67d146a850e0},2");
                ZwSetValueKey(zeroKey, &val, 0, REG_SZ, (PVOID)Name, nameBytes);

                // {1da5d803-d492-4edd-8c23-e0c0ffee7f0e},7 = supports event-mode streaming
                DWORD one = 1;
                RtlInitUnicodeString(&val, L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},7");
                ZwSetValueKey(zeroKey, &val, 0, REG_DWORD, &one, sizeof(DWORD));

                // {1da5d803-d492-4edd-8c23-e0c0ffee7f0e},2 = association (empty GUID)
                static const WCHAR emptyGuid[] = L"{00000000-0000-0000-0000-000000000000}";
                RtlInitUnicodeString(&val, L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},2");
                ZwSetValueKey(zeroKey, &val, 0, REG_SZ, (PVOID)emptyGuid, sizeof(emptyGuid));

                ZwClose(zeroKey);
            }
            ZwClose(msepKey);
        }

        ZwClose(ifKey);
        status = STATUS_SUCCESS;
        break;
    }

    ExFreePool(interfaces);
    return status;
}

static NTSTATUS
StartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp,
    _In_ PRESOURCELIST  ResourceList
)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourceList);

    NTSTATUS status = STATUS_SUCCESS;

    for (ULONG i = 0; i < CHANNEL_COUNT; i++) {
        PPORTTOPOLOGY topoPort = nullptr;
        PPORTWAVERT   wavePort = nullptr;
        PMINIPORT     topoMini = nullptr;
        PMINIPORT     waveMini = nullptr;

        status = PcNewPort(reinterpret_cast<PPORT*>(&wavePort), CLSID_PortWaveRT);
        if (NT_SUCCESS(status))
            status = CreateMiniportWaveRT(
                reinterpret_cast<PUNKNOWN*>(&waveMini),
                CLSID_Miniport, nullptr, POOL_FLAG_NON_PAGED);
        if (NT_SUCCESS(status))
            status = wavePort->Init(DeviceObject, Irp,
                                    reinterpret_cast<PUNKNOWN>(waveMini),
                                    nullptr, nullptr);
        if (NT_SUCCESS(status))
            status = PcRegisterSubdevice(DeviceObject, g_WaveNames[i], wavePort);

        if (NT_SUCCESS(status))
            status = PcNewPort(reinterpret_cast<PPORT*>(&topoPort), CLSID_PortTopology);
        if (NT_SUCCESS(status))
            status = CreateMiniportTopology(
                reinterpret_cast<PUNKNOWN*>(&topoMini),
                CLSID_Miniport, nullptr, POOL_FLAG_NON_PAGED);
        if (NT_SUCCESS(status))
            status = topoPort->Init(DeviceObject, Irp,
                                    reinterpret_cast<PUNKNOWN>(topoMini),
                                    nullptr, nullptr);
        if (NT_SUCCESS(status))
            status = PcRegisterSubdevice(DeviceObject, g_TopoNames[i], topoPort);

        // wave pin 1 (bridge-out) -> topo pin 0 (bridge-in)
        if (NT_SUCCESS(status))
            status = PcRegisterPhysicalConnection(
                DeviceObject,
                reinterpret_cast<PUNKNOWN>(wavePort), 1,
                reinterpret_cast<PUNKNOWN>(topoPort), 0);

        // Write channel name directly to device interface registry from kernel mode.
        // AudioEndpointBuilder reads MSEP\0 asynchronously after interface-enabled
        // notification — writing here (after PcRegisterSubdevice) is safe because
        // AudioEndpointBuilder processes the notification via a deferred work item.
        if (NT_SUCCESS(status))
            SetEndpointName(g_TopoNames[i], g_ChannelNames[i]);

        if (topoMini) topoMini->Release();
        if (waveMini) waveMini->Release();
        if (topoPort) topoPort->Release();
        if (wavePort) wavePort->Release();

        if (!NT_SUCCESS(status)) break;
    }

    return status;
}

#pragma code_seg()

static NTSTATUS
AddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject
)
{
    PAGED_CODE();
    g_Pdo = PhysicalDeviceObject;
    return PcAddAdapterDevice(
        DriverObject,
        PhysicalDeviceObject,
        PCPFNSTARTDEVICE(StartDevice),
        CHANNEL_COUNT * 2,
        0
    );
}

extern "C" NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    return PcInitializeAdapterDriver(
        DriverObject,
        RegistryPath,
        AddDevice
    );
}
