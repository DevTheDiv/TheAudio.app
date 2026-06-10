#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <string>
#include <map>
#include <queue>
#include <mutex>
#include <chrono>
#include <thread>
#include <iostream>
#include "audio.h"
#include "routing.h"
#include "utils.h"
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

// ── Settings ──────────────────────────────────────────────────────────────────

static AppSettings LoadSettings(const std::wstring& path) {
    AppSettings s;
    std::string json = ReadFile(path);
    if (json.empty()) return s;

    auto extract = [&](const std::string& key) {
        auto t = "\"" + key + "\"";
        auto p = json.find(t);
        if (p == std::string::npos) return std::string();
        auto c = json.find(':', p + t.size());
        while (c + 1 < json.size() && (json[c+1]==' '||json[c+1]=='\t'||json[c+1]=='\r'||json[c+1]=='\n')) c++;
        if (c + 1 < json.size() && json[c+1] == '{') {
            int d = 0;
            for (size_t i = c + 1; i < json.size(); i++) {
                if (json[i] == '{') d++;
                else if (json[i] == '}' && --d == 0) return json.substr(c + 1, i - c);
            }
        } else if (c + 1 < json.size() && json[c+1] == '"') {
            auto vs = c + 2, ve = json.find('"', vs);
            if (ve != std::string::npos) return json.substr(vs, ve - vs);
        }
        return std::string();
    };

    auto parseMapSS = [&](const std::string& obj) {
        std::map<std::wstring, std::wstring> res;
        for (size_t pos = 0; pos < obj.size();) {
            auto ks = obj.find('"', pos); if (ks == std::string::npos) break;
            auto ke = obj.find('"', ks+1); if (ke == std::string::npos) break;
            auto co = obj.find(':', ke+1); if (co == std::string::npos) break;
            auto vs = obj.find('"', co+1); if (vs == std::string::npos) break;
            auto ve = obj.find('"', vs+1); if (ve == std::string::npos) break;
            res[Utf8ToWstr(obj.substr(ks+1, ke-ks-1))] = Utf8ToWstr(obj.substr(vs+1, ve-vs-1));
            pos = ve + 1;
        }
        return res;
    };

    std::string robj = extract("routing"); if (!robj.empty()) s.routing = parseMapSS(robj);
    s.defaultDevice = Utf8ToWstr(extract("defaultDevice"));
    return s;
}

// ── Stdin command queue ───────────────────────────────────────────────────────

struct Cmd {
    std::string  type;
    std::wstring name;
    float        value    = 0.0f;
    bool         bval     = false;
    std::wstring deviceId;
};

static std::queue<Cmd> g_cmdQueue;
static std::mutex      g_cmdMutex;

static std::string ExtractStr(const std::string& s, const std::string& key) {
    auto k = "\"" + key + "\":\"";
    auto p = s.find(k);
    if (p == std::string::npos) return {};
    auto vs = p + k.size(), ve = s.find('"', vs);
    return ve != std::string::npos ? s.substr(vs, ve - vs) : std::string();
}

static float ExtractFloat(const std::string& s, const std::string& key) {
    auto k = "\"" + key + "\":";
    auto p = s.find(k);
    if (p == std::string::npos) return 0.0f;
    size_t ns = p + k.size();
    while (ns < s.size() && (s[ns]==' '||s[ns]=='\t')) ns++;
    size_t ne = ns;
    while (ne < s.size() && (std::isdigit((unsigned char)s[ne])||s[ne]=='.'||s[ne]=='-')) ne++;
    if (ne > ns) try { return std::stof(s.substr(ns, ne - ns)); } catch(...) {}
    return 0.0f;
}

static bool ExtractBool(const std::string& s, const std::string& key) {
    auto k = "\"" + key + "\":";
    auto p = s.find(k);
    if (p == std::string::npos) return false;
    size_t vs = p + k.size();
    while (vs < s.size() && (s[vs]==' '||s[vs]=='\t')) vs++;
    return s.substr(vs, 4) == "true";
}

