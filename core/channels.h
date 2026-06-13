#pragma once

// Virtual channel definitions — must match the friendly names in VirtualAudioDriver.inf exactly.
// Both the C++ core and the bridge use these constants so a name change only needs to happen here.

constexpr const wchar_t* VIRTUAL_CHANNEL_PREFIX = L"TheAudio.app ";

struct VirtualChannel {
    const wchar_t* name;  // full MMDEVAPI friendly name, e.g. L"TheAudio.app Games"
    const wchar_t* key;   // short JSON key used in settings, e.g. L"Games"
};

inline constexpr VirtualChannel VIRTUAL_CHANNELS[] = {
    { L"TheAudio.app Games (TheAudio.app Virtual Audio)", L"Games" },
    { L"TheAudio.app Media (TheAudio.app Virtual Audio)", L"Media" },
    { L"TheAudio.app Voice (TheAudio.app Virtual Audio)", L"Voice" },
};

inline constexpr int VIRTUAL_CHANNEL_COUNT = 3;

inline bool IsVirtualEndpoint(const wchar_t* name) {
    if (!name) return false;
    const wchar_t* p = VIRTUAL_CHANNEL_PREFIX;
    while (*p && *name == *p) { ++p; ++name; }
    return *p == L'\0';
}
