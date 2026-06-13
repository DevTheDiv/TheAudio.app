#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

inline std::wstring Utf8ToWstr(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

inline std::string WstrToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

inline std::string JsonStr(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else                out += c;
    }
    return out + "\"";
}

// Returns false if another instance is already running.
bool AcquireSingleInstance();

// Returns the parent PID of the given process, or 0 on failure.
DWORD GetParentPid(DWORD pid);

// Sets the current thread and process to high/realtime priority.
void SetRealtimePriority();

// Reads the entire contents of a UTF-8 file. Returns empty string on failure.
std::string ReadFile(const std::wstring& path);

// Writes content to a UTF-8 file atomically (write to temp, rename).
bool WriteFileAtomic(const std::wstring& path, const std::string& content);

// Returns the directory containing the running executable.
std::wstring GetExeDir();
