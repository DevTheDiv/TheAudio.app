#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <string>
#include <map>

// Start one forwarding thread per virtual channel.
// channelOutputs maps short key ("Games") to a physical MMDEVAPI device ID.
// Passing an empty device ID for a channel leaves that channel un-forwarded.
void StartBridges(IMMDeviceEnumerator* de,
                  const std::map<std::wstring, std::wstring>& channelOutputs);

// Swap the physical output for a running bridge at runtime.
// key is the short channel key ("Games", "Media", "Voice").
void SetBridgeOutput(const std::wstring& key, const std::wstring& deviceId);

void StopBridges();
