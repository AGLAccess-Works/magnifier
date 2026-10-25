// Minimal logging to %APPDATA%\Magnifier\log.txt and the debugger. The log
// records what the magnifier did and how long it took, never the text or
// names it read from the screen.
#pragma once

#include <windows.h>

#include <cstdio>
#include <share.h>
#include <mutex>
#include <string>

#include "settings.h"

// Log file name inside the settings folder; set it before the first Log call.
inline const wchar_t*& LogFileName() {
    static const wchar_t* name = L"log.txt";
    return name;
}

inline void Log(const wchar_t* fmt, ...) {
    static std::mutex mu;
    static FILE* file = [] {
        std::wstring path = SettingsPath();
        path.replace(path.rfind(L'\\') + 1, std::wstring::npos, LogFileName());
        // Shared so the log can be read while the magnifier is running.
        return _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    }();
    wchar_t msg[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1100];
    swprintf_s(line, L"%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    OutputDebugStringW(line);
    // The file starts afresh each run and stops growing at about 2 MB.
    constexpr long kMaxLog = 2 << 20;
    std::lock_guard lock(mu);
    if (file && ftell(file) < kMaxLog) {
        fputws(line, file);
        if (ftell(file) >= kMaxLog) fputws(L"(log full)\n", file);
        fflush(file);
    }
}

// Logs and returns false when hr is a failure.
inline bool Check(HRESULT hr, const wchar_t* what) {
    if (SUCCEEDED(hr)) return true;
    Log(L"%s failed: 0x%08X", what, (unsigned)hr);
    return false;
}
