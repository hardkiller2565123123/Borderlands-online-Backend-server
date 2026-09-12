#include <windows.h>
#include <versionhelpers.h>
#include <string>

#include "EmuBootstrap.h"
#include "Logger.h"

namespace
{
    HMODULE g_realVersion = nullptr;

    template <typename T>
    T Resolve(const char* name)
    {
        if (!g_realVersion)
        {
            wchar_t systemDirectory[MAX_PATH]{};
            const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
            if (!length || length >= MAX_PATH)
                return nullptr;

            const std::wstring path = std::wstring(systemDirectory) + L"\\version.dll";
            g_realVersion = LoadLibraryW(path.c_str());
        }

        if (!g_realVersion)
            return nullptr;

        return reinterpret_cast<T>(GetProcAddress(g_realVersion, name));
    }
}

extern "C" __declspec(dllexport) DWORD WINAPI Proxy_GetFileVersionInfoSizeA(LPCSTR fileName, LPDWORD handle)
{
    using Fn = DWORD (WINAPI*)(LPCSTR, LPDWORD);
    static Fn fn = Resolve<Fn>("GetFileVersionInfoSizeA");
    return fn ? fn(fileName, handle) : 0;
}

extern "C" __declspec(dllexport) DWORD WINAPI Proxy_GetFileVersionInfoSizeW(LPCWSTR fileName, LPDWORD handle)
{
    using Fn = DWORD (WINAPI*)(LPCWSTR, LPDWORD);
    static Fn fn = Resolve<Fn>("GetFileVersionInfoSizeW");
    return fn ? fn(fileName, handle) : 0;
}

extern "C" __declspec(dllexport) BOOL WINAPI Proxy_GetFileVersionInfoA(LPCSTR fileName, DWORD handle, DWORD length, LPVOID data)
{
    using Fn = BOOL (WINAPI*)(LPCSTR, DWORD, DWORD, LPVOID);
    static Fn fn = Resolve<Fn>("GetFileVersionInfoA");
    return fn ? fn(fileName, handle, length, data) : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI Proxy_GetFileVersionInfoW(LPCWSTR fileName, DWORD handle, DWORD length, LPVOID data)
{
    using Fn = BOOL (WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    static Fn fn = Resolve<Fn>("GetFileVersionInfoW");
    return fn ? fn(fileName, handle, length, data) : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI Proxy_VerQueryValueA(LPCVOID block, LPCSTR subBlock, LPVOID* buffer, PUINT length)
{
    using Fn = BOOL (WINAPI*)(LPCVOID, LPCSTR, LPVOID*, PUINT);
    static Fn fn = Resolve<Fn>("VerQueryValueA");
    return fn ? fn(block, subBlock, buffer, length) : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI Proxy_VerQueryValueW(LPCVOID block, LPCWSTR subBlock, LPVOID* buffer, PUINT length)
{
    using Fn = BOOL (WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    static Fn fn = Resolve<Fn>("VerQueryValueW");
    return fn ? fn(block, subBlock, buffer, length) : FALSE;
}

// BOL.exe is x86 and imports these undecorated VERSION.dll names.
// Keep the project .def-free by exporting aliases with linker directives.
#ifdef _M_IX86
#pragma comment(linker, "/export:GetFileVersionInfoSizeA=_Proxy_GetFileVersionInfoSizeA@8")
#pragma comment(linker, "/export:GetFileVersionInfoSizeW=_Proxy_GetFileVersionInfoSizeW@8")
#pragma comment(linker, "/export:GetFileVersionInfoA=_Proxy_GetFileVersionInfoA@16")
#pragma comment(linker, "/export:GetFileVersionInfoW=_Proxy_GetFileVersionInfoW@16")
#pragma comment(linker, "/export:VerQueryValueA=_Proxy_VerQueryValueA@16")
#pragma comment(linker, "/export:VerQueryValueW=_Proxy_VerQueryValueW@16")
#else
#pragma comment(linker, "/export:GetFileVersionInfoSizeA=Proxy_GetFileVersionInfoSizeA")
#pragma comment(linker, "/export:GetFileVersionInfoSizeW=Proxy_GetFileVersionInfoSizeW")
#pragma comment(linker, "/export:GetFileVersionInfoA=Proxy_GetFileVersionInfoA")
#pragma comment(linker, "/export:GetFileVersionInfoW=Proxy_GetFileVersionInfoW")
#pragma comment(linker, "/export:VerQueryValueA=Proxy_VerQueryValueA")
#pragma comment(linker, "/export:VerQueryValueW=Proxy_VerQueryValueW")
#endif

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, emu::BootstrapThread, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        bol_log::Shutdown();
        if (g_realVersion)
        {
            FreeLibrary(g_realVersion);
            g_realVersion = nullptr;
        }
    }

    return TRUE;
}
