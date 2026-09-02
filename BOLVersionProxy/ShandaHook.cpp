#include "ShandaHook.h"
#include "Logger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winnt.h>

#include <cstdint>
#include <cstring>
#include <cwchar>

namespace
{
    constexpr wchar_t kWrapperName[] = L"ShandaLoginWrapper.dll";
    constexpr wchar_t kLocalTicket[] = L"LOCAL-TICKET-ADMIN";

    using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
    GetProcAddressFn g_realGetProcAddress = nullptr;

    volatile LONG g_runResolved = 0;
    volatile LONG g_inlineInstalled = 0;

    bool EndsWithInsensitive(const wchar_t* value, const wchar_t* suffix)
    {
        if (!value || !suffix)
            return false;

        const std::size_t valueLength = std::wcslen(value);
        const std::size_t suffixLength = std::wcslen(suffix);
        if (suffixLength > valueLength)
            return false;

        return _wcsicmp(value + (valueLength - suffixLength), suffix) == 0;
    }

    bool IsShandaWrapper(HMODULE module)
    {
        if (!module)
            return false;

        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return false;

        return EndsWithInsensitive(path, kWrapperName);
    }

    // Native ShandaLoginWrapper.dll::Run is x86 cdecl and takes a UTF-16
    // output buffer. The original function copies a BSTR ticket into this
    // buffer and returns a native C++ bool in AL.
    bool __cdecl LocalRun(wchar_t* ticket)
    {
        if (!ticket)
        {
            bol_log::Write("[SHANDA] Run intercepted with a null ticket buffer");
            return false;
        }

        // Keep the synthetic ticket deliberately short. The original ABI does
        // not provide a destination capacity to Run either.
        std::memcpy(ticket, kLocalTicket, sizeof(kLocalTicket));
        bol_log::Write("[SHANDA] Run intercepted -> local emulator ticket issued for admin");
        return true;
    }

    FARPROC WINAPI HookGetProcAddress(HMODULE module, LPCSTR procName)
    {
        if (procName && reinterpret_cast<std::uintptr_t>(procName) > 0xFFFFu &&
            std::strcmp(procName, "Run") == 0 && IsShandaWrapper(module))
        {
            if (InterlockedExchange(&g_runResolved, 1) == 0)
                bol_log::Write("[SHANDA] P/Invoke resolved ShandaLoginWrapper.dll!Run -> local hook");

            return reinterpret_cast<FARPROC>(&LocalRun);
        }

        return g_realGetProcAddress ? g_realGetProcAddress(module, procName) : nullptr;
    }

    template <typename T>
    bool PatchIatEntry(HMODULE module, const char* importName, T hook, T* original)
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

        const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!directory.VirtualAddress || !directory.Size)
            return false;

        auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
        for (; descriptor->Name; ++descriptor)
        {
            if (!descriptor->FirstThunk)
                continue;

            const DWORD lookupRva = descriptor->OriginalFirstThunk
                ? descriptor->OriginalFirstThunk
                : descriptor->FirstThunk;

            auto* lookup = reinterpret_cast<IMAGE_THUNK_DATA*>(base + lookupRva);
            auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);

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
                    return false;

                *slot = hook;
                FlushInstructionCache(GetCurrentProcess(), slot, sizeof(T));

                DWORD ignored = 0;
                VirtualProtect(slot, sizeof(T), oldProtect, &ignored);
                return true;
            }
        }

        return false;
    }

    bool InstallInlineRunFallback(HMODULE wrapper)
    {
        if (!wrapper || InterlockedCompareExchange(&g_inlineInstalled, 0, 0) != 0)
            return true;

        FARPROC run = g_realGetProcAddress
            ? g_realGetProcAddress(wrapper, "Run")
            : ::GetProcAddress(wrapper, "Run");
        if (!run)
        {
            bol_log::Write("[SHANDA] ShandaLoginWrapper.dll loaded but Run export was not found");
            return false;
        }

#if defined(_M_IX86)
        auto* target = reinterpret_cast<unsigned char*>(run);
        const std::intptr_t displacement =
            static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(&LocalRun)) -
            static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(target) + 5u);

        unsigned char patch[5]{};
        patch[0] = 0xE9;
        const std::int32_t rel32 = static_cast<std::int32_t>(displacement);
        std::memcpy(patch + 1, &rel32, sizeof(rel32));

        DWORD oldProtect = 0;
        if (!VirtualProtect(target, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            bol_log::Write("[SHANDA] VirtualProtect(Run) failed (Win32=%lu)", GetLastError());
            return false;
        }

        std::memcpy(target, patch, sizeof(patch));
        FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));

        DWORD ignored = 0;
        VirtualProtect(target, sizeof(patch), oldProtect, &ignored);

        InterlockedExchange(&g_inlineInstalled, 1);
        bol_log::Write("[SHANDA] inline fallback installed on ShandaLoginWrapper.dll!Run @ %p", run);
        return true;
