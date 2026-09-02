#include "../EmulatorShared.h"

namespace bolemu
{
    bool SendPhotonCharacterListResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        bool encrypted)
    {
        // CharacterService::OnGetCharacterListResponse expects parameter 0 to be
        // a Hashtable whose values are character Hashtables. Before the first
        // create this is empty; after op 82 succeeds we return the locally saved
        // character so relaunches land on character selection instead of create.
        std::vector<unsigned char> plain;
        plain.reserve(1024);
        plain.push_back(operationCode);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // null DebugMessage
        AppendU16BE(plain, 1); // one response parameter
        plain.push_back(0x00); // key 0: character table
        if (g_localCharacter.exists)
        {
            AppendProtocol16HashtableHeader(plain, 1);
            AppendProtocol16TypedInt(plain, g_localCharacter.id);
            AppendCharacterHashtableValue(plain, g_localCharacter);
        }
        else
        {
            AppendProtocol16HashtableHeader(plain, 0);
        }

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
                return false;
            message.push_back(0x83);
            message.insert(message.end(), cipher.begin(), cipher.end());
        }
        else
        {
            message.push_back(0x03);
            message.insert(message.end(), plain.begin(), plain.end());
        }

        std::vector<unsigned char> reply;
        AppendPhotonHeaderForPeer(reply, 1, 1, receivedSentTime, challenge);
        const unsigned int sequence = GetPhotonSession(remote).serverReliableSequenceCh0++;
        AppendPhotonCommandHeader(reply, 6, 0, 1, 4,
                                  static_cast<unsigned int>(12 + message.size()), sequence);
        reply.insert(reply.end(), message.begin(), message.end());

        const int sent = sendto(server,
                                reinterpret_cast<const char*>(reply.data()),
                                static_cast<int>(reply.size()), 0,
                                reinterpret_cast<const sockaddr*>(&remote), remoteLength);
        if (sent != static_cast<int>(reply.size()))
        {
            std::printf("[PHOTON/UDP] character-list response send failed: %d\n", WSAGetLastError());
            return false;
        }

        PrintHex("[PHOTON/UDP] character-list response plaintext", plain.data(), static_cast<int>(plain.size()));
        PrintHex("[PHOTON/UDP] -> GET_CHARACTER_LIST_RESPONSE", reply.data(), static_cast<int>(reply.size()));
        if (g_localCharacter.exists)
            std::printf("[PHOTON/UDP] GetCharacterList success: local character id=%u name=%s sent\n",
                        g_localCharacter.id, g_localCharacter.name.c_str());
        else
            std::printf("[PHOTON/UDP] GetCharacterList success: empty character list sent; frontend should enter CharacterCreate\n");
        std::fflush(stdout);
        return true;
    }

    bool SendPhotonCreateCharacterResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        const unsigned char* requestPlain,
        int requestPlainSize,
        bool encrypted)
    {
        LocalCharacterState created;
        if (!ParseCreateCharacterRequest(requestPlain, requestPlainSize, created))
        {
            std::printf("[PHOTON/UDP] CreateCharacter request parse failed; leaving request captured for research\n");
            std::fflush(stdout);
            return false;
        }

        // Local preservation behavior: every non-empty nickname is available.
        // The old production name database is intentionally not consulted.
        g_localCharacter = created;
        g_localCharacter.exists = true;

        std::printf("[PHOTON/UDP] op 82 identified as BOL CharacterService::CreateCharacter\n");
        std::printf("[PHOTON/UDP]   nickname=%s avatarType=%u avatarId=%u\n",
                    g_localCharacter.name.c_str(), g_localCharacter.avatarType,
                    g_localCharacter.avatarIdBits);
        std::printf("[PHOTON/UDP]   parts=%s materials=%s\n",
                    g_localCharacter.avatarParts.c_str(), g_localCharacter.avatarMaterials.c_str());

        // CharacterService::OnCreateCharacterResponse expects parameter 0 to be
        // the Character Hashtable itself. ReturnCode 0 selects the success path;
        // production return code 32732 is the "nickname already exists" path the
        // current UI was showing while v13 left op 82 unanswered.
        std::vector<unsigned char> plain;
        plain.reserve(1024);
        plain.push_back(82);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // null DebugMessage
        AppendU16BE(plain, 1);
        plain.push_back(0x00);
        AppendCharacterHashtableValue(plain, g_localCharacter);

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/UDP] CreateCharacter response encryption failed\n");
                return false;
            }
            message.push_back(0x83);
            message.insert(message.end(), cipher.begin(), cipher.end());
        }
        else
        {
            message.push_back(0x03);
            message.insert(message.end(), plain.begin(), plain.end());
        }

        std::vector<unsigned char> reply;
        reply.reserve(24 + message.size());
        AppendPhotonHeaderForPeer(reply, 1, 1, receivedSentTime, challenge);
        const unsigned int sequence = GetPhotonSession(remote).serverReliableSequenceCh0++;
        AppendPhotonCommandHeader(reply, 6, 0, 1, 4,
                                  static_cast<unsigned int>(12 + message.size()), sequence);
        reply.insert(reply.end(), message.begin(), message.end());

        const int sent = sendto(server,
                                reinterpret_cast<const char*>(reply.data()),
                                static_cast<int>(reply.size()), 0,
                                reinterpret_cast<const sockaddr*>(&remote), remoteLength);
        if (sent != static_cast<int>(reply.size()))
        {
            std::printf("[PHOTON/UDP] CreateCharacter response send failed: %d\n", WSAGetLastError());
            return false;
        }

        const bool saved = SaveLocalCharacter();
        PrintHex("[PHOTON/UDP] create-character response plaintext", plain.data(), static_cast<int>(plain.size()));
        PrintHex("[PHOTON/UDP] -> CREATE_CHARACTER_RESPONSE", reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] CreateCharacter success: id=%u name=%s ch=0 seq=%u%s\n",
                    g_localCharacter.id, g_localCharacter.name.c_str(), sequence,
                    encrypted ? " encrypted" : "");
        std::printf("[PHOTON/UDP] local character persistence: %s (%s)\n",
                    saved ? "saved" : "save failed", CharacterStatePath().c_str());
        std::printf("[PHOTON/UDP] waiting for the first post-character BOL operation\n");
        std::fflush(stdout);
        return true;
    }

    bool SendPhotonSingleCharacterResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        bool encrypted,
        const char* label)
    {
        // Assembly-CSharp.dll CharacterService uses the same response callback
        // shape for GetMyCharacter (80), GetCharacter (81), and SelectCharacter
        // (83): response parameter 0 is the selected Character Hashtable.
        // v14 stopped on op 80 after CreateCharacter because it only ACKed the
        // request and never delivered this response.
        if (!g_localCharacter.exists)
        {
            std::printf("[PHOTON/UDP] %s requested but no local character exists\n", label);
            std::fflush(stdout);
            return false;
        }

        std::vector<unsigned char> plain;
        plain.reserve(1024);
        plain.push_back(operationCode);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // null DebugMessage
        AppendU16BE(plain, 1); // one response parameter
        plain.push_back(0x00); // key 0: Character Hashtable
        AppendCharacterHashtableValue(plain, g_localCharacter);

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/UDP] %s response encryption failed\n", label);
                std::fflush(stdout);
                return false;
            }
            message.push_back(0x83); // encrypted OperationResponse
            message.insert(message.end(), cipher.begin(), cipher.end());
        }
        else
        {
            message.push_back(0x03); // OperationResponse
            message.insert(message.end(), plain.begin(), plain.end());
        }

        std::vector<unsigned char> reply;
        reply.reserve(24 + message.size());
        AppendPhotonHeaderForPeer(reply, 1, 1, receivedSentTime, challenge);
        const unsigned int sequence = GetPhotonSession(remote).serverReliableSequenceCh0++;
        AppendPhotonCommandHeader(reply, 6, 0, 1, 4,
                                  static_cast<unsigned int>(12 + message.size()), sequence);
        reply.insert(reply.end(), message.begin(), message.end());

        const int sent = sendto(server,
                                reinterpret_cast<const char*>(reply.data()),
                                static_cast<int>(reply.size()), 0,
                                reinterpret_cast<const sockaddr*>(&remote), remoteLength);
        if (sent != static_cast<int>(reply.size()))
        {
            std::printf("[PHOTON/UDP] %s response send failed: %d\n", label, WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        char plainLabel[128]{};
        char replyLabel[128]{};
        std::snprintf(plainLabel, sizeof(plainLabel), "[PHOTON/UDP] %s response plaintext", label);
        std::snprintf(replyLabel, sizeof(replyLabel), "[PHOTON/UDP] -> %s_RESPONSE", label);
        PrintHex(plainLabel, plain.data(), static_cast<int>(plain.size()));
        PrintHex(replyLabel, reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] %s success: character id=%u name=%s ch=0 seq=%u%s\n",
                    label, g_localCharacter.id, g_localCharacter.name.c_str(), sequence,
                    encrypted ? " encrypted" : "");
        std::printf("[PHOTON/UDP] waiting for the next post-character BOL operation\n");
        std::fflush(stdout);
        return true;
    }


}
