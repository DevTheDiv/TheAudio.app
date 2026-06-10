#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <string>
#include <map>
#include <vector>

struct SessionInfo {
    std::wstring id, name, exePath;
    DWORD        pid       = 0;
    DWORD        parentPid = 0;
    float        volume    = 1.0f;
    bool         muted     = false;
    float        peak      = 0.0f;
    std::wstring endpoint, endpointId;
};

struct EndpointInfo {
    std::wstring id, name;
    std::string  type; // "physical"
};

struct AppSettings {
    std::map<std::wstring, std::wstring> routing;
    std::wstring                         defaultDevice;
};

struct CachedSession {
    IAudioSessionControl2*  ctrl  = nullptr;
    ISimpleAudioVolume*     vol   = nullptr;
    IAudioMeterInformation* meter = nullptr;
    SessionInfo             info;
    bool                    valid = true;
};
