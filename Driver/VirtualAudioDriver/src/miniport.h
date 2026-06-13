#pragma once

#include <portcls.h>
#include <stdunk.h>
#include "driver.h"

class CMiniportWaveRT : public IMiniportWaveRT, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWaveRT);
    ~CMiniportWaveRT();

    // IMiniport
    IMP_IMiniportWaveRT;

private:
    PPORTWAVERT     m_Port;
    PPCFILTER_DESCRIPTOR m_FilterDesc;
};
