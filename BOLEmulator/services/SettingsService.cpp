#include "../EmulatorShared.h"

namespace bolemu
{
    bool SendPhotonPersonalSettingsResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        bool encrypted)
    {
        // Assembly-CSharp.dll CharacterService constructor registers operation
        // 75 with OnGetPersonalSettingsResponse. That callback requires response
        // parameter 0 to be a System.String. CharacterServiceWrapper treats an
        // empty string as "no saved settings" and continues with local defaults.
        std::vector<unsigned char> plain;
        plain.reserve(16);
        plain.push_back(operationCode);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // null DebugMessage
        AppendU16BE(plain, 1); // one response parameter
        plain.push_back(0x00); // key 0: settings string
        AppendProtocol16TypedString(plain, "");

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/UDP] GET_PERSONAL_SETTINGS response encryption failed\n");
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
            std::printf("[PHOTON/UDP] GET_PERSONAL_SETTINGS response send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        PrintHex("[PHOTON/UDP] personal-settings response plaintext",
                 plain.data(), static_cast<int>(plain.size()));
        PrintHex("[PHOTON/UDP] -> GET_PERSONAL_SETTINGS_RESPONSE",
                 reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] GetPersonalSettings success: empty local/default settings ch=0 seq=%u%s\n",
                    sequence, encrypted ? " encrypted" : "");
        std::printf("[PHOTON/UDP] waiting for the next post-settings BOL operation\n");
        std::fflush(stdout);
        return true;
    }

}
