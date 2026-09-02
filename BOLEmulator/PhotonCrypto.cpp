#include "EmulatorShared.h"

#pragma comment(lib, "Bcrypt.lib")

namespace bolemu
{
    // v10 intentionally advertises DH public value 1. The legacy Photon client
    // therefore derives shared secret BigInteger(1), whose GetBytes() result is
    // the single byte 0x01. DiffieHellmanCryptoProvider then SHA-256 hashes that
    // byte and uses the 32-byte digest as its Rijndael/AES key with a zero IV,
    // CBC mode and PKCS7 padding. This fixed key was verified against the first
    // encrypted BOL Authenticate packet captured from Photon3Unity3D 3.2.0.1.
    const unsigned char kPhotonAesKey[32] = {
        0x4B, 0xF5, 0x12, 0x2F, 0x34, 0x45, 0x54, 0xC5,
        0x3B, 0xDE, 0x2E, 0xBB, 0x8C, 0xD2, 0xB7, 0xE3,
        0xD1, 0x60, 0x0A, 0xD6, 0x31, 0xC3, 0x85, 0xA5,
        0xD7, 0xCC, 0xE2, 0x3C, 0x77, 0x85, 0x45, 0x9A
    };

    bool PhotonAesCrypt(
        bool encrypt,
        const unsigned char* input,
        int inputSize,
        std::vector<unsigned char>& output)
    {
        output.clear();
        if (!input || inputSize <= 0)
            return false;

        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_KEY_HANDLE key = nullptr;
        std::vector<unsigned char> keyObject;
        unsigned char iv[16] = {};
        ULONG objectLength = 0;
        ULONG resultLength = 0;
        ULONG bytesDone = 0;
        NTSTATUS status = BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0);
        if (status < 0)
            goto cleanup;

        status = BCryptSetProperty(
            algorithm,
            BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
            static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_CBC)),
            0);
        if (status < 0)
            goto cleanup;

        status = BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength),
            sizeof(objectLength),
            &bytesDone,
            0);
        if (status < 0 || objectLength == 0)
            goto cleanup;

        keyObject.resize(objectLength);
        status = BCryptGenerateSymmetricKey(
            algorithm,
            &key,
            keyObject.data(),
            static_cast<ULONG>(keyObject.size()),
            const_cast<PUCHAR>(kPhotonAesKey),
            static_cast<ULONG>(sizeof(kPhotonAesKey)),
            0);
        if (status < 0)
            goto cleanup;

        if (encrypt)
        {
            status = BCryptEncrypt(
                key,
                const_cast<PUCHAR>(input),
                static_cast<ULONG>(inputSize),
                nullptr,
                iv,
                sizeof(iv),
                nullptr,
                0,
                &resultLength,
                BCRYPT_BLOCK_PADDING);
        }
        else
        {
            status = BCryptDecrypt(
                key,
                const_cast<PUCHAR>(input),
                static_cast<ULONG>(inputSize),
                nullptr,
                iv,
                sizeof(iv),
                nullptr,
                0,
                &resultLength,
                BCRYPT_BLOCK_PADDING);
        }
        if (status < 0 || resultLength == 0)
            goto cleanup;

        output.resize(resultLength);
        std::memset(iv, 0, sizeof(iv));
        if (encrypt)
        {
            status = BCryptEncrypt(
                key,
                const_cast<PUCHAR>(input),
                static_cast<ULONG>(inputSize),
                nullptr,
                iv,
                sizeof(iv),
                output.data(),
                static_cast<ULONG>(output.size()),
                &resultLength,
                BCRYPT_BLOCK_PADDING);
        }
        else
        {
            status = BCryptDecrypt(
                key,
                const_cast<PUCHAR>(input),
                static_cast<ULONG>(inputSize),
                nullptr,
                iv,
                sizeof(iv),
                output.data(),
                static_cast<ULONG>(output.size()),
                &resultLength,
                BCRYPT_BLOCK_PADDING);
        }
        if (status < 0)
        {
            output.clear();
            goto cleanup;
        }

        output.resize(resultLength);

    cleanup:
        if (key)
            BCryptDestroyKey(key);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
        return status >= 0 && !output.empty();
    }

    bool PhotonDecrypt(const unsigned char* input, int inputSize, std::vector<unsigned char>& plain)
    {
        return PhotonAesCrypt(false, input, inputSize, plain);
    }

    bool PhotonEncrypt(const unsigned char* input, int inputSize, std::vector<unsigned char>& cipher)
    {
        return PhotonAesCrypt(true, input, inputSize, cipher);
    }

    DWORD WINAPI PhotonTcpThread(void*)
    {
        SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server == INVALID_SOCKET)
        {
            std::printf("[PHOTON/TCP] socket failed: %d\n", WSAGetLastError());
            return 0;
        }

        BOOL exclusive = TRUE;
        setsockopt(server, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kPhotonPort);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

        if (bind(server, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
            listen(server, SOMAXCONN) == SOCKET_ERROR)
        {
            std::printf("[PHOTON/TCP] bind/listen 127.0.0.1:%u failed: %d\n", kPhotonPort, WSAGetLastError());
            closesocket(server);
            return 0;
        }

        std::printf("[PHOTON/TCP] listening on 127.0.0.1:%u\n", kPhotonPort);
        std::fflush(stdout);

        for (;;)
        {
            SOCKET client = accept(server, nullptr, nullptr);
            if (client == INVALID_SOCKET)
                break;

            std::printf("[PHOTON/TCP] client connected\n");
            std::fflush(stdout);

            unsigned char buffer[4096];
            for (;;)
            {
                const int received = recv(client, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
                if (received <= 0)
                    break;
                PrintHex("[PHOTON/TCP] recv", buffer, received);
            }

            shutdown(client, SD_BOTH);
            closesocket(client);
            std::printf("[PHOTON/TCP] client disconnected\n");
            std::fflush(stdout);
        }

        closesocket(server);
        return 0;
    }
}
