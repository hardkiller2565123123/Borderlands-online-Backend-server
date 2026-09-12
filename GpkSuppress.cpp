#include "GpkSuppress.h"
#include "Logger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>
#include <cwchar>

namespace
{
    using MessageBoxAFn = int (WINAPI*)(HWND, LPCSTR, LPCSTR, UINT);
    using MessageBoxWFn = int (WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);

    MessageBoxAFn g_realMessageBoxA = nullptr;
    MessageBoxWFn g_realMessageBoxW = nullptr;
    volatile LONG g_loggedSuppressedDialog = 0;
    volatile LONG g_loggedHiddenUpdater = 0;

    bool ContainsInsensitiveA(const char* text, const char* needle)
    {
        if (!text || !needle || !*needle)
            return false;

        const std::size_t needleLength = std::strlen(needle);
        for (const char* p = text; *p; ++p)
        {
            if (_strnicmp(p, needle, needleLength) == 0)
                return true;
        }
        return false;
    }

    bool ContainsInsensitiveW(const wchar_t* text, const wchar_t* needle)
    {
        if (!text || !needle || !*needle)
            return false;

        const std::size_t needleLength = std::wcslen(needle);
        for (const wchar_t* p = text; *p; ++p)
        {
            if (_wcsnicmp(p, needle, needleLength) == 0)
                return true;
        }
        return false;
    }

    bool IsHarmlessGpkFailureA(const char* text, const char* caption)
    {
        return ContainsInsensitiveA(text, "GPK Update Failed") ||
               ContainsInsensitiveA(text, "Just Try To Continue") ||
               (ContainsInsensitiveA(caption, "GPK") && ContainsInsensitiveA(text, "Update Failed"));
    }

    bool IsHarmlessGpkFailureW(const wchar_t* text, const wchar_t* caption)
    {
        return ContainsInsensitiveW(text, L"GPK Update Failed") ||
               ContainsInsensitiveW(text, L"Just Try To Continue") ||
               (ContainsInsensitiveW(caption, L"GPK") && ContainsInsensitiveW(text, L"Update Failed"));
    }

    int WINAPI HookMessageBoxA(HWND wnd, LPCSTR text, LPCSTR caption, UINT type)
    {
        if (IsHarmlessGpkFailureA(text, caption))
        {
            if (InterlockedExchange(&g_loggedSuppressedDialog, 1) == 0)
                bol_log::Write("[GPK] Suppressed harmless legacy GPK update failure dialog");
            return IDOK;
        }
        return g_realMessageBoxA ? g_realMessageBoxA(wnd, text, caption, type) : IDOK;
    }

    int WINAPI HookMessageBoxW(HWND wnd, LPCWSTR text, LPCWSTR caption, UINT type)
    {
        if (IsHarmlessGpkFailureW(text, caption))
        {
            if (InterlockedExchange(&g_loggedSuppressedDialog, 1) == 0)
                bol_log::Write("[GPK] Suppressed harmless legacy GPK update failure dialog");
            return IDOK;
        }
        return g_realMessageBoxW ? g_realMessageBoxW(wnd, text, caption, type) : IDOK;
    }

    template <typename T>
    bool PatchIat(HMODULE module, const char* importName, T hook, T* original)
    {
        if (!module || !importName || !hook)
            return false;

        auto* base = reinterpret_cast<unsigned char*>(module);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;

        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress)
            return false;

        auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
        bool patched = false;
        for (; desc->Name; ++desc)
        {
            const DWORD lookupRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
            if (!lookupRva || !desc->FirstThunk)
                continue;

            auto* lookup = reinterpret_cast<IMAGE_THUNK_DATA*>(base + lookupRva);
            auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
            for (; lookup->u1.AddressOfData; ++lookup, ++iat)
            {
                if (IMAGE_SNAP_BY_ORDINAL(lookup->u1.Ordinal))
                    continue;
                auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + lookup->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(byName->Name), importName) != 0)
                    continue;

                auto* slot = reinterpret_cast<T*>(&iat->u1.Function);
                if (original && !*original)
                    *original = *slot;