static void StartStdinReader() {
    std::thread([]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;
            Cmd c;
            c.type     = ExtractStr(line, "cmd");
            if (c.type.empty()) continue;
            c.name     = Utf8ToWstr(ExtractStr(line, "name"));
            c.deviceId = Utf8ToWstr(ExtractStr(line, "deviceId"));
            c.value    = ExtractFloat(line, "value");
            c.bval     = ExtractBool(line, "muted");
            { std::lock_guard<std::mutex> lk(g_cmdMutex); g_cmdQueue.push(c); }
            SetEvent(g_refreshEvent);
        }
    }).detach();
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!AcquireSingleInstance()) return 1;
    timeBeginPeriod(1);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* de = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator), (void**)&de);
    if (!de) { CoUninitialize(); return 1; }

    std::wstring sPath = GetExeDir() + L"\\settings.json";
    AppSettings settings = LoadSettings(sPath);

    StartSilentStream(de);
    StartStdinReader();

    DeviceNotificationClient* dn = new DeviceNotificationClient();
    de->RegisterEndpointNotificationCallback(dn);
    RegisterSessionNotifications(de);

    EmitStatus();
    auto endpoints = EnumerateEndpoints(de);
    EmitEndpoints(endpoints);

    auto lastEPR  = std::chrono::steady_clock::now();
    auto lastSync = std::chrono::steady_clock::now();
    FILETIME lastMT{};

    std::thread([&]() {
        while (true) {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(sPath.c_str(), GetFileExInfoStandard, &fa)) {
                if (CompareFileTime(&fa.ftLastWriteTime, &lastMT) != 0) SetEvent(g_refreshEvent);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }).detach();

    while (true) {
        WaitForSingleObject(g_refreshEvent, 41);
        auto now = std::chrono::steady_clock::now();

        // Reload settings if file changed
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        GetFileAttributesExW(sPath.c_str(), GetFileExInfoStandard, &fa);
        if (CompareFileTime(&fa.ftLastWriteTime, &lastMT) != 0) {
            lastMT = fa.ftLastWriteTime;
            settings = LoadSettings(sPath);
        }

        // Re-enumerate endpoints and sessions when devices change
        if (g_needsRefresh) {
            endpoints = EnumerateEndpoints(de);
            EmitEndpoints(endpoints);
            RegisterSessionNotifications(de);
            RefreshSessionCache(de, settings, endpoints);
            g_needsRefresh = false;
        }

        if (now - lastEPR > std::chrono::seconds(15)) {
            endpoints = EnumerateEndpoints(de);
            EmitEndpoints(endpoints);
            lastEPR = now;
        }

        if (now - lastSync > std::chrono::seconds(5)) {
            SyncEndpointsFromWindows();
            lastSync = now;
        }

        // Drain stdin commands
        std::queue<Cmd> cmds;
        { std::lock_guard<std::mutex> lk(g_cmdMutex); cmds.swap(g_cmdQueue); }
        while (!cmds.empty()) {
            auto& c = cmds.front();
            if      (c.type == "volume")       SetSessionVolume(c.name, c.value);
            else if (c.type == "mute")         SetSessionMute(c.name, c.bval);
            else if (c.type == "systemVolume") SetSystemVolume(de, c.value);
            else if (c.type == "systemMute")   SetSystemMute(de, c.bval);
            else if (c.type == "deviceVolume") SetDeviceVolume(c.deviceId, c.value);
            else if (c.type == "deviceMute")   SetDeviceMute(c.deviceId, c.bval);
            cmds.pop();
        }

        UpdateSessionsAndEmit(settings, endpoints);
        UpdateEndpointLevelsAndEmit();
        UpdateSystemAndEmit(de);
    }

    CoUninitialize();
    return 0;
}
