#include "AuthRedirect.h"
#include "Logger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <winnt.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr wchar_t kSdologinExe[] = L"sdologin.exe";
    constexpr wchar_t kEmulatorExe[] = L"BOLEmulator.exe";

    using CreateProcessWFn = BOOL(WINAPI*)(
        LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL,
        DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);

    using CreateProcessAFn = BOOL(WINAPI*)(
        LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL,
        DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);

    CreateProcessWFn g_realCreateProcessW = nullptr;
    CreateProcessAFn g_realCreateProcessA = nullptr;

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

    bool EqualsInsensitive(const wchar_t* a, const wchar_t* b)
    {
        return a && b && _wcsicmp(a, b) == 0;
    }

    std::wstring ToLowerWide(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
            return static_cast<wchar_t>(towlower(c));
        });
        return value;
    }

    std::string ToLowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    bool ContainsSdologin(const wchar_t* applicationName, const wchar_t* commandLine)
    {
        std::wstring joined;
        if (applicationName)
            joined += applicationName;
        joined += L" ";
        if (commandLine)
            joined += commandLine;
        joined = ToLowerWide(joined);
        return joined.find(L"sdologin.exe") != std::wstring::npos;
    }

    bool ContainsSdologin(const char* applicationName, const char* commandLine)
    {
        std::string joined;
        if (applicationName)
            joined += applicationName;
        joined += ' ';
        if (commandLine)
            joined += commandLine;
        joined = ToLowerAscii(joined);
        return joined.find("sdologin.exe") != std::string::npos;
    }

    std::vector<DWORD> FindProcessesByName(const wchar_t* executableName)
    {
        std::vector<DWORD> pids;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return pids;

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (EqualsInsensitive(entry.szExeFile, executableName))
                    pids.push_back(entry.th32ProcessID);
            }
            while (Process32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);
        return pids;
    }

    bool IsEmulatorRunning()
    {
        return !FindProcessesByName(kEmulatorExe).empty();
    }

    void TryStartEmulator()
    {
        if (IsEmulatorRunning())
        {
            bol_log::Write("[AUTH] BOLEmulator.exe is already running");
            return;
        }

        const std::wstring root = GetGameDirectory();
        const std::wstring path = root + L"\\" + kEmulatorExe;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            bol_log::Write("[AUTH] BOLEmulator.exe is not beside BOL.exe; start it manually or copy it beside BOL.exe");
            return;
        }

        std::wstring commandLine = L"\"" + path + L"\"";
        std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
        mutableCommand.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};

        if (CreateProcessW(
                path.c_str(),
                mutableCommand.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NEW_CONSOLE,
                nullptr,
                root.c_str(),
                &startup,
                &process))
        {
            bol_log::Write("[AUTH] Started BOLEmulator.exe (pid=%lu)", process.dwProcessId);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        else
        {
            bol_log::Write("[AUTH] Failed to start BOLEmulator.exe (Win32=%lu)", GetLastError());
        }
    }

    bool IsReadableProtection(DWORD protection)
    {
        if (protection & PAGE_GUARD)
            return false;
        const DWORD base = protection & 0xFFu;
        return base != PAGE_NOACCESS && base != 0;
    }

    bool WriteRemote(HANDLE process, std::uintptr_t address, const std::vector<unsigned char>& replacement)
    {
        DWORD oldProtection = 0;
        void* remote = reinterpret_cast<void*>(address);
        if (!VirtualProtectEx(process, remote, replacement.size(), PAGE_EXECUTE_READWRITE, &oldProtection))
            return false;

        SIZE_T written = 0;
        const BOOL ok = WriteProcessMemory(process, remote, replacement.data(), replacement.size(), &written);
        FlushInstructionCache(process, remote, replacement.size());

        DWORD ignored = 0;
        VirtualProtectEx(process, remote, replacement.size(), oldProtection, &ignored);
        return ok && written == replacement.size();
    }

    std::size_t PatchBytes(
        HANDLE process,
        const std::vector<unsigned char>& needle,
        const std::vector<unsigned char>& replacement)
    {
        if (!process || needle.empty() || needle.size() != replacement.size())
            return 0;

        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);

        const std::uintptr_t minAddress = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
        const std::uintptr_t maxAddress = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
        constexpr SIZE_T kChunkSize = 1024 * 1024;
        const SIZE_T overlap = needle.size() > 1 ? needle.size() - 1 : 0;

        std::size_t patched = 0;
        std::uintptr_t address = minAddress;

        while (address < maxAddress)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)))
                break;

            const std::uintptr_t regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const SIZE_T regionSize = mbi.RegionSize;
            const std::uintptr_t nextAddress = regionBase + regionSize;

            if (mbi.State == MEM_COMMIT && IsReadableProtection(mbi.Protect) && regionSize > 0)
            {
                SIZE_T offset = 0;
                std::vector<unsigned char> buffer(std::min<SIZE_T>(kChunkSize, regionSize));

                while (offset < regionSize)
                {
                    const SIZE_T requested = std::min<SIZE_T>(buffer.size(), regionSize - offset);
                    SIZE_T bytesRead = 0;
                    const std::uintptr_t chunkAddress = regionBase + offset;

                    if (ReadProcessMemory(
                            process,
                            reinterpret_cast<LPCVOID>(chunkAddress),
                            buffer.data(),
                            requested,
                            &bytesRead) &&
                        bytesRead >= needle.size())
                    {
                        auto begin = buffer.begin();
                        auto end = buffer.begin() + static_cast<std::ptrdiff_t>(bytesRead);
                        auto it = begin;

                        while ((it = std::search(it, end, needle.begin(), needle.end())) != end)
                        {
                            const SIZE_T localOffset = static_cast<SIZE_T>(std::distance(begin, it));
                            if (WriteRemote(process, chunkAddress + localOffset, replacement))
                                ++patched;
                            it += needle.size();
                        }
                    }

                    if (bytesRead == 0)
                        break;

                    if (bytesRead <= overlap)
                        offset += bytesRead;
                    else
                        offset += bytesRead - overlap;
                }
            }

            if (nextAddress <= address)
                break;
            address = nextAddress;
        }

        return patched;
    }

    std::vector<unsigned char> MakeAsciiPattern(const char* text)
    {
        const std::size_t bytes = std::strlen(text);
        return std::vector<unsigned char>(
            reinterpret_cast<const unsigned char*>(text),
            reinterpret_cast<const unsigned char*>(text) + bytes);
    }

    std::vector<unsigned char> MakeAsciiReplacement(const char* text, std::size_t targetSize)
    {
        std::vector<unsigned char> result(targetSize, 0);
        const std::size_t bytes = std::min<std::size_t>(std::strlen(text), targetSize);
        std::memcpy(result.data(), text, bytes);
        return result;
    }

    std::vector<unsigned char> MakeWidePattern(const wchar_t* text)
    {
        const std::size_t bytes = std::wcslen(text) * sizeof(wchar_t);
        const auto* first = reinterpret_cast<const unsigned char*>(text);
        return std::vector<unsigned char>(first, first + bytes);
    }

    std::vector<unsigned char> MakeWideReplacement(const wchar_t* text, std::size_t targetSize)
    {
        std::vector<unsigned char> result(targetSize, 0);
        const std::size_t textBytes = std::wcslen(text) * sizeof(wchar_t);
        const std::size_t bytes = std::min<std::size_t>(textBytes, targetSize);
        std::memcpy(result.data(), text, bytes);
        return result;
    }

    std::size_t PatchUrl(HANDLE process, const char* oldAscii, const wchar_t* oldWide)
    {
        constexpr char newAscii[] = "http://127.0.0.1";
        constexpr wchar_t newWide[] = L"http://127.0.0.1";

        const auto asciiNeedle = MakeAsciiPattern(oldAscii);
        const auto wideNeedle = MakeWidePattern(oldWide);

        std::size_t total = 0;
        total += PatchBytes(process, asciiNeedle, MakeAsciiReplacement(newAscii, asciiNeedle.size()));
        total += PatchBytes(process, wideNeedle, MakeWideReplacement(newWide, wideNeedle.size()));
        return total;
    }

    std::size_t PatchSdologinHandle(HANDLE process)
    {
        if (!process)
            return 0;

        std::size_t patched = 0;
        patched += PatchUrl(process, "https://cas.sdo.com", L"https://cas.sdo.com");
        patched += PatchUrl(process, "http://cas.sdo.com", L"http://cas.sdo.com");
        patched += PatchUrl(process, "https://n1.cas.sdo.com", L"https://n1.cas.sdo.com");
        patched += PatchUrl(process, "http://n1.cas.sdo.com", L"http://n1.cas.sdo.com");
        return patched;
    }

    std::size_t PatchSdologin(DWORD pid)
    {
        HANDLE process = OpenProcess(
            PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE,
            FALSE,
            pid);

        if (!process)
        {
            bol_log::Write("[AUTH] OpenProcess(sdologin pid=%lu) failed (Win32=%lu)", pid, GetLastError());
            return 0;
        }

        const std::size_t patched = PatchSdologinHandle(process);
        CloseHandle(process);
        return patched;
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

            const DWORD lookupRva = descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk : descriptor->FirstThunk;
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

    void ResolveRealCreateProcess()
    {
        if (g_realCreateProcessW && g_realCreateProcessA)
            return;

        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        if (!kernel32)
            kernel32 = LoadLibraryW(L"kernel32.dll");

        if (kernel32)
        {
            if (!g_realCreateProcessW)
                g_realCreateProcessW = reinterpret_cast<CreateProcessWFn>(GetProcAddress(kernel32, "CreateProcessW"));
            if (!g_realCreateProcessA)
                g_realCreateProcessA = reinterpret_cast<CreateProcessAFn>(GetProcAddress(kernel32, "CreateProcessA"));
        }
    }

    BOOL WINAPI HookCreateProcessW(
        LPCWSTR applicationName,
        LPWSTR commandLine,
        LPSECURITY_ATTRIBUTES processAttributes,
        LPSECURITY_ATTRIBUTES threadAttributes,
        BOOL inheritHandles,
        DWORD creationFlags,
        LPVOID environment,
        LPCWSTR currentDirectory,
        LPSTARTUPINFOW startupInfo,
        LPPROCESS_INFORMATION processInformation)
    {
        ResolveRealCreateProcess();
        if (!g_realCreateProcessW)
        {
            SetLastError(ERROR_PROC_NOT_FOUND);
            return FALSE;
        }

        const bool target = ContainsSdologin(applicationName, commandLine);
        const bool callerWantedSuspended = (creationFlags & CREATE_SUSPENDED) != 0;
        const DWORD effectiveFlags = target ? (creationFlags | CREATE_SUSPENDED) : creationFlags;

        const BOOL ok = g_realCreateProcessW(
            applicationName,
            commandLine,
            processAttributes,
            threadAttributes,
            inheritHandles,
            effectiveFlags,
            environment,
            currentDirectory,
            startupInfo,
            processInformation);

        if (!ok || !target || !processInformation)
            return ok;

        bol_log::Write("[AUTH] intercepted CreateProcessW for sdologin.exe (pid=%lu, suspended)", processInformation->dwProcessId);
        const std::size_t count = PatchSdologinHandle(processInformation->hProcess);
        bol_log::Write("[AUTH] pre-start CAS redirect patched %zu URL instance(s)", count);

        if (!callerWantedSuspended)
        {
            if (ResumeThread(processInformation->hThread) == static_cast<DWORD>(-1))
                bol_log::Write("[AUTH] ResumeThread(sdologin) failed (Win32=%lu)", GetLastError());
            else
                bol_log::Write("[AUTH] sdologin.exe resumed after redirect patch");
        }

        return ok;
    }

    BOOL WINAPI HookCreateProcessA(
        LPCSTR applicationName,
        LPSTR commandLine,
        LPSECURITY_ATTRIBUTES processAttributes,
        LPSECURITY_ATTRIBUTES threadAttributes,
        BOOL inheritHandles,
        DWORD creationFlags,
        LPVOID environment,
        LPCSTR currentDirectory,
        LPSTARTUPINFOA startupInfo,
        LPPROCESS_INFORMATION processInformation)
    {
        ResolveRealCreateProcess();
        if (!g_realCreateProcessA)
        {
            SetLastError(ERROR_PROC_NOT_FOUND);
            return FALSE;
        }

        const bool target = ContainsSdologin(applicationName, commandLine);
        const bool callerWantedSuspended = (creationFlags & CREATE_SUSPENDED) != 0;
        const DWORD effectiveFlags = target ? (creationFlags | CREATE_SUSPENDED) : creationFlags;

        const BOOL ok = g_realCreateProcessA(
            applicationName,
            commandLine,
            processAttributes,
            threadAttributes,
            inheritHandles,
            effectiveFlags,
            environment,
            currentDirectory,
            startupInfo,
            processInformation);

        if (!ok || !target || !processInformation)
            return ok;

        bol_log::Write("[AUTH] intercepted CreateProcessA for sdologin.exe (pid=%lu, suspended)", processInformation->dwProcessId);
        const std::size_t count = PatchSdologinHandle(processInformation->hProcess);
        bol_log::Write("[AUTH] pre-start CAS redirect patched %zu URL instance(s)", count);

        if (!callerWantedSuspended)
        {
            if (ResumeThread(processInformation->hThread) == static_cast<DWORD>(-1))
                bol_log::Write("[AUTH] ResumeThread(sdologin) failed (Win32=%lu)", GetLastError());
            else
                bol_log::Write("[AUTH] sdologin.exe resumed after redirect patch");
        }

        return ok;
    }

    bool HookCreateProcessInModule(HMODULE module, const char* label)
    {
        bool any = false;
        if (PatchIatEntry<CreateProcessWFn>(module, "CreateProcessW", &HookCreateProcessW, &g_realCreateProcessW))
        {
            bol_log::Write("[AUTH] hooked %s!CreateProcessW", label);
            any = true;
        }
        if (PatchIatEntry<CreateProcessAFn>(module, "CreateProcessA", &HookCreateProcessA, &g_realCreateProcessA))
        {
            bol_log::Write("[AUTH] hooked %s!CreateProcessA", label);
            any = true;
        }
        return any;
    }

    DWORD WINAPI CreateProcessHookWorker(void*)
    {
        bool entryDone = false;
        bool wrapperDone = false;

        for (;;)
        {
            if (!entryDone)
            {
                if (HMODULE module = GetModuleHandleW(L"sdologinentry.dll"))
                {
                    entryDone = HookCreateProcessInModule(module, "sdologinentry.dll");
                    if (!entryDone)
                    {
                        bol_log::Write("[AUTH] sdologinentry.dll loaded but no CreateProcessA/W import was found yet");
                        entryDone = true;
                    }
                }
            }

            if (!wrapperDone)
            {
                if (HMODULE module = GetModuleHandleW(L"ShandaLoginWrapper.dll"))
                {
                    wrapperDone = HookCreateProcessInModule(module, "ShandaLoginWrapper.dll");
                    if (!wrapperDone)
                        wrapperDone = true;
                }
            }

            if (entryDone && wrapperDone)
                break;

            Sleep(10);
        }

        return 0;
    }

    DWORD WINAPI RedirectWorker(void*)
    {
        DWORD lastPid = 0;
        unsigned int pass = 0;

        for (;;)
        {
            const auto pids = FindProcessesByName(kSdologinExe);
            if (pids.empty())
            {
                if (lastPid != 0)
                {
                    bol_log::Write("[AUTH] sdologin.exe exited; waiting for next login process");
                    lastPid = 0;
                    pass = 0;
                }
                Sleep(10);
                continue;
            }

            const DWORD pid = pids.front();
            if (pid != lastPid)
            {
                lastPid = pid;
                pass = 0;
                bol_log::Write("[AUTH] sdologin.exe detected by fallback watcher (pid=%lu)", pid);
            }

            const bool shouldPatch = pass < 100 || (pass % 50) == 0;
            if (shouldPatch)
            {
                const std::size_t count = PatchSdologin(pid);
                if (count)
                {
                    bol_log::Write(
                        "[AUTH] fallback redirected %zu CAS URL instance(s) in sdologin.exe -> http://127.0.0.1",
                        count);
                }
            }

            ++pass;
            Sleep(10);
        }
    }
}

namespace auth_redirect
{
    void Start()
    {
        ResolveRealCreateProcess();
        TryStartEmulator();

        if (HANDLE thread = CreateThread(nullptr, 0, CreateProcessHookWorker, nullptr, 0, nullptr))
        {
            CloseHandle(thread);
            bol_log::Write("[AUTH] sdologin.exe pre-start CreateProcess hook watcher started");
        }
        else
        {
            bol_log::Write("[AUTH] Failed to start CreateProcess hook watcher (Win32=%lu)", GetLastError());
        }

        if (HANDLE thread = CreateThread(nullptr, 0, RedirectWorker, nullptr, 0, nullptr))
        {
            CloseHandle(thread);
            bol_log::Write("[AUTH] sdologin.exe fallback CAS redirect watcher started");
        }
        else
        {
            bol_log::Write("[AUTH] Failed to start redirect watcher (Win32=%lu)", GetLastError());
        }
    }
}
