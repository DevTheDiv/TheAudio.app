#include "utils.h"
#include <fstream>
#include <sstream>
#include <tlhelp32.h>

static HANDLE g_singleInstanceMutex = nullptr;

bool AcquireSingleInstance() {
    g_singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Global\\TheAudioApp_SingleInstance");
    if (!g_singleInstanceMutex) return false;
    return GetLastError() != ERROR_ALREADY_EXISTS;
}

void SetRealtimePriority() {
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

std::string ReadFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    DWORD size = GetFileSize(h, nullptr);
    std::string buf(size, '\0');
    DWORD read = 0;
    ReadFile(h, buf.data(), size, &read, nullptr);
    CloseHandle(h);
    buf.resize(read);
    return buf;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& content) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr);
    CloseHandle(h);
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

DWORD GetParentPid(DWORD pid) {
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    DWORD parent = 0;
    if (Process32FirstW(h, &pe)) {
        do { if (pe.th32ProcessID == pid) { parent = pe.th32ParentProcessID; break; } }
        while (Process32NextW(h, &pe));
    }
    CloseHandle(h);
    return parent;
}

std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    auto pos = path.rfind(L'\\');
    return pos != std::wstring::npos ? path.substr(0, pos) : path;
}
