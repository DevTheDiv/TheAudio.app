#pragma once
#include <portcls.h>
#include <stdunk.h>
#include "driver.h"

class CMiniportTopology : public IMiniportTopology, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportTopology);
    ~CMiniportTopology();

    IMP_IMiniportTopology;

private:
    PPORTTOPOLOGY        m_Port;
    PPCFILTER_DESCRIPTOR m_FilterDesc;
};

NTSTATUS CreateMiniportTopology(
    _Out_ PUNKNOWN* Unknown,
    _In_  REFCLSID,
    _In_  PUNKNOWN UnknownOuter,
    _In_  POOL_FLAGS PoolFlags
);
