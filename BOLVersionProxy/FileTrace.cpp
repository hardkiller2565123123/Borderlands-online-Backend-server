#include "FileTrace.h"

extern "C" __declspec(selectany) const char g_BolRevivalV63ProxyTraceMarker[] = "BOLREVIVAL_V63_PROXY_TRACE_MARKER";
#include "Logger.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <string>

namespace
{
    using CreateFileWFn = HANDLE (WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using CreateFileAFn = HANDLE (WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

    CreateFileWFn g_realCreateFileW = nullptr;
    CreateFileAFn g_realCreateFileA = nullptr;
    std::atomic<bool> g_started{ false };
    thread_local bool g_insideHook = false;

    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            if (ch >= L'A' && ch <= L'Z')
                return static_cast<wchar_t>(ch - L'A' + L'a');
            return ch;
        });
        return value;
    }

    std::string ToLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    std::string WideToUtf8(const wchar_t* value)
    {
        if (!value || !*value)
            return std::string();

        const int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
        if (count <= 1)
            return std::string();

        std::string result(static_cast<size_t>(count), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), count, nullptr, nullptr);
        if (!result.empty() && result.back() == '\0')
            result.pop_back();
        return result;
    }

    bool ShouldTrace(const std::wstring& raw)
    {
        const std::wstring path = ToLower(raw);
        static const wchar_t* kNeedles[] = {
            L"assetbundle", L"resources.assets", L"sharedassets",
            L"maindata", L"globalgamemanagers", L"resourceloading",
            L"lvl_floasm", L"\\level0", L"\\level1", L"\\level2",
            L"\\level3", L"\\level4", L"\\level5"
        };
        for (const wchar_t* needle : kNeedles)
        {
            if (path.find(needle) != std::wstring::npos)
                return true;
        }
        return false;
    }

    bool ShouldTrace(const std::string& raw)
    {
        const std::string path = ToLower(raw);
        static const char* kNeedles[] = {
            "assetbundle", "resources.assets", "sharedassets",
            "maindata", "globalgamemanagers", "resourceloading",
            "lvl_floasm", "\\level0", "\\level1", "\\level2",
            "\\level3", "\\level4", "\\level5"
        };
        for (const char* needle : kNeedles)
        {
            if (path.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }

    HANDLE WINAPI HookCreateFileW(
        LPCWSTR fileName,
        DWORD desiredAccess,
        DWORD shareMode,
        LPSECURITY_ATTRIBUTES securityAttributes,
        DWORD creationDisposition,
        DWORD flagsAndAttributes,
        HANDLE templateFile)
    {
        if (!g_realCreateFileW)
            return INVALID_HANDLE_VALUE;

        const bool trace = fileName && ShouldTrace(std::wstring(fileName));
        const bool outer = !g_insideHook;
        if (outer)
            g_insideHook = true;

        SetLastError(ERROR_SUCCESS);
        HANDLE result = g_realCreateFileW(fileName, desiredAccess, shareMode, securityAttributes,
                                          creationDisposition, flagsAndAttributes, templateFile);
        const DWORD error = GetLastError();

        if (trace && outer)
        {
            const std::string utf8 = WideToUtf8(fileName);
            bol_log::Write("[LOADTRACE] CreateFileW path=%s access=0x%08lX disposition=%lu result=%p error=%lu",
                           utf8.c_str(), desiredAccess, creationDisposition, result, error);
        }

        if (outer)
            g_insideHook = false;
        SetLastError(error);
        return result;
    }

    HANDLE WINAPI HookCreateFileA(
        LPCSTR fileName,
        DWORD desiredAccess,
        DWORD shareMode,
        LPSECURITY_ATTRIBUTES securityAttributes,
        DWORD creationDisposition,
        DWORD flagsAndAttributes,
        HANDLE templateFile)
    {
        if (!g_realCreateFileA)
            return INVALID_HANDLE_VALUE;

        const bool trace = fileName && ShouldTrace(std::string(fileName));
        const bool outer = !g_insideHook;
        if (outer)
            g_insideHook = true;

        SetLastError(ERROR_SUCCESS);
        HANDLE result = g_realCreateFileA(fileName, desiredAccess, shareMode, securityAttributes,
                                          creationDisposition, flagsAndAttributes, templateFile);
        const DWORD error = GetLastError();

        if (trace && outer)
        {
            bol_log::Write("[LOADTRACE] CreateFileA path=%s access=0x%08lX disposition=%lu result=%p error=%lu",
                           fileName, desiredAccess, creationDisposition, result, error);
        }

        if (outer)
            g_insideHook = false;
        SetLastError(error);
        return result;
    }

    bool IsGameOwnedModule(const wchar_t* modulePath)
    {
        if (!modulePath || !*modulePath)
            return false;

        wchar_t exePath[MAX_PATH]{};
        const DWORD exeLen = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (!exeLen || exeLen >= MAX_PATH)
            return false;

        std::wstring root(exePath, exeLen);
        const auto slash = root.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            root.resize(slash + 1);
        root = ToLower(root);

        const std::wstring candidate = ToLower(std::wstring(modulePath));
        if (candidate.rfind(root, 0) != 0)
            return false;

        const auto basePos = candidate.find_last_of(L"\\/");
        const std::wstring base = (basePos == std::wstring::npos) ? candidate : candidate.substr(basePos + 1);
        return base != L"version.dll";
    }

    unsigned int PatchModuleIat(HMODULE module, const wchar_t* modulePath)
    {
        if (!module)
            return 0;

        auto* base = reinterpret_cast<unsigned char*>(module);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return 0;

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return 0;

        const IMAGE_DATA_DIRECTORY& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!importDir.VirtualAddress || !importDir.Size)
            return 0;

        auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
        unsigned int patched = 0;

        for (; descriptor->Name; ++descriptor)
        {
            auto* firstThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
            IMAGE_THUNK_DATA* originalThunk = descriptor->OriginalFirstThunk
                ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk)
                : nullptr;

            for (size_t index = 0; firstThunk[index].u1.Function; ++index)
            {
                const char* importName = nullptr;
                if (originalThunk && !IMAGE_SNAP_BY_ORDINAL(originalThunk[index].u1.Ordinal))
                {
                    auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + originalThunk[index].u1.AddressOfData);
                    importName = reinterpret_cast<const char*>(importByName->Name);
                }

                void* replacement = nullptr;
                if (importName)
                {
                    if (std::strcmp(importName, "CreateFileW") == 0)
                        replacement = reinterpret_cast<void*>(&HookCreateFileW);
                    else if (std::strcmp(importName, "CreateFileA") == 0)
                        replacement = reinterpret_cast<void*>(&HookCreateFileA);
                }
                else
                {
                    const auto current = reinterpret_cast<void*>(static_cast<uintptr_t>(firstThunk[index].u1.Function));
                    if (current == reinterpret_cast<void*>(g_realCreateFileW))
                        replacement = reinterpret_cast<void*>(&HookCreateFileW);
                    else if (current == reinterpret_cast<void*>(g_realCreateFileA))
                        replacement = reinterpret_cast<void*>(&HookCreateFileA);
                }

                if (!replacement)
                    continue;

                auto** slot = reinterpret_cast<void**>(&firstThunk[index].u1.Function);
                if (*slot == replacement)
                    continue;

                DWORD oldProtect = 0;
                if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
                    continue;

                *slot = replacement;
                FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
                DWORD ignored = 0;
                VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
                ++patched;
            }
        }

        if (patched)
        {
            const std::string utf8 = WideToUtf8(modulePath);
            bol_log::Write("[LOADTRACE] hooked %u CreateFile import(s) in %s", patched, utf8.c_str());
        }
        return patched;
    }

    unsigned int PatchGameModules()
    {
        const DWORD pid = GetCurrentProcessId();
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot == INVALID_HANDLE_VALUE)
            return 0;

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        unsigned int patched = 0;
        if (Module32FirstW(snapshot, &entry))
        {
            do
            {
                if (IsGameOwnedModule(entry.szExePath))
                    patched += PatchModuleIat(entry.hModule, entry.szExePath);
            } while (Module32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return patched;
    }

    DWORD WINAPI WatchThread(void*)
    {
        unsigned int totalPatched = 0;
        for (;;)
        {
            const unsigned int added = PatchGameModules();
            totalPatched += added;
            if (added && totalPatched > 0)
                bol_log::Write("[LOADTRACE] file-open tracing active; cumulative patched imports=%u", totalPatched);
            Sleep(1000);
        }
    }
}

