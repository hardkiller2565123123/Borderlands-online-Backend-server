#include "EmuBootstrap.h"
#include "AuthRedirect.h"
#include "Logger.h"
#include "ShandaHook.h"

#include <string>
#include <vector>
#include <cwctype>

namespace
{
    bool FileExists(const std::wstring& path)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

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

    std::wstring GetExecutablePath()
    {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return L"";
        return std::wstring(path, length);
    }

    std::wstring LowerCommandLine()
    {
        const wchar_t* raw = GetCommandLineW();
        if (!raw)
            return L"";

        std::wstring value(raw);
        for (wchar_t& ch : value)
            ch = static_cast<wchar_t>(std::towlower(ch));
        return value;
    }

    bool HasArgument(const wchar_t* value)
    {
        if (!value || !*value)
            return false;

        std::wstring wanted(value);
        for (wchar_t& ch : wanted)
            ch = static_cast<wchar_t>(std::towlower(ch));

        return LowerCommandLine().find(wanted) != std::wstring::npos;
    }

    bool RelaunchForLocalGameBackend()
    {
        const std::wstring executable = GetExecutablePath();
        if (executable.empty())
        {
            bol_log::Write("[BOOT] Could not resolve BOL.exe path for local backend relaunch");
            return false;
        }

        const wchar_t* raw = GetCommandLineW();
        std::wstring commandLine = (raw && *raw)
            ? std::wstring(raw)
            : (L"\"" + executable + L"\"");

        // IMPORTANT: do NOT add -sndalogin here. That selects the optional
        // Shanda ticket path and skips the normal username/password Login.swf.
        // The normal login flow authenticates against the game's Photon backend.
        commandLine += L" -server:127.0.0.1";
        if (!HasArgument(L"-areaid:"))
            commandLine += L" -areaid:1";
        if (!HasArgument(L"-groupid:"))
            commandLine += L" -groupid:1";

        std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
        mutableCommand.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const std::wstring root = GetGameDirectory();

        if (!CreateProcessW(
                executable.c_str(),
                mutableCommand.data(),
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                root.c_str(),
                &startup,
                &process))
        {
            bol_log::Write("[BOOT] Local backend relaunch failed (Win32=%lu)", GetLastError());
            return false;
        }

        bol_log::Write("[BOOT] Relaunched BOL.exe with -server:127.0.0.1 -areaid:1 -groupid:1 (pid=%lu)", process.dwProcessId);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }

    void LogKnownFiles()
    {
        const std::wstring root = GetGameDirectory();

        struct Entry
        {
            const wchar_t* relative;
            const char* label;
        };

        const Entry files[] =
        {
            { L"BOL_Data\\Managed\\Assembly-CSharp.dll", "Assembly-CSharp.dll" },
            { L"BOL_Data\\Mono\\mono.dll", "mono.dll" },
            { L"BOL_Data\\Managed\\Photon3Unity3D.dll", "Photon3Unity3D.dll" },
            { L"BOL_Data\\Plugins\\ShandaLoginWrapper.dll", "ShandaLoginWrapper.dll" },
            { L"GPKitClt.dll", "GPKitClt.dll (game root)" },
            { L"BOL_Data\\Plugins\\GPKitClt.dll", "GPKitClt.dll (Plugins)" },
            { L"BOLEmulator.exe", "BOLEmulator.exe" }
        };

        for (const auto& entry : files)
        {
            const std::wstring path = root + L"\\" + entry.relative;
            bol_log::Write("[FILES] %-34s : %s", entry.label, FileExists(path) ? "FOUND" : "missing");
        }
    }

    void WatchNativeLoginModules()
    {
        bool sawMono = false;
        bool sawShanda = false;
        bool sawGpk = false;

        for (;;)
        {
            if (!sawMono)
            {
                if (HMODULE module = GetModuleHandleW(L"mono.dll"))
                {
                    bol_log::Write("[MODULE] mono.dll loaded @ %p", module);
                    sawMono = true;
                }
            }

            if (!sawShanda)
            {
                if (HMODULE module = GetModuleHandleW(L"ShandaLoginWrapper.dll"))
                {
                    bol_log::Write("[MODULE] ShandaLoginWrapper.dll loaded @ %p", module);
                    sawShanda = true;
                }
            }

            if (!sawGpk)
            {
                if (HMODULE module = GetModuleHandleW(L"GPKitClt.dll"))
                {
                    bol_log::Write("[MODULE] GPKitClt.dll loaded @ %p", module);
                    sawGpk = true;
                }
            }

            if (sawMono && sawShanda && sawGpk)
                break;

            Sleep(250);
        }
    }
}

namespace emu
{
    DWORD WINAPI BootstrapThread(void*)
    {
        bol_log::Initialize();

        const bool sndaLogin = HasArgument(L"-sndalogin");

        // The previous build forced -sndalogin. Reverse engineering of the
        // managed LoginBehaviour shows that was the wrong branch for the normal
        // username/password screen. Normal login uses Login.swf and then an
        // AuthenticateAsyncTask over Photon on port 5055.
        if (!sndaLogin && !HasArgument(L"-server:"))
        {
            bol_log::Write("[BOOT] Normal username/password login selected");
            bol_log::Write("[BOOT] Routing the game backend to 127.0.0.1 (Photon port 5055)");
            if (RelaunchForLocalGameBackend())
            {
                bol_log::Write("[BOOT] Closing the original process; the local-backend process will continue startup");
                Sleep(100);
                ExitProcess(0);
            }

            bol_log::Write("[BOOT] Relaunch failed; continuing with the original server configuration");
        }
        else if (sndaLogin)
        {
            bol_log::Write("[BOOT] WARNING: -sndalogin was supplied manually; this bypasses the normal Login.swf username/password flow");
        }
        else
        {
            bol_log::Write("[BOOT] Local game backend argument present; normal username/password login remains enabled");
        }

        bol_log::Write("Borderlands Online revival bootstrap started");
        bol_log::Write("Architecture: x86 / Win32");
        bol_log::Write("Emulator services: game backend routing + CAS compatibility");
        bol_log::Write("Local account target: admin / admin");
        bol_log::Write("VERSION proxy: includes VerQueryValueW required by BOL_Data\\Mono\\mono.dll");

        LogKnownFiles();

        // Only arm the direct Shanda Run hook when the user explicitly asks for
        // the optional -sndalogin path. In normal mode this hook must stay off:
        // returning a synthetic Shanda ticket immediately skips Login.swf and
        // advances into an unimplemented backend state, which caused the crash
        // observed in v6.
        if (sndaLogin)
        {
            shanda_hook::Start();
            bol_log::Write("[SHANDA] direct Run hook enabled because -sndalogin was explicitly requested");
        }
        else
        {
            bol_log::Write("[SHANDA] direct Run hook disabled in normal username/password mode");
        }

        // Keep legacy CAS routing available for any auxiliary sdologin requests,
        // but normal BOL account login is now known to go through Photon.
        auth_redirect::Start();

        WatchNativeLoginModules();

        bol_log::Write("[EMU] Normal login path active. Login.swf -> AuthenticateAsyncTask -> Photon 127.0.0.1:5055.");
        return 0;
    }
}
