#include "../EmulatorShared.h"

namespace bolemu
{
    bool SendPhotonGroupInfoResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        bool encrypted)
    {
        // Assembly-CSharp.dll -> OperationCode.GetMyGroupInfo = 158 (0x9E).
        // GroupService::OnMyGroupInfoGot reads response parameter 0 as the
        // group id. When groupId <= 0 it does not touch parameter 1 (the
        // member Presence array) and invokes the callback with an empty list.
        // A fresh local character is not in a group, so groupId=0 is the
        // smallest valid response and avoids inventing remote group members.
        std::vector<unsigned char> plain;
        plain.reserve(16);
        plain.push_back(operationCode);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // null DebugMessage
        AppendU16BE(plain, 1); // one response parameter
        plain.push_back(0);    // parameter key 0 = group id
        AppendProtocol16TypedInt(plain, 0); // groupId = 0 (not grouped)

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/UDP] GET_MY_GROUP_INFO response encryption failed\n");
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
        AppendPhotonCommandHeader(
            reply,
            6, // SEND_RELIABLE
            0,
            1,
            4,
            static_cast<unsigned int>(12 + message.size()),
            sequence);
        reply.insert(reply.end(), message.begin(), message.end());

        const int sent = sendto(
            server,
            reinterpret_cast<const char*>(reply.data()),
            static_cast<int>(reply.size()),
            0,
            reinterpret_cast<const sockaddr*>(&remote),
            remoteLength);

        if (sent != static_cast<int>(reply.size()))
        {
            std::printf("[PHOTON/UDP] GET_MY_GROUP_INFO response send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        PrintHex("[PHOTON/UDP] group-info response plaintext", plain.data(), static_cast<int>(plain.size()));
        PrintHex("[PHOTON/UDP] -> GET_MY_GROUP_INFO_RESPONSE", reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] GetMyGroupInfo success: character id=%u is not in a group ch=0 seq=%u%s\n",
                    g_localCharacter.id,
                    sequence,
                    encrypted ? " encrypted" : "");
        std::printf("[PHOTON/UDP] waiting for the next post-group-info BOL operation\n");
        std::fflush(stdout);
        return true;
    }
}