#else
        bol_log::Write("[SHANDA] inline fallback skipped: this build must be Win32/x86");
        return false;
#endif
    }

    void ResolveRealGetProcAddress()
    {
        if (g_realGetProcAddress)
            return;

        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        if (!kernel32)
            return;

        // The version proxy itself has not patched kernel32, so resolving from
        // the export table here gives us the real routine used for forwarding.
        g_realGetProcAddress = reinterpret_cast<GetProcAddressFn>(::GetProcAddress(kernel32, "GetProcAddress"));
    }

    DWORD WINAPI HookWorker(void*)
    {
        ResolveRealGetProcAddress();
        if (!g_realGetProcAddress)
        {
            bol_log::Write("[SHANDA] failed to resolve real GetProcAddress");
            return 0;
        }

        bool exeHooked = false;
        bool monoHooked = false;
        bool wrapperSeen = false;

        for (;;)
        {
            if (!exeHooked)
            {
                if (HMODULE exe = GetModuleHandleW(nullptr))
                {
                    exeHooked = PatchIatEntry<GetProcAddressFn>(
                        exe, "GetProcAddress", &HookGetProcAddress, &g_realGetProcAddress);
                    if (exeHooked)
                        bol_log::Write("[SHANDA] BOL.exe GetProcAddress resolver hook installed");
                    else
                        exeHooked = true; // not required if Mono owns P/Invoke resolution
                }
            }

            if (!monoHooked)
            {
                if (HMODULE mono = GetModuleHandleW(L"mono.dll"))
                {
                    monoHooked = PatchIatEntry<GetProcAddressFn>(
                        mono, "GetProcAddress", &HookGetProcAddress, &g_realGetProcAddress);
                    if (monoHooked)
                        bol_log::Write("[SHANDA] mono.dll GetProcAddress resolver hook installed");
                    else
                    {
                        bol_log::Write("[SHANDA] mono.dll has no direct GetProcAddress IAT entry; inline fallback will be used");
                        monoHooked = true;
                    }
                }
            }

            if (!wrapperSeen)
            {
                if (HMODULE wrapper = GetModuleHandleW(kWrapperName))
                {
                    wrapperSeen = true;
                    bol_log::Write("[SHANDA] original ShandaLoginWrapper.dll detected @ %p (file left untouched)", wrapper);
                    InstallInlineRunFallback(wrapper);
                }
            }

            if (monoHooked && wrapperSeen)
                break;

            Sleep(1);
        }

        return 0;
    }
}

namespace shanda_hook
{
    void Start()
    {
        if (HANDLE thread = CreateThread(nullptr, 0, HookWorker, nullptr, 0, nullptr))
        {
            CloseHandle(thread);
            bol_log::Write("[SHANDA] local Run hook watcher started");
        }
        else
        {
            bol_log::Write("[SHANDA] failed to start hook watcher (Win32=%lu)", GetLastError());
        }
    }
}
