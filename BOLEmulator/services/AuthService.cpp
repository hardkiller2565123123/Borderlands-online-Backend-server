#include "../EmulatorShared.h"

namespace bolemu
{
    bool SendPhotonAuthenticateResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        bool onlinePeerAuthenticate)
    {
        std::vector<unsigned char> plain;
        plain.reserve(128);
        plain.push_back(operationCode);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // GpType.Null DebugMessage

        if (onlinePeerAuthenticate)
        {
            // OnlineServerPeer::OnOperationResponse consumes parameter 0 as the
            // BOL account id, then invokes AuthenticateAsyncTask's callback.
            AppendU16BE(plain, 1);
            plain.push_back(0x00);
            plain.push_back(0x69); // GpType.Int32
            AppendU32BE(plain, 100000001u);
        }
        else
        {
            // NetworkingPeer's first Authenticate is the master-server phase.
            // BOL explicitly consumes these four parameters on success:
            //   221 token -> copied into OnlineServerPeer::mToken
            //   110 account id -> OnlineService::s_accountId
            //   220 server version id
            //   108 online server address -> OnlinePeerHandler::OnConnectedToMaster
            // Without 108 the handler receives an empty string and never connects
            // the second OnlineServerPeer, which is exactly where v12 stopped.
            AppendU16BE(plain, 4);
            AppendProtocol16StringParameter(plain, 221, "LOCAL-PHOTON-TOKEN");
            plain.push_back(110);
            plain.push_back(0x69); // GpType.Int32
            AppendU32BE(plain, 100000001u);
            AppendProtocol16StringParameter(plain, 220, "1.0.753.189377");
            AppendProtocol16StringParameter(plain, 108, "127.0.0.1:5055");
        }

        std::vector<unsigned char> cipher;
        if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
        {
            std::printf("[PHOTON/UDP] auth response encryption failed\n");
            std::fflush(stdout);
            return false;
        }

        PrintHex(onlinePeerAuthenticate
                     ? "[PHOTON/UDP] online auth response plaintext"
                     : "[PHOTON/UDP] master auth response plaintext",
                 plain.data(), static_cast<int>(plain.size()));

        std::vector<unsigned char> message;
        message.reserve(2 + cipher.size());
        message.push_back(0xF3);
        message.push_back(0x83); // encrypted OperationResponse (0x80 | 3)
        message.insert(message.end(), cipher.begin(), cipher.end());

        std::vector<unsigned char> reply;
        reply.reserve(24 + message.size());
        AppendPhotonHeaderForPeer(reply, 1, 1, receivedSentTime, challenge);

        const unsigned int sequence = GetPhotonSession(remote).serverReliableSequenceCh0++;
        AppendPhotonCommandHeader(
            reply,
            6,
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
            std::printf("[PHOTON/UDP] Authenticate response send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        const bool gameServerPeer = GetPhotonSession(remote).gameServerPeer;
        PrintHex(gameServerPeer
                     ? "[PHOTON/UDP] -> GAMESERVER_AUTHENTICATE_RESPONSE"
                     : (onlinePeerAuthenticate
                            ? "[PHOTON/UDP] -> ONLINE_AUTHENTICATE_RESPONSE"
                            : "[PHOTON/UDP] -> MASTER_AUTHENTICATE_RESPONSE"),
                 reply.data(), static_cast<int>(reply.size()));

        if (onlinePeerAuthenticate)
        {
            if (gameServerPeer)
            {
                std::printf("[PHOTON/UDP] GameServerPeer Authenticate success: accountId=100000001 ch=0 seq=%u\n", sequence);
                std::printf("[PHOTON/UDP] GameServer peer ready; waiting for JoinGame (op 226)\n");
            }
            else
            {
                std::printf("[PHOTON/UDP] OnlineServerPeer Authenticate success: accountId=100000001 ch=0 seq=%u\n", sequence);
                std::printf("[PHOTON/UDP] AuthenticateAsyncTask should now continue to GetCharacterList (op 84)\n");
            }
        }
        else
        {
            std::printf("[PHOTON/UDP] Master Authenticate success: token/account/server-version supplied\n");
            std::printf("[PHOTON/UDP] online server address supplied: 127.0.0.1:5055\n");
            std::printf("[PHOTON/UDP] expecting a SECOND Photon CONNECT from BOL's OnlineServerPeer\n");
        }
        std::fflush(stdout);
        return true;
    }

}
