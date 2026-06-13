#pragma once

#include <portcls.h>
#include <stdunk.h>
#include <ksdebug.h>

#define DRIVER_TAG 'DAVA'  // Virtual Audio Driver

// {B4D5A7C0-1234-4321-ABCD-0123456789AB}
DEFINE_GUID(CLSID_Miniport,
    0xb4d5a7c0, 0x1234, 0x4321, 0xab, 0xcd, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab);

// Number of virtual endpoints we expose
#define NUM_ENDPOINTS 2

NTSTATUS CreateMiniportWaveRT(
    _Out_ PUNKNOWN* Unknown,
    _In_  REFCLSID,
    _In_  PUNKNOWN UnknownOuter,
    _In_  POOL_FLAGS PoolFlags
);
