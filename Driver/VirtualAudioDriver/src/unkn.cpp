#include <portcls.h>
#include "driver.h"
CUnknown::CUnknown(PUNKNOWN pUnknownOuter)
    : m_lRefCount(0)
{
    m_pUnknownOuter = pUnknownOuter
        ? pUnknownOuter
        : PUNKNOWN(static_cast<INonDelegatingUnknown *>(this));
}
CUnknown::~CUnknown() {}
STDMETHODIMP_(ULONG)
CUnknown::NonDelegatingAddRef()
{
    return (ULONG)InterlockedIncrement(&m_lRefCount);
}
STDMETHODIMP_(ULONG)
CUnknown::NonDelegatingRelease()
{
    LONG ref = InterlockedDecrement(&m_lRefCount);
    if (ref == 0)
        delete this;
    return (ULONG)ref;
}
STDMETHODIMP_(NTSTATUS)
CUnknown::NonDelegatingQueryInterface(REFIID, PVOID *ppv)
{
    *ppv = nullptr;
    return STATUS_NOINTERFACE;
}

