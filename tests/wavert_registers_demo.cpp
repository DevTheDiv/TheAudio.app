/**
 * wavert_registers_demo.cpp
 * 
 * CONCEPTUAL IMPLEMENTATION ONLY. 
 * This file demonstrates how to implement GetPositionRegister and GetClockRegister 
 * to fix runaway clock drift (e.g., the 2.11x speed issue) in a WaveRT driver.
 * 
 * Background:
 * When a WaveRT driver does not provide a hardware register, the Windows Audio Engine
 * polls GetPosition() and tries to estimate the clock. If the software response is 
 * jittery, the Engine's clock-follower can "run away," pushing data at 2-3x speed.
 * 
 * Solution: 
 * Provide a memory-mapped shared register that the Engine reads directly without 
 * kernel transitions.
 */

#include <ntddk.h>
#include <portcls.h>
#include <ksmedia.h>

// --- Conceptual Stream Class ---

class CMyWaveRTStream {
public:
    // Shared memory for the Position Register
    PKSRTAUDIO_HWREGISTER m_pPositionRegister;
    PHYSICAL_ADDRESS      m_PositionRegisterPhysicalAddr;

    // Shared memory for the Clock Register
    PKSRTAUDIO_HWREGISTER m_pClockRegister;
    PHYSICAL_ADDRESS      m_ClockRegisterPhysicalAddr;

    // Buffer state
    ULONG m_ulBufferSize;
    ULONG m_ulSampleRate;
    ULONG m_ulBytesPerFrame;
    LONGLONG m_ullStartTimeHns;

    // High-resolution timer
    PEX_TIMER m_NotificationTimer;

    /**
     * 1. ALLOCATION
     * Called during NewStream or Init.
     */
    NTSTATUS AllocateRegisters() {
        // Allocate 1 page of non-paged memory for registers.
        // In a real driver, this must be available to the Audio Engine.
        m_pPositionRegister = (PKSRTAUDIO_HWREGISTER)ExAllocatePool2(
            POOL_FLAG_NON_PAGED, 
            PAGE_SIZE, 
            'geRP' // 'PReg'
        );

        if (!m_pPositionRegister) return STATUS_INSUFFICIENT_RESOURCES;

        // Map second register in same page or separate allocation
        m_pClockRegister = (PKSRTAUDIO_HWREGISTER)((PUCHAR)m_pPositionRegister + sizeof(ULONG));

        // Get physical addresses (required for PortCls to map to User Mode)
        m_PositionRegisterPhysicalAddr = MmGetPhysicalAddress(m_pPositionRegister);
        m_ClockRegisterPhysicalAddr    = MmGetPhysicalAddress(m_pClockRegister);

        return STATUS_SUCCESS;
    }

    /**
     * 2. THE HANDSHAKE (Position)
     * PortCls calls this to get the location of the play head.
     */
    STDMETHODIMP GetPositionRegister(_Out_ PKSRTAUDIO_HWREGISTER Register) {
        // Return the mapped virtual address. PortCls will handle the 
        // user-mode mapping for the Audio Engine.
        Register->Register    = m_pPositionRegister;
        Register->Width       = 32; // 32-bit register
        Register->Numerator   = 1;
        Register->Denominator = 1;

        // Once this returns SUCCESS, the Audio Engine stops calling GetPosition()
        // and starts reading the memory address directly.
        return STATUS_SUCCESS;
    }

    /**
     * 3. THE HANDSHAKE (Clock)
     * Tells the Engine the absolute "wall clock" of the hardware.
     */
    STDMETHODIMP GetClockRegister(_Out_ PKSRTAUDIO_HWREGISTER Register) {
        Register->Register    = m_pClockRegister;
        Register->Width       = 64; // 64-bit counter
        Register->Numerator   = 1;
        Register->Denominator = 1; // Units = 100ns (HNS)

        return STATUS_SUCCESS;
    }

    /**
     * 4. THE INTERRUPT (The Master Clock)
     * Fired by EX_TIMER_HIGH_RESOLUTION every 10ms.
     */
    void OnTimerInterrupt() {
        LONGLONG now = KeQueryInterruptTime();
        LONGLONG elapsed = now - m_ullStartTimeHns;

        // Calculate precise byte offset
        LONGLONG totalBytes = (elapsed * (LONGLONG)m_ulSampleRate * (LONGLONG)m_ulBytesPerFrame) / 10000000;
        ULONG playOffset = (ULONG)(totalBytes % (LONGLONG)m_ulBufferSize);

        // UPDATE THE REGISTERS:
        // Because the Audio Engine has these addresses mapped, it sees 
        // these updates instantly. There is no "estimation" or "guessing" 
        // by the OS clock follower.
        *(ULONG*)(m_pPositionRegister) = playOffset;
        *(LONGLONG*)(m_pClockRegister) = now;

        // Notify engine to process data
        SignalNotificationEvents();
    }
};