                DWORD oldProtect = 0;
                if (!VirtualProtect(slot, sizeof(T), PAGE_READWRITE, &oldProtect))
                    continue;
                *slot = hook;
                FlushInstructionCache(GetCurrentProcess(), slot, sizeof(T));
                DWORD ignored = 0;
                VirtualProtect(slot, sizeof(T), oldProtect, &ignored);
                patched = true;
            }
        }
        return patched;
    }

    struct WindowProbe
    {
        bool hasDownloadingText = false;
        bool hasFailureText = false;
    };

    BOOL CALLBACK ProbeChild(HWND child, LPARAM value)
    {
        auto* probe = reinterpret_cast<WindowProbe*>(value);
        wchar_t text[256]{};
        GetWindowTextW(child, text, static_cast<int>(_countof(text)));
        if (ContainsInsensitiveW(text, L"GPK Update Failed") ||
            ContainsInsensitiveW(text, L"Just Try To Continue"))
        {
            probe->hasFailureText = true;
            return TRUE;
        }
        if (ContainsInsensitiveW(text, L"Downloading(") ||
            ContainsInsensitiveW(text, L"GPK Update UI"))
        {
            probe->hasDownloadingText = true;
        }
        return TRUE;
    }

    BOOL CALLBACK HideLegacyUpdaterWindow(HWND wnd, LPARAM)
    {
        if (!IsWindowVisible(wnd))
            return TRUE;

        wchar_t title[256]{};
        wchar_t className[128]{};
        GetWindowTextW(wnd, title, static_cast<int>(_countof(title)));
        GetClassNameW(wnd, className, static_cast<int>(_countof(className)));

        WindowProbe probe{};
        EnumChildWindows(wnd, ProbeChild, reinterpret_cast<LPARAM>(&probe));

        if (probe.hasFailureText)
        {
            // Works even when the legacy updater created the MessageBox in its
            // helper process rather than inside BOL.exe. The text match is
            // deliberately exact to the retired GPK update failure.
            PostMessageW(wnd, WM_CLOSE, 0, 0);
            if (InterlockedExchange(&g_loggedSuppressedDialog, 1) == 0)
                bol_log::Write("[GPK] Closed harmless legacy GPK update failure dialog");
            return TRUE;
        }

        const bool updater =
            ContainsInsensitiveW(title, L"GPK Update UI") ||
            ContainsInsensitiveW(title, L"GAME PROTECT KIT") ||
            probe.hasDownloadingText;

        if (updater)
        {
            ShowWindow(wnd, SW_HIDE);
            if (InterlockedExchange(&g_loggedHiddenUpdater, 1) == 0)
                bol_log::Write("[GPK] Hid obsolete GPK updater/download window");
        }
        return TRUE;
    }

    void PatchModule(HMODULE module, const wchar_t* label)
    {
        bool any = false;
        any |= PatchIat<MessageBoxAFn>(module, "MessageBoxA", &HookMessageBoxA, &g_realMessageBoxA);
        any |= PatchIat<MessageBoxWFn>(module, "MessageBoxW", &HookMessageBoxW, &g_realMessageBoxW);
        if (any)
            bol_log::Write("[GPK] legacy update dialog suppression armed in %ls", label);
    }

    DWORD WINAPI Worker(void*)
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (!user32)
            user32 = LoadLibraryW(L"user32.dll");
        if (user32)
        {
            g_realMessageBoxA = reinterpret_cast<MessageBoxAFn>(GetProcAddress(user32, "MessageBoxA"));
            g_realMessageBoxW = reinterpret_cast<MessageBoxWFn>(GetProcAddress(user32, "MessageBoxW"));
        }

        HMODULE patchedGpk = nullptr;
        HMODULE patchedUpdater = nullptr;
        for (;;)
        {
            if (HMODULE gpk = GetModuleHandleW(L"GPKitClt.dll"))
            {
                if (gpk != patchedGpk)
                {
                    PatchModule(gpk, L"GPKitClt.dll");
                    patchedGpk = gpk;
                }
            }

            if (HMODULE updater = GetModuleHandleW(L"GPKUP.dll"))
            {
                if (updater != patchedUpdater)
                {
                    PatchModule(updater, L"GPKUP.dll");
                    patchedUpdater = updater;
                }
            }

            EnumWindows(HideLegacyUpdaterWindow, 0);
            Sleep(50);
        }
    }
}

namespace gpk_suppress
{
    void Start()
    {
        if (HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr))
        {
            CloseHandle(thread);
            bol_log::Write("[GPK] obsolete updater suppression watcher started");
        }
        else
        {
            bol_log::Write("[GPK] failed to start updater suppression watcher (Win32=%lu)", GetLastError());
        }
    }
}
