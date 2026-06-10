#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include "types.h"

void SetDefaultDevice(const std::wstring& deviceId);
void SetProcessEndpoint(DWORD pid, const std::wstring& deviceId, const std::wstring& exeName);
std::wstring GetImmersiveEndpoint(DWORD pid);
void ApplyRouting(const std::vector<SessionInfo>& sessions,
                  const AppSettings&               settings,
                  const std::vector<EndpointInfo>& endpoints);
