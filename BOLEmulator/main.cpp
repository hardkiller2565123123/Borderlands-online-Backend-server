#include "EmulatorShared.h"

#include <string>
#include <vector>
#include <string_view>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Bcrypt.lib")

namespace
{

    bool FileContainsAsciiMarker(const std::wstring& path, const char* marker)
    {
        if (!marker || !*marker)
            return false;

        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 64ll * 1024ll * 1024ll)
        {
            CloseHandle(file);
            return false;
        }

        std::vector<char> data(static_cast<size_t>(size.QuadPart));
        DWORD total = 0;
        while (total < data.size())
        {
            const DWORD want = static_cast<DWORD>((data.size() - total) > 1024 * 1024 ? 1024 * 1024 : (data.size() - total));
            DWORD got = 0;
            if (!ReadFile(file, data.data() + total, want, &got, nullptr) || got == 0)
                break;
            total += got;
        }
        CloseHandle(file);
        if (total == 0)
            return false;

        const std::string_view hay(data.data(), total);
        return hay.find(marker) != std::string_view::npos;
    }

    void VerifyV63Pairing()
    {
        wchar_t executablePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
        std::wstring directory = L".";
        if (length && length < MAX_PATH)
        {
            directory.assign(executablePath, length);
            const auto slash = directory.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
                directory.resize(slash);
        }

        const std::wstring proxy = directory + L"\\version.dll";
        const DWORD attrs = GetFileAttributesW(proxy.c_str());
        const bool exists = attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
        const bool matches = exists && FileContainsAsciiMarker(proxy, "BOLREVIVAL_V63_PROXY_TRACE_MARKER");

        std::printf("[V63/PAIR] BOLEmulator build marker: BOLREVIVAL_V63_EMULATOR\n");
        if (!exists)
            std::printf("[V63/PAIR] ERROR: version.dll is missing beside BOLEmulator.exe; v63 native Unity tracing cannot run\n");
        else if (!matches)
            std::printf("[V63/PAIR] ERROR: version.dll is NOT the matching v63 proxy (marker missing). Replace BOTH BOLEmulator.exe and version.dll from the same v63 build.\n");
        else
            std::printf("[V63/PAIR] OK: matching v63 version.dll detected; native Unity loading tracer is available\n");
        std::fflush(stdout);
    }

    DWORD WINAPI LoadTraceTailThread(void*)
    {
        HANDLE file = INVALID_HANDLE_VALUE;
        unsigned long long offset = 0;
        std::string pending;

        for (;;)
        {
            if (file == INVALID_HANDLE_VALUE)
            {
                wchar_t executablePath[MAX_PATH]{};
                const DWORD executableLength = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
                std::wstring logPath = L"bol_revive.log";
                if (executableLength && executableLength < MAX_PATH)
                {
                    std::wstring directory(executablePath, executableLength);
                    const auto slash = directory.find_last_of(L"\\/");
                    if (slash != std::wstring::npos)
                        directory.resize(slash + 1);
                    logPath = directory + L"bol_revive.log";
                }

                file = CreateFileW(logPath.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE)
                {
                    Sleep(500);
                    continue;
                }

                LARGE_INTEGER size{};
                if (GetFileSizeEx(file, &size))
                    offset = static_cast<unsigned long long>(size.QuadPart);
                std::printf("[GAME/LOADTRACE] bridge armed at end of bol_revive.log; new v63 native/managed loading traces will appear here\n");
                std::fflush(stdout);
            }

            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file, &size))
            {
                CloseHandle(file);
                file = INVALID_HANDLE_VALUE;
                Sleep(250);
                continue;
            }

            const unsigned long long currentSize = static_cast<unsigned long long>(size.QuadPart);
            if (currentSize < offset)
            {
                // Log was replaced/truncated between game launches.
                offset = 0;
                pending.clear();
            }

            while (offset < currentSize)
            {
                const unsigned long long remaining = currentSize - offset;
                const DWORD toRead = static_cast<DWORD>(remaining > 4096 ? 4096 : remaining);
                LARGE_INTEGER position{};
                position.QuadPart = static_cast<LONGLONG>(offset);
                if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
                    break;

                char buffer[4096];
                DWORD bytesRead = 0;
                if (!ReadFile(file, buffer, toRead, &bytesRead, nullptr) || bytesRead == 0)
                    break;

                offset += bytesRead;
                pending.append(buffer, buffer + bytesRead);

                size_t newline = 0;
                while ((newline = pending.find('\n')) != std::string::npos)
                {
                    std::string line = pending.substr(0, newline);
                    pending.erase(0, newline + 1);
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();

                    size_t marker = line.find("[LOADTRACE]");
                    const char* bridgeTag = "[GAME/LOADTRACE]";
                    if (marker == std::string::npos)
                    {
                        marker = line.find("[MONO/LOADSTEP]");
                        bridgeTag = "[GAME/MONO-LOADSTEP]";
                    }
                    if (marker != std::string::npos)
                    {
                        std::printf("%s %s\n", bridgeTag, line.substr(marker).c_str());
                        std::fflush(stdout);
                    }
                }
            }

            Sleep(100);
        }
    }
}


int main()
{
    using namespace bolemu;

    SetConsoleTitleW(L"Borderlands Online Emulator");
    const bool loadedCharacter = LoadLocalCharacter();

    std::printf("Borderlands Online local emulator v63\n");
    std::printf("Listen: 127.0.0.1:%u\n", kListenPort);
    std::printf("Premade account: admin / admin\n");
    std::printf("CAS compatibility service: ready\n");
    std::printf("Game login route: Photon 127.0.0.1:%u\n", kPhotonPort);
    std::printf("Local area/group: 1 / 1 (Local)\n");
    if (loadedCharacter)
        std::printf("Local character: id=%u name=%s (loaded)\n", g_localCharacter.id, g_localCharacter.name.c_str());
    else
        std::printf("Local character: none yet (Create Character will be accepted locally)\n");
    std::printf("Waiting for emulator requests...\n");
    VerifyV63Pairing();
    std::printf("\n");

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::printf("WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    if (HANDLE thread = CreateThread(nullptr, 0, PhotonTcpThread, nullptr, 0, nullptr))
        CloseHandle(thread);
    if (HANDLE thread = CreateThread(nullptr, 0, PhotonUdpThread, nullptr, 0, nullptr))
        CloseHandle(thread);
    if (HANDLE thread = CreateThread(nullptr, 0, LoadTraceTailThread, nullptr, 0, nullptr))
        CloseHandle(thread);

    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET)
    {
        std::printf("socket failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    BOOL exclusive = TRUE;
    setsockopt(server, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kListenPort);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    if (bind(server, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        std::printf("bind 127.0.0.1:%u failed: %d\n", kListenPort, WSAGetLastError());
        closesocket(server);
        WSACleanup();
        return 1;
    }

    if (listen(server, SOMAXCONN) == SOCKET_ERROR)
    {
        std::printf("listen failed: %d\n", WSAGetLastError());
        closesocket(server);
        WSACleanup();
        return 1;
    }

    for (;;)
    {
        SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET)
        {
            std::printf("accept failed: %d\n", WSAGetLastError());
            break;
        }

        HandleClient(client);
        shutdown(client, SD_BOTH);
        closesocket(client);
    }

    closesocket(server);
    WSACleanup();
    return 0;
}
