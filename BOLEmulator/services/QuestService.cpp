#include "../EmulatorShared.h"

namespace bolemu
{
    bool SendPhotonDailyQuestContentsResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        bool encrypted)
    {
        // Assembly-CSharp.dll -> QuestService uses operation 141 (0x8D) for
        // GetPlayerDailyQuestsContent / OnGetDailyQuestContents.
        // OnGetDailyQuestContents reads response parameter 0, casts it to
        // System.Collections.Hashtable, and enumerates its entries. An empty
        // Hashtable is therefore a valid local baseline for a new character:
        // there are simply no daily quests assigned yet.
        std::vector<unsigned char> plain;
        plain.reserve(16);
        plain.push_back(operationCode);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // null DebugMessage
        AppendU16BE(plain, 1); // one response parameter
        plain.push_back(0x00); // key 0: daily-quest-content Hashtable
        AppendEmptyHashtableValue(plain);

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/UDP] GET_DAILY_QUEST_CONTENTS response encryption failed\n");
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
            std::printf("[PHOTON/UDP] GET_DAILY_QUEST_CONTENTS response send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        PrintHex("[PHOTON/UDP] daily-quest-contents response plaintext",
                 plain.data(), static_cast<int>(plain.size()));
        PrintHex("[PHOTON/UDP] -> GET_DAILY_QUEST_CONTENTS_RESPONSE",
                 reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] GetPlayerDailyQuestsContent success: empty local daily quest list for character id=%u ch=0 seq=%u%s\n",
                    g_localCharacter.id, sequence, encrypted ? " encrypted" : "");
        std::printf("[PHOTON/UDP] waiting for the next post-quest BOL operation\n");
        std::fflush(stdout);
        return true;
    }


    bool SendPhotonPlayerQuestsResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        bool encrypted)
    {
        // Assembly-CSharp.dll -> OperationCode.GetPlayerQuests = 185 (0xB9).
        // QuestService::OnGetPlayerQuests reads:
        //   response parameter 0 -> Int32 characterId
        //   response parameter 1 -> Hashtable quest progress entries
        // For a new local character an empty quest Hashtable is valid.
        std::vector<unsigned char> plain;
        plain.reserve(24);
        plain.push_back(operationCode);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // null DebugMessage
        AppendU16BE(plain, 2); // two response parameters

        plain.push_back(0x00); // key 0: characterId
        AppendProtocol16TypedInt(plain, g_localCharacter.id);

        plain.push_back(0x01); // key 1: player quest Hashtable
        AppendEmptyHashtableValue(plain);

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/UDP] GET_PLAYER_QUESTS response encryption failed\n");
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
            std::printf("[PHOTON/UDP] GET_PLAYER_QUESTS response send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        PrintHex("[PHOTON/UDP] player-quests response plaintext",
                 plain.data(), static_cast<int>(plain.size()));
        PrintHex("[PHOTON/UDP] -> GET_PLAYER_QUESTS_RESPONSE",
                 reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] GetPlayerQuests success: character id=%u empty local quest progress ch=0 seq=%u%s\n",
                    g_localCharacter.id, sequence, encrypted ? " encrypted" : "");
        std::printf("[PHOTON/UDP] waiting for the next post-player-quests BOL operation\n");
        std::fflush(stdout);
        return true;
    }
}
