#include "../EmulatorShared.h"

namespace bolemu
{
    bool SendPhotonMySkillPointsResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        bool encrypted)
    {
        // RetrieveCurrentCharacterDataAsyncTask is the complete post-character
        // bootstrap task in the dnSpy export. Its final network gate is
        // SkillTreeService::GetMySkillPoints (173).
        //
        // SkillTreeService::OnGetMySkillPoints consumes:
        //   [0] characterId (int)
        //   [1] earned       (int)
        //   [2] unspent      (int)
        //   [3] Hashtable[] invested skills
        std::vector<unsigned char> plain;
        plain.reserve(40);
        plain.push_back(operationCode);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // null DebugMessage
        AppendU16BE(plain, 4);

        plain.push_back(0);
        AppendProtocol16TypedInt(plain, g_localCharacter.id);
        plain.push_back(1);
        AppendProtocol16TypedInt(plain, 0);
        plain.push_back(2);
        AppendProtocol16TypedInt(plain, 0);
        plain.push_back(3);
        AppendProtocol16EmptyHashtableArray(plain);

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/UDP] GET_MY_SKILL_POINTS response encryption failed\n");
                std::fflush(stdout);
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
        AppendPhotonCommandHeader(
            reply, 6, 0, 1, 4,
            static_cast<unsigned int>(12 + message.size()), sequence);
        reply.insert(reply.end(), message.begin(), message.end());

        const int sent = sendto(
            server,
            reinterpret_cast<const char*>(reply.data()),
            static_cast<int>(reply.size()), 0,
            reinterpret_cast<const sockaddr*>(&remote), remoteLength);

        if (sent != static_cast<int>(reply.size()))
        {
            std::printf("[PHOTON/UDP] GET_MY_SKILL_POINTS response send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        PrintHex("[PHOTON/UDP] skill-points response plaintext", plain.data(), static_cast<int>(plain.size()));
        PrintHex("[PHOTON/UDP] -> GET_MY_SKILL_POINTS_RESPONSE", reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] GetMySkillPoints success: character id=%u earned=0 unspent=0 invested=0 ch=0 seq=%u%s\n",
                    g_localCharacter.id, sequence, encrypted ? " encrypted" : "");
        std::printf("[PHOTON/UDP] RetrieveCurrentCharacterDataAsyncTask has all required startup services\n");
        std::fflush(stdout);
        return true;
    }
}
