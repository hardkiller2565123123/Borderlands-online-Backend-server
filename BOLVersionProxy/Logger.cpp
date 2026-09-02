#include "Logger.h"

#include <cstdio>
#include <mutex>
#include <string>

namespace
{
    std::mutex g_logMutex;
    FILE* g_logFile = nullptr;
    bool g_consoleReady = false;

    std::wstring GetGameDirectory()
    {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return L".";

        std::wstring result(path, length);
        const auto slash = result.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            result.resize(slash);
        return result;
    }
}

namespace bol_log
{
    void Initialize()
    {
        std::lock_guard<std::mutex> lock(g_logMutex);

        if (!g_consoleReady)
        {
            if (AllocConsole() || GetLastError() == ERROR_ACCESS_DENIED)
            {
                FILE* ignored = nullptr;
                freopen_s(&ignored, "CONOUT$", "w", stdout);
                freopen_s(&ignored, "CONOUT$", "w", stderr);
                SetConsoleTitleW(L"Borderlands Online Revival");
                g_consoleReady = true;
            }
        }

        if (!g_logFile)
        {
            const std::wstring logPath = GetGameDirectory() + L"\\bol_revive.log";
            _wfopen_s(&g_logFile, logPath.c_str(), L"a+, ccs=UTF-8");
        }
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_logFile)
        {
            fflush(g_logFile);
            fclose(g_logFile);
            g_logFile = nullptr;
        }
    }

    void WriteV(const char* format, va_list args)
    {
        char buffer[2048]{};
        vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);

        SYSTEMTIME now{};
        GetLocalTime(&now);

        std::lock_guard<std::mutex> lock(g_logMutex);
        printf("[%02u:%02u:%02u.%03u] %s\n",
               now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, buffer);
        fflush(stdout);

        if (g_logFile)
        {
            fwprintf(g_logFile, L"[%02u:%02u:%02u.%03u] %S\n",
                     now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, buffer);
            fflush(g_logFile);
        }
    }

    void Write(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        WriteV(format, args);
        va_end(args);
    }
}
