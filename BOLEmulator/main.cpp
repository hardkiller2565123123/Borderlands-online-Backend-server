#include "EmulatorShared.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Bcrypt.lib")

int main()
{
    using namespace bolemu;

    SetConsoleTitleW(L"Borderlands Online Emulator");
    const bool loadedCharacter = LoadLocalCharacter();

    std::printf("Borderlands Online local emulator\n");
    std::printf("Listen: 127.0.0.1:%u\n", kListenPort);
    std::printf("Premade account: admin / admin\n");
    std::printf("CAS compatibility service: ready\n");
    std::printf("Game login route: Photon 127.0.0.1:%u\n", kPhotonPort);
    std::printf("Local area/group: 1 / 1 (Local)\n");
    if (loadedCharacter)
        std::printf("Local character: id=%u name=%s (loaded)\n", g_localCharacter.id, g_localCharacter.name.c_str());
    else
        std::printf("Local character: none yet (Create Character will be accepted locally)\n");
    std::printf("Waiting for emulator requests...\n\n");

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