namespace file_trace
{
    void Start()
    {
        bool expected = false;
        if (!g_started.compare_exchange_strong(expected, true))
            return;

        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        if (!kernel32)
            kernel32 = LoadLibraryW(L"kernel32.dll");
        g_realCreateFileW = kernel32 ? reinterpret_cast<CreateFileWFn>(GetProcAddress(kernel32, "CreateFileW")) : nullptr;
        g_realCreateFileA = kernel32 ? reinterpret_cast<CreateFileAFn>(GetProcAddress(kernel32, "CreateFileA")) : nullptr;

        if (!g_realCreateFileW || !g_realCreateFileA)
        {
            bol_log::Write("[LOADTRACE] could not resolve CreateFileA/W; runtime loading trace disabled");
            return;
        }

        const unsigned int initialPatched = PatchGameModules();
        bol_log::Write("[LOADTRACE] v63 native Unity file-open tracer started; marker=BOLREVIVAL_V63_PROXY_TRACE_MARKER initial patched imports=%u", initialPatched);
        bol_log::Write("[LOADTRACE] watch for assetbundle/mainData/level/resources file opens after SessionInfo PHASE B");

        HANDLE thread = CreateThread(nullptr, 0, &WatchThread, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
        else
            bol_log::Write("[LOADTRACE] module watcher thread creation failed (Win32=%lu)", GetLastError());
    }
}
