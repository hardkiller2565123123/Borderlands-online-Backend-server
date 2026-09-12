#include "../EmulatorShared.h"

namespace bolemu
{
    bool SendPhotonPresenceResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        bool encrypted)
    {
        // Presence(Dictionary<byte, object>) directly indexes the required
        // keys 0..11 plus 13 and 14. Returning an empty Hashtable makes the
        // stock client throw before the async presence request can complete.
        // Keep this as a minimal but structurally complete local presence.
        std::vector<unsigned char> plain;
        plain.reserve(160);
        plain.push_back(operationCode);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // null DebugMessage
        AppendU16BE(plain, 1); // one response parameter
        plain.push_back(0x00); // key 0: Presence Hashtable

        AppendProtocol16HashtableHeader(plain, 14);
        AppendProtocol16IntEntry(plain, 0, g_localCharacter.id);
        AppendProtocol16HashtableIntKey(plain, 1);
        plain.push_back(0x6F); // GpType.Boolean
        plain.push_back(1);    // online=true
        AppendProtocol16IntEntry(plain, 2, 0); // lastOnline unix seconds
        AppendProtocol16IntEntry(plain, 3, g_localServices.currentTown); // current town playlist id
        AppendProtocol16IntEntry(plain, 4, g_localCharacter.level);
        AppendProtocol16IntEntry(plain, 5, g_localCharacter.experience);
        AppendProtocol16IntEntry(plain, 6, g_localServices.bondCurrency); // display currency
        AppendProtocol16IntEntry(plain, 7, g_localCharacter.avatarType);
        AppendProtocol16StringEntry(plain, 8, g_localCharacter.avatarParts);
        AppendProtocol16StringEntry(plain, 9, g_localCharacter.avatarMaterials);
        AppendProtocol16IntEntry(plain, 10, g_localCharacter.avatarIdBits);
        AppendProtocol16StringEntry(plain, 11, g_localCharacter.name);
        // Presence intentionally has no required key 12 in this client build.
        AppendProtocol16IntEntry(plain, 13, 1); // local group id
        AppendProtocol16IntEntry(plain, 14, 1); // local lobby id

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/UDP] GET_PRESENCE response encryption failed\n");
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
            std::printf("[PHOTON/UDP] GET_PRESENCE response send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        PrintHex("[PHOTON/UDP] presence response plaintext",
                 plain.data(), static_cast<int>(plain.size()));
        PrintHex("[PHOTON/UDP] -> GET_PRESENCE_RESPONSE",
                 reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] GetPresence success: complete local Presence schema for character id=%u ch=0 seq=%u%s\n",
                    g_localCharacter.id, sequence, encrypted ? " encrypted" : "");
        std::printf("[PHOTON/UDP] waiting for the next post-presence BOL operation\n");
        std::fflush(stdout);
        return true;
    }

}
