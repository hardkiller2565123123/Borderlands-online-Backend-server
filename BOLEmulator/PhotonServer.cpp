#include "EmulatorShared.h"

namespace bolemu
{
    static bool g_sessionViewProbeActive = false;
    static bool g_sessionViewProbeAwaitingAck = false;
    static bool g_sessionViewProbeMismatchArmed = false;
    static unsigned int g_sessionViewProbeCandidateUsed = 0;
    static unsigned int g_sessionViewProbeExpectedAckSequence = 0;
    static unsigned int g_sessionViewProbeIgnoreMismatchUntil = 0;
    static unsigned int g_sessionViewProbeRetryCount = 0;
    static unsigned long long g_sessionViewProbeGameServerKey = 0;
    static sockaddr_in g_sessionViewProbeGameServerRemote = {};
    static int g_sessionViewProbeGameServerRemoteLength = 0;
    static unsigned int g_sessionViewProbeGameServerChallenge = 0;
    static bool g_sessionViewProbeEncrypted = false;
    static bool g_sessionViewProbeWaitingForCleanLeave = false;

    static bool g_playerViewProbeActive = false;
    static bool g_playerViewProbeAwaitingAck = false;
    static bool g_playerViewProbeMismatchArmed = false;
    static unsigned int g_playerViewProbeCandidateUsed = 0;
    static unsigned int g_playerViewProbeExpectedAckSequence = 0;
    static unsigned int g_playerViewProbeIgnoreMismatchUntil = 0;
    static unsigned int g_playerViewProbeAcceptAfter = 0;
    static unsigned long long g_playerViewProbeGameServerKey = 0;
    static sockaddr_in g_playerViewProbeGameServerRemote = {};
    static int g_playerViewProbeGameServerRemoteLength = 0;
    static unsigned int g_playerViewProbeGameServerChallenge = 0;
    static bool g_playerViewProbeEncrypted = false;
    static bool g_playerViewProbeWaitingForCleanLeave = false;

    static bool ContainsAscii(const unsigned char* data, int size, const char* text)
    {
        if (!data || size <= 0 || !text || !*text)
            return false;
        const size_t textLength = std::strlen(text);
        if (textLength > static_cast<size_t>(size))
            return false;
        for (int i = 0; i + static_cast<int>(textLength) <= size; ++i)
        {
            if (std::memcmp(data + i, text, textLength) == 0)
                return true;
        }
        return false;
    }

    static bool SendSessionProbePunCloseConnection(
        SOCKET server,
        unsigned int receivedSentTime)
    {
        // PUN event 203 (CloseConnection) is the protocol-level way for the
        // room MasterClient to ask another client to leave. Unlike a raw ENet
        // DISCONNECT, NetworkingPeer handles this by calling LeaveRoom(false),
        // which should produce op 254 and let the normal LoadBalancing state
        // machine return through Master -> GameServer on a fresh peer.
        constexpr unsigned char kCloseConnectionEventCode = 203;
        constexpr unsigned char kActorNumberParameter = 254;
        constexpr unsigned int kServerActorNumber = 1;

        std::vector<unsigned char> plain;
        plain.reserve(10);
        plain.push_back(kCloseConnectionEventCode);
        AppendU16BE(plain, 1);
        plain.push_back(kActorNumberParameter);
        plain.push_back(0x69); // GpType.Integer
        AppendU32BE(plain, kServerActorNumber);

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (g_sessionViewProbeEncrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/AUTO-VIEW] PUN CloseConnection event encryption failed\n");
                std::fflush(stdout);
                return false;
            }
            message.push_back(0x84); // encrypted EventData
            message.insert(message.end(), cipher.begin(), cipher.end());
        }
        else
        {
            message.push_back(0x04); // EventData
            message.insert(message.end(), plain.begin(), plain.end());
        }

        PhotonSessionState& gameSession = GetPhotonSession(g_sessionViewProbeGameServerRemote);
        const unsigned int sequence = gameSession.serverReliableSequenceCh0++;

        std::vector<unsigned char> reply;
        reply.reserve(24 + message.size());
        AppendPhotonHeaderForPeer(reply, 1, 1, receivedSentTime, g_sessionViewProbeGameServerChallenge);
        AppendPhotonCommandHeader(
            reply, 6, 0, 1, 4,
            static_cast<unsigned int>(12 + message.size()), sequence);
        reply.insert(reply.end(), message.begin(), message.end());

        const int sent = sendto(
            server,
            reinterpret_cast<const char*>(reply.data()),
            static_cast<int>(reply.size()),
            0,
            reinterpret_cast<const sockaddr*>(&g_sessionViewProbeGameServerRemote),
            g_sessionViewProbeGameServerRemoteLength);
        if (sent != static_cast<int>(reply.size()))
            return false;

        std::printf("[PHOTON/AUTO-VIEW] -> PUN_CLOSE_CONNECTION_EVENT_203 sender/masterActor=1 ch=0 seq=%u%s\n",
                    sequence, g_sessionViewProbeEncrypted ? " encrypted" : "");
        std::printf("[PHOTON/AUTO-VIEW] asked BOL to LeaveRoom(false); waiting for client op 254 then clean Master -> GameServer reconnect for candidate %u\n",
                    GetSessionViewCountCandidate());
        std::fflush(stdout);
        return true;
    }

    static void BeginSessionViewProbeJoin(
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int challenge,
        bool encrypted)
    {
        g_sessionViewProbeActive = true;
        g_sessionViewProbeAwaitingAck = true;
        g_sessionViewProbeMismatchArmed = false;
        g_sessionViewProbeCandidateUsed = GetSessionViewCountCandidate();
        g_sessionViewProbeGameServerKey = PhotonSessionKey(remote);
        g_sessionViewProbeGameServerRemote = remote;
        g_sessionViewProbeGameServerRemoteLength = remoteLength;
        g_sessionViewProbeGameServerChallenge = challenge;
        g_sessionViewProbeEncrypted = encrypted;
        g_sessionViewProbeRetryCount = 0;
        g_sessionViewProbeIgnoreMismatchUntil = 0;
        g_sessionViewProbeWaitingForCleanLeave = false;

        PhotonSessionState& gameSession = GetPhotonSession(remote);
        g_sessionViewProbeExpectedAckSequence =
            gameSession.serverReliableSequenceCh0 > 0 ? gameSession.serverReliableSequenceCh0 - 1 : 0;

        std::printf("[PHOTON/AUTO-VIEW] active GameServer join is testing Session PhotonView count %u; waiting for Event202 ACK seq=%u\n",
                    g_sessionViewProbeCandidateUsed, g_sessionViewProbeExpectedAckSequence);
        std::fflush(stdout);
    }

    static void HandleSessionViewProbeAck(const sockaddr_in& remote, unsigned int acknowledgedSequence, unsigned int clientTime)
    {
        if (!g_sessionViewProbeActive ||
            PhotonSessionKey(remote) != g_sessionViewProbeGameServerKey ||
            !g_sessionViewProbeAwaitingAck ||
            acknowledgedSequence != g_sessionViewProbeExpectedAckSequence)
        {
            return;
        }

        g_sessionViewProbeAwaitingAck = false;
        g_sessionViewProbeMismatchArmed = true;
        // Old debug uploads from the previous clean peer can still be queued on
        // OnlineServerPeer. Do not let those stale reports reject a brand-new
        // room candidate immediately after its Event202 ACK.
        g_sessionViewProbeIgnoreMismatchUntil = clientTime + 700;
        std::printf("[PHOTON/AUTO-VIEW] Session Event202 candidate %u ACKed (seq=%u); mismatch detector armed after stale-report guard until clientTime=%u\n",
                    g_sessionViewProbeCandidateUsed, acknowledgedSequence, g_sessionViewProbeIgnoreMismatchUntil);
        std::fflush(stdout);
    }

    static void HandleSessionViewCountMismatchReport(SOCKET server, unsigned int clientTime)
    {
        if (!g_sessionViewProbeActive ||
            g_sessionViewProbeAwaitingAck ||
            !g_sessionViewProbeMismatchArmed)
        {
            return;
        }

        if (g_sessionViewProbeIgnoreMismatchUntil != 0 &&
            clientTime < g_sessionViewProbeIgnoreMismatchUntil)
        {
            return;
        }

        g_sessionViewProbeMismatchArmed = false;
        const unsigned int rejected = g_sessionViewProbeCandidateUsed;
        const unsigned int next = AdvanceSessionViewCountCandidate();
        if (next <= rejected || next > 128)
        {
            std::printf("[PHOTON/AUTO-VIEW] clean-peer probe exhausted after rejecting %u; cached candidate=%u\n",
                        rejected, next);
            g_sessionViewProbeActive = false;
            std::fflush(stdout);
            return;
        }

        std::printf("[PHOTON/AUTO-VIEW] BOL rejected Session PhotonView count %u; candidate %u cached for a FRESH GameServer peer\n",
                    rejected, next);

        // Never put a second Event202 into a poisoned peer. Ask PUN itself to
        // leave the room so its LoadBalancing state machine performs the same
        // GameServer -> Master transition a normal room leave would use.
        g_sessionViewProbeActive = false;
        g_sessionViewProbeAwaitingAck = false;
        g_sessionViewProbeMismatchArmed = false;

        if (!SendSessionProbePunCloseConnection(server, clientTime))
        {
            std::printf("[PHOTON/AUTO-VIEW] PUN CloseConnection event send failed; candidate %u remains cached for the next reconnect/restart\n",
                        next);
        }
        else
        {
            g_sessionViewProbeWaitingForCleanLeave = true;
            std::printf("[PHOTON/AUTO-VIEW] PUN leave requested; waiting for op 254 and clean Master -> GameServer reconnect; DO NOT live-retry in this peer\n");
        }
        std::fflush(stdout);
    }

    static void NoteSessionProbeLeaveObserved()
    {
        if (!g_sessionViewProbeWaitingForCleanLeave)
            return;

        g_sessionViewProbeWaitingForCleanLeave = false;
        std::printf("[PHOTON/AUTO-VIEW] client op 254 Leave observed after PUN event 203; clean peer transition is underway\n");
        std::printf("[PHOTON/AUTO-VIEW] next JoinGame will test Session PhotonView count candidate %u\n",
                    GetSessionViewCountCandidate());
        std::fflush(stdout);
    }

    static void ConfirmSessionViewProbeIfActive(const sockaddr_in& remote)
    {
        if (!g_sessionViewProbeActive || PhotonSessionKey(remote) != g_sessionViewProbeGameServerKey)
            return;

        ConfirmSessionViewCountCandidate();
        std::printf("[PHOTON/AUTO-VIEW] safe probe succeeded with Session PhotonView count %u after %u retry/retries\n",
                    g_sessionViewProbeCandidateUsed, g_sessionViewProbeRetryCount);
        g_sessionViewProbeActive = false;
        g_sessionViewProbeAwaitingAck = false;
        g_sessionViewProbeMismatchArmed = false;
        std::fflush(stdout);
    }


    static bool SendPlayerProbePunCloseConnection(SOCKET server, unsigned int receivedSentTime)
    {
        constexpr unsigned char kCloseConnectionEventCode = 203;
        constexpr unsigned char kActorNumberParameter = 254;
        constexpr unsigned int kServerActorNumber = 1;

        std::vector<unsigned char> plain;
        plain.reserve(10);
        plain.push_back(kCloseConnectionEventCode);
        AppendU16BE(plain, 1);
        plain.push_back(kActorNumberParameter);
        plain.push_back(0x69);
        AppendU32BE(plain, kServerActorNumber);

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (g_playerViewProbeEncrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/AUTO-PLAYER] PUN CloseConnection event encryption failed\n");
                std::fflush(stdout);
                return false;
            }
            message.push_back(0x84);
            message.insert(message.end(), cipher.begin(), cipher.end());
        }
        else
        {
            message.push_back(0x04);
            message.insert(message.end(), plain.begin(), plain.end());
        }

        PhotonSessionState& gameSession = GetPhotonSession(g_playerViewProbeGameServerRemote);
        const unsigned int sequence = gameSession.serverReliableSequenceCh0++;
        std::vector<unsigned char> reply;
        reply.reserve(24 + message.size());
        AppendPhotonHeaderForPeer(reply, 1, 1, receivedSentTime, g_playerViewProbeGameServerChallenge);
        AppendPhotonCommandHeader(
            reply, 6, 0, 1, 4,
            static_cast<unsigned int>(12 + message.size()), sequence);
        reply.insert(reply.end(), message.begin(), message.end());

        const int sent = sendto(
            server,
            reinterpret_cast<const char*>(reply.data()),
            static_cast<int>(reply.size()),
            0,
            reinterpret_cast<const sockaddr*>(&g_playerViewProbeGameServerRemote),
            g_playerViewProbeGameServerRemoteLength);
        if (sent != static_cast<int>(reply.size()))
            return false;

        std::printf("[PHOTON/AUTO-PLAYER] -> PUN_CLOSE_CONNECTION_EVENT_203 sender/masterActor=1 ch=0 seq=%u%s\n",
                    sequence, g_playerViewProbeEncrypted ? " encrypted" : "");
        std::printf("[PHOTON/AUTO-PLAYER] asked BOL to leave the failed Player room; next clean room will test Player count %u\n",
                    GetPlayerViewCountCandidate());
        std::fflush(stdout);
        return true;
    }

    static bool BeginPlayerViewProbe(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int clientTime,
        unsigned int challenge,
        bool encrypted)
    {
        if (g_playerViewProbeActive && PhotonSessionKey(remote) == g_playerViewProbeGameServerKey)
            return true;

        g_playerViewProbeGameServerKey = PhotonSessionKey(remote);
        g_playerViewProbeGameServerRemote = remote;
        g_playerViewProbeGameServerRemoteLength = remoteLength;
        g_playerViewProbeGameServerChallenge = challenge;
        g_playerViewProbeEncrypted = encrypted;
        g_playerViewProbeCandidateUsed = GetPlayerViewCountCandidate();
        g_playerViewProbeAwaitingAck = false;
        g_playerViewProbeMismatchArmed = false;
        g_playerViewProbeIgnoreMismatchUntil = 0;
        g_playerViewProbeAcceptAfter = 0;
        g_playerViewProbeWaitingForCleanLeave = false;

        if (!SendPhotonPlayerInstantiateEvent(
                server, remote, remoteLength, clientTime, challenge, encrypted))
        {
            std::printf("[PHOTON/AUTO-PLAYER] failed to send Player candidate %u\n",
                        g_playerViewProbeCandidateUsed);
            std::fflush(stdout);
            return false;
        }

        PhotonSessionState& gameSession = GetPhotonSession(remote);
        g_playerViewProbeExpectedAckSequence =
            gameSession.serverReliableSequenceCh0 > 0 ? gameSession.serverReliableSequenceCh0 - 1 : 0;
        g_playerViewProbeActive = true;
        g_playerViewProbeAwaitingAck = true;
        std::printf("[PHOTON/AUTO-PLAYER] testing Player PhotonView count candidate %u; waiting for Event202 ACK seq=%u\n",
                    g_playerViewProbeCandidateUsed, g_playerViewProbeExpectedAckSequence);
        std::fflush(stdout);
        return true;
    }

    static void HandlePlayerViewProbeAck(
        const sockaddr_in& remote,
        unsigned int acknowledgedSequence,
        unsigned int clientTime)
    {
        if (!g_playerViewProbeActive ||
            PhotonSessionKey(remote) != g_playerViewProbeGameServerKey ||
            !g_playerViewProbeAwaitingAck ||
            acknowledgedSequence != g_playerViewProbeExpectedAckSequence)
        {
            return;
        }

        g_playerViewProbeAwaitingAck = false;
        g_playerViewProbeMismatchArmed = true;
        g_playerViewProbeIgnoreMismatchUntil = clientTime + 700;
        g_playerViewProbeAcceptAfter = clientTime + 3000;
        std::printf("[PHOTON/AUTO-PLAYER] Player Event202 candidate %u ACKed (seq=%u); waiting 3 seconds for a count-mismatch report\n",
                    g_playerViewProbeCandidateUsed, acknowledgedSequence);
        std::fflush(stdout);
    }

    static void MaybeConfirmPlayerViewProbe(const sockaddr_in& remote, unsigned int clientTime)
    {
        if (!g_playerViewProbeActive ||
            g_playerViewProbeAwaitingAck ||
            !g_playerViewProbeMismatchArmed ||
            PhotonSessionKey(remote) != g_playerViewProbeGameServerKey ||
            g_playerViewProbeAcceptAfter == 0 ||
            clientTime < g_playerViewProbeAcceptAfter)
        {
            return;
        }

        ConfirmPlayerViewCountCandidate();
        std::printf("[PHOTON/AUTO-PLAYER] Player candidate %u survived the mismatch window; Player prefab instantiation is now the active path\n",
                    g_playerViewProbeCandidateUsed);
        g_playerViewProbeActive = false;
        g_playerViewProbeAwaitingAck = false;
        g_playerViewProbeMismatchArmed = false;
        g_playerViewProbeAcceptAfter = 0;
        std::fflush(stdout);
    }

    static void HandlePlayerViewCountMismatchReport(SOCKET server, unsigned int clientTime)
    {
        if (!g_playerViewProbeActive ||
            g_playerViewProbeAwaitingAck ||
            !g_playerViewProbeMismatchArmed)
        {
            return;
        }

        if (g_playerViewProbeIgnoreMismatchUntil != 0 &&
            clientTime < g_playerViewProbeIgnoreMismatchUntil)
        {
            return;
        }

        g_playerViewProbeMismatchArmed = false;
        const unsigned int rejected = g_playerViewProbeCandidateUsed;
        const unsigned int next = AdvancePlayerViewCountCandidate();
        if (next <= rejected || next > 128)
        {
            std::printf("[PHOTON/AUTO-PLAYER] Player view-count probe exhausted after rejecting %u\n", rejected);
            g_playerViewProbeActive = false;
            std::fflush(stdout);
            return;
        }

        std::printf("[PHOTON/AUTO-PLAYER] BOL rejected Player PhotonView count %u; candidate %u cached for a FRESH GameServer peer\n",
                    rejected, next);

        g_playerViewProbeActive = false;
        g_playerViewProbeAwaitingAck = false;
        g_playerViewProbeMismatchArmed = false;
        if (!SendPlayerProbePunCloseConnection(server, clientTime))
        {
            std::printf("[PHOTON/AUTO-PLAYER] PUN leave send failed; candidate %u remains cached for the next restart\n", next);
        }
        else
        {
            g_playerViewProbeWaitingForCleanLeave = true;
            std::printf("[PHOTON/AUTO-PLAYER] waiting for client op 254 and clean Master -> GameServer reconnect\n");
        }
        std::fflush(stdout);
    }

    static void HandlePhotonViewCountMismatchReport(SOCKET server, unsigned int clientTime)
    {
        if (g_playerViewProbeActive)
            HandlePlayerViewCountMismatchReport(server, clientTime);
        else
            HandleSessionViewCountMismatchReport(server, clientTime);
    }

    static void NotePlayerProbeLeaveObserved()
    {
        if (!g_playerViewProbeWaitingForCleanLeave)
            return;
        g_playerViewProbeWaitingForCleanLeave = false;
        std::printf("[PHOTON/AUTO-PLAYER] client op 254 Leave observed; next clean room will retest Session=6 then Player=%u\n",
                    GetPlayerViewCountCandidate());
        std::fflush(stdout);
    }

    static void HandleRaiseEventAfterSession(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int clientTime,
        unsigned int challenge,
        const unsigned char* plain,
        int plainSize,
        bool encrypted)
    {
        ConfirmSessionViewProbeIfActive(remote);

        if (!ContainsAscii(plain, plainSize, "RPC_RequestPlayerReady"))
        {
            std::printf("[PHOTON/UDP] RaiseEvent payload captured after Session bootstrap\n");
            return;
        }

        std::printf("[PHOTON/AUTO-PLAYER] RPC_RequestPlayerReady captured for characterId=%u; creating network Player prefab\n",
                    g_localCharacter.id);
        BeginPlayerViewProbe(
            server, remote, remoteLength, clientTime, challenge, encrypted);
    }
    void LogPhotonOperationRequest(const unsigned char* plain, int plainSize)
    {
        if (!plain || plainSize < 3)
            return;

        const unsigned char operationCode = plain[0];
        const unsigned short parameterCount = ReadU16BE(plain + 1);
        std::printf("[PHOTON/UDP] OP_REQUEST code=%u (0x%02X) params=%u\n",
                    static_cast<unsigned int>(operationCode),
                    static_cast<unsigned int>(operationCode),
                    static_cast<unsigned int>(parameterCount));

        int offset = 3;
        for (unsigned int i = 0; i < parameterCount && offset < plainSize; ++i)
        {
            if (offset + 2 > plainSize)
                break;
            const unsigned char key = plain[offset++];
            const unsigned char type = plain[offset++];

            if (type == 0x73 && offset + 2 <= plainSize) // string
            {
                const unsigned short length = ReadU16BE(plain + offset);
                offset += 2;
                if (offset + length > plainSize)
                    break;
                std::string value(reinterpret_cast<const char*>(plain + offset), length);
                if (key == 109) // password field in BOL's first authenticate request
                    value = "<redacted>";
                std::printf("[PHOTON/UDP]   param key=%u type=string value=%s\n",
                            static_cast<unsigned int>(key), value.c_str());
                offset += length;
            }
            else if (type == 0x62 && offset + 1 <= plainSize) // Byte
            {
                std::printf("[PHOTON/UDP]   param key=%u type=byte value=%u\n",
                            static_cast<unsigned int>(key),
                            static_cast<unsigned int>(plain[offset] != 0));
                offset += 1;
            }
            else if (type == 0x69 && offset + 4 <= plainSize) // Int32
            {
                const unsigned int value = ReadU32BE(plain + offset);
                std::printf("[PHOTON/UDP]   param key=%u type=int value=%u\n",
                            static_cast<unsigned int>(key), value);
                offset += 4;
            }
            else
            {
                std::printf("[PHOTON/UDP]   param key=%u type=0x%02X (parser stops here)\n",
                            static_cast<unsigned int>(key), static_cast<unsigned int>(type));
                break;
            }
        }
        std::fflush(stdout);
    }

    static void PrintBOLDebugString(unsigned int fieldKey, const std::string& value)
    {
        if (value.empty())
            return;

        std::printf("[BOL/DEBUG] field %u (%u byte(s)):\n",
                    fieldKey, static_cast<unsigned int>(value.size()));

        size_t start = 0;
        while (start <= value.size())
        {
            const size_t end = value.find('\n', start);
            std::string line = value.substr(
                start,
                end == std::string::npos ? std::string::npos : end - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            std::printf("[BOL/DEBUG]   %s\n", line.c_str());

            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        std::fflush(stdout);
    }

    static bool LogRecordClientDebugInfoText(const unsigned char* plain, int plainSize)
    {
        // RecordClientDebugInfo (op 61) sends parameter 112 as a Protocol16
        // object[] containing the build string and a Dictionary<int, Hashtable>.
        // The game's debug-record Hashtable stores the managed stack/message
        // text as Int32 key 5 -> String.  The normal request logger stops at
        // object[] (0x7A), so scan the complete, already reassembled operation
        // for those typed key/string pairs and print the text verbatim.
        if (!plain || plainSize < 9 || plain[0] != 61)
            return false;

        bool sawSessionViewCountMismatch = false;

        unsigned int emitted = 0;
        for (int i = 0; i + 8 <= plainSize; ++i)
        {
            if (plain[i] != 0x69) // typed Int32 key
                continue;

            const unsigned int fieldKey = ReadU32BE(plain + i + 1);
            if (fieldKey > 32 || plain[i + 5] != 0x73) // typed String value
                continue;

            const unsigned short length = ReadU16BE(plain + i + 6);
            const int stringOffset = i + 8;
            if (length == 0 || stringOffset + static_cast<int>(length) > plainSize)
                continue;

            std::string value(
                reinterpret_cast<const char*>(plain + stringOffset),
                static_cast<size_t>(length));

            if (value.find("Error in Instantiation! The resource's PhotonView count is not the same as in incoming data.") != std::string::npos)
                sawSessionViewCountMismatch = true;

            // Avoid treating arbitrary binary as a debug string.  Managed
            // stack/message text is overwhelmingly printable plus CR/LF/TAB.
            unsigned int readable = 0;
            for (unsigned char ch : value)
            {
                if ((ch >= 0x20 && ch < 0x7F) || ch == '\r' || ch == '\n' || ch == '\t')
                    ++readable;
            }
            if (readable * 100u < static_cast<unsigned int>(value.size()) * 85u)
                continue;

            PrintBOLDebugString(fieldKey, value);
            ++emitted;

            // Skip over this string so byte patterns inside stack traces do not
            // get mistaken for nested Protocol16 fields.
            i = stringOffset + static_cast<int>(length) - 1;
        }

        if (emitted == 0)
            std::printf("[BOL/DEBUG] op 61 decoded, but no printable Int32-keyed strings were found\n");
        else
            std::printf("[BOL/DEBUG] extracted %u debug string(s) from op 61\n", emitted);
        std::fflush(stdout);
        return sawSessionViewCountMismatch;
    }

    static void HandleReassembledPhotonPayload(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int sentTime,
        unsigned int challenge,
        const std::vector<unsigned char>& payload)
    {
        if (payload.size() < 2 || payload[0] != 0xF3)
        {
            PrintHex("[PHOTON/UDP] reassembled non-RTS payload",
                     payload.data(), static_cast<int>(payload.size()));
            return;
        }

        const unsigned char wireMessageType = payload[1];
        const bool encrypted = (wireMessageType & 0x80) != 0;
        const unsigned char messageType = wireMessageType & 0x7F;
        std::printf("[PHOTON/UDP] reassembled RTS message type=%u%s size=%u\n",
                    static_cast<unsigned int>(messageType),
                    encrypted ? " encrypted" : "",
                    static_cast<unsigned int>(payload.size()));

        if (messageType != 2 || payload.size() <= 2)
        {
            PrintHex("[PHOTON/UDP] reassembled RTS payload",
                     payload.data(), static_cast<int>(payload.size()));
            return;
        }

        if (encrypted)
        {
            std::vector<unsigned char> plain;
            if (!PhotonDecrypt(payload.data() + 2, static_cast<int>(payload.size()) - 2, plain))
            {
                std::printf("[PHOTON/UDP] reassembled encrypted operation decrypt failed\n");
                return;
            }

            PrintHex("[PHOTON/UDP] reassembled decrypted operation",
                     plain.data(), static_cast<int>(plain.size()));
            LogPhotonOperationRequest(plain.data(), static_cast<int>(plain.size()));
            if (!plain.empty() && plain[0] == 61)
            {
                if (LogRecordClientDebugInfoText(plain.data(), static_cast<int>(plain.size())))
                    HandlePhotonViewCountMismatchReport(server, sentTime);
                std::printf("[PHOTON/UDP] op 61 identified as DebugInfoManager::RecordClientDebugInfo\n");
                SendPhotonEmptySuccessResponse(
                    server, remote, remoteLength, sentTime, challenge,
                    plain[0], true, "RECORD_CLIENT_DEBUG_INFO");
            }
            else if (!plain.empty())
            {
                std::printf("[PHOTON/UDP] reassembled encrypted operation %u (0x%02X) %s::%s captured\n",
                            static_cast<unsigned int>(plain[0]),
                            static_cast<unsigned int>(plain[0]),
                            LookupBOLServiceName(plain[0]),
                            LookupBOLOperationName(plain[0]));
            }
            return;
        }

        const unsigned char operationCode = payload[2];
        if (payload.size() >= 5)
        {
            const unsigned short parameterCount = ReadU16BE(payload.data() + 3);
            std::printf("[PHOTON/UDP] OP_REQUEST code=%u (0x%02X) params=%u [reassembled]\n",
                        static_cast<unsigned int>(operationCode),
                        static_cast<unsigned int>(operationCode),
                        static_cast<unsigned int>(parameterCount));
            LogPhotonOperationRequest(payload.data() + 2, static_cast<int>(payload.size()) - 2);
        }

        if (operationCode == 61)
        {
            if (LogRecordClientDebugInfoText(payload.data() + 2, static_cast<int>(payload.size()) - 2))
                HandlePhotonViewCountMismatchReport(server, sentTime);
            std::printf("[PHOTON/UDP] op 61 identified as DebugInfoManager::RecordClientDebugInfo\n");
            SendPhotonEmptySuccessResponse(
                server, remote, remoteLength, sentTime, challenge,
                operationCode, false, "RECORD_CLIENT_DEBUG_INFO");
        }
        else
        {
            std::printf("[PHOTON/UDP] reassembled operation %u (0x%02X) %s::%s captured\n",
                        static_cast<unsigned int>(operationCode),
                        static_cast<unsigned int>(operationCode),
                        LookupBOLServiceName(operationCode),
                        LookupBOLOperationName(operationCode));
        }
    }

    void HandlePhotonUdpDatagram(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        const unsigned char* buffer,
        int received)
    {
        if (received < 12)
            return;

        PhotonSessionState& session = GetPhotonSession(remote);
        char remoteIp[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &remote.sin_addr, remoteIp, sizeof(remoteIp));

        const unsigned short peerId = ReadU16BE(buffer);
        const unsigned char packetFlags = buffer[2];
        const unsigned char commandCount = buffer[3];
        const unsigned int sentTime = ReadU32BE(buffer + 4);
        const unsigned int challenge = ReadU32BE(buffer + 8);

        std::printf("[PHOTON/UDP] remote=%s:%u peer=%u flags=%u cmds=%u time=%u challenge=%08X\n",
                    remoteIp, static_cast<unsigned int>(ntohs(remote.sin_port)),
                    static_cast<unsigned int>(peerId),
                    static_cast<unsigned int>(packetFlags),
                    static_cast<unsigned int>(commandCount),
                    sentTime,
                    challenge);

        int offset = 12;
        for (unsigned int i = 0; i < commandCount; ++i)
        {
            if (offset + 12 > received)
            {
                std::printf("[PHOTON/UDP] truncated command header at %d\n", offset);
                break;
            }

            const unsigned char type = buffer[offset + 0];
            const unsigned char channel = buffer[offset + 1];
            const unsigned char flags = buffer[offset + 2];
            const unsigned char reserved = buffer[offset + 3];
            const unsigned int commandSize = ReadU32BE(buffer + offset + 4);
            const unsigned int reliableSequence = ReadU32BE(buffer + offset + 8);

            std::printf("[PHOTON/UDP] cmd type=%u ch=%u flags=%u size=%u seq=%u\n",
                        static_cast<unsigned int>(type),
                        static_cast<unsigned int>(channel),
                        static_cast<unsigned int>(flags),
                        commandSize,
                        reliableSequence);

            if (commandSize < 12 || offset + static_cast<int>(commandSize) > received)
            {
                std::printf("[PHOTON/UDP] invalid command size %u\n", commandSize);
                break;
            }

            const unsigned char* payload = buffer + offset + 12;
            const int payloadSize = static_cast<int>(commandSize) - 12;

            // A correct Player Event202 will stop producing the exact PUN
            // PhotonView-count exception. GameServer pings give us a clean
            // clock to confirm a candidate after a short quiet window.
            MaybeConfirmPlayerViewProbe(remote, sentTime);

            if (type == 2 && payloadSize >= 32)
            {
                const unsigned short requestedMtu = ReadU16BE(payload + 2);
                const unsigned int windowSize = ReadU32BE(payload + 4);
                const unsigned int channels = ReadU32BE(payload + 8);
                std::printf("[PHOTON/UDP] CONNECT mtu=%u window=%u channels=%u\n",
                            static_cast<unsigned int>(requestedMtu), windowSize, channels);

                session = PhotonSessionState{};
                std::printf("[PHOTON/UDP] new transport session remote=%s:%u\n",
                            remoteIp, static_cast<unsigned int>(ntohs(remote.sin_port)));

                SendPhotonVerifyConnect(
                    server, remote, remoteLength, payload, payloadSize, sentTime, challenge);

                // CONNECT itself is reliable (flags bit 0), so acknowledge sequence 1
                // as well. This stops the client's retry timer while it moves to init.
                if ((flags & 1) != 0)
                    SendPhotonAck(server, remote, remoteLength, channel, reliableSequence, sentTime, challenge);
            }
            else if (type == 6)
            {
                PrintHex("[PHOTON/UDP] reliable payload", payload, payloadSize);

                const bool isRtsMessage = payloadSize >= 2 && payload[0] == 0xF3;
                const unsigned char wireMessageType = isRtsMessage ? payload[1] : 0xFF;
                const bool isEncryptedRts = isRtsMessage && (wireMessageType & 0x80) != 0;
                const unsigned char messageType = isRtsMessage ? (wireMessageType & 0x7F) : 0xFF;
                if (!isRtsMessage && session.encryptionResponseSent)
                {
                    std::printf("[PHOTON/UDP] post-encryption raw payload captured (%d byte(s))\n", payloadSize);
                    std::fflush(stdout);
                }
                if (isRtsMessage)
                {
                    std::printf("[PHOTON/UDP] RTS message type=%u%s",
                                static_cast<unsigned int>(messageType),
                                isEncryptedRts ? " encrypted" : "");
                    if (messageType == 0) std::printf(" (Init)");
                    else if (messageType == 1) std::printf(" (InitResponse)");
                    else if (messageType == 2) std::printf(" (Operation)");
                    else if (messageType == 3) std::printf(" (OperationResponse)");
                    else if (messageType == 4) std::printf(" (Event)");
                    else if (messageType == 6) std::printf(" (InternalOperationRequest)");
                    else if (messageType == 7) std::printf(" (InternalOperationResponse)");
                    std::printf("\n");
                }

                if (isRtsMessage && messageType == 0)
                {
                    std::printf("[PHOTON/UDP] Photon application INIT request captured\n");
                    if (payloadSize > 9)
                    {
                        std::string app;
                        for (int n = 9; n < payloadSize && payload[n] != 0; ++n)
                        {
                            const unsigned char c = payload[n];
                            app.push_back((c >= 32 && c < 127) ? static_cast<char>(c) : '.');
                        }
                        std::printf("[PHOTON/UDP] app id/name: %s\n", app.c_str());
                    }

                    if ((flags & 1) != 0)
                        SendPhotonAck(server, remote, remoteLength, channel, reliableSequence, sentTime, challenge);

                    if (!session.initResponseSent)
                    {
                        session.initResponseSent = SendPhotonInitResponse(
                            server, remote, remoteLength, sentTime, challenge);
                    }
                }
                else
                {
                    if (isRtsMessage && messageType == 6 && payloadSize >= 11)
                    {
                        const unsigned char internalOperationCode = payload[2];
                        std::printf("[PHOTON/UDP] INTERNAL_OP_REQUEST code=%u (0x%02X)\n",
                                    static_cast<unsigned int>(internalOperationCode),
                                    static_cast<unsigned int>(internalOperationCode));

                        // InitEncryption request: op 0, one parameter (key 1),
                        // byte-array type ('x'). The captured BOL client sends a
                        // 96-byte Oakley Group 1 public key.
                        if (internalOperationCode == 0 &&
                            payload[3] == 0x00 && payload[4] == 0x01 &&
                            payload[5] == 0x01 && payload[6] == 0x78)
                        {
                            const unsigned int clientKeyLength = ReadU32BE(payload + 7);
                            std::printf("[PHOTON/UDP] InitEncryption client key length=%u\n", clientKeyLength);
                            if (clientKeyLength <= static_cast<unsigned int>(payloadSize - 11))
                                PrintHex("[PHOTON/UDP] client DH public key", payload + 11, static_cast<int>(clientKeyLength));

                            if (!session.encryptionResponseSent)
                            {
                                session.encryptionResponseSent = SendPhotonEncryptionResponse(
                                    server, remote, remoteLength, sentTime, challenge);
                            }
                        }
                    }

                    if (isRtsMessage && isEncryptedRts && messageType == 2 && payloadSize > 2)
                    {
                        std::vector<unsigned char> plain;
                        if (PhotonDecrypt(payload + 2, payloadSize - 2, plain))
                        {
                            PrintHex("[PHOTON/UDP] decrypted operation", plain.data(), static_cast<int>(plain.size()));
                            LogPhotonOperationRequest(plain.data(), static_cast<int>(plain.size()));

                            if (!plain.empty() && plain[0] == 230 && !session.authResponseSent)
                            {
                                const bool onlinePeerAuthenticate =
                                    LooksLikeOnlinePeerAuthenticate(plain.data(), static_cast<int>(plain.size()));
                                const bool gameServerAuthenticate =
                                    LooksLikeGameServerAuthenticate(plain.data(), static_cast<int>(plain.size()));
                                session.onlinePeer = onlinePeerAuthenticate;
                                session.gameServerPeer = gameServerAuthenticate;
                                const char* peerRole = gameServerAuthenticate
                                    ? "GameServerPeer"
                                    : (onlinePeerAuthenticate ? "OnlineServerPeer" : "NetworkingPeer/master");
                                std::printf("[PHOTON/UDP] Authenticate phase: %s remote=%s:%u\n",
                                            peerRole,
                                            remoteIp, static_cast<unsigned int>(ntohs(remote.sin_port)));
                                session.authResponseSent = SendPhotonAuthenticateResponse(
                                    server, remote, remoteLength, sentTime, challenge, plain[0], onlinePeerAuthenticate);
                            }
                            else if (!plain.empty() && plain[0] == 61)
                            {
                                if (LogRecordClientDebugInfoText(plain.data(), static_cast<int>(plain.size())))
                                    HandlePhotonViewCountMismatchReport(server, sentTime);
                                std::printf("[PHOTON/UDP] op 61 identified as DebugInfoManager::RecordClientDebugInfo\n");
                                SendPhotonEmptySuccessResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    plain[0], true, "RECORD_CLIENT_DEBUG_INFO");
                            }
                            else if (!plain.empty() && plain[0] == 84)
                            {
                                std::printf("[PHOTON/UDP] op 84 identified from BOL CharacterService::GetCharacterList\n");
                                session.characterListResponseSent = SendPhotonCharacterListResponse(
                                    server, remote, remoteLength, sentTime, challenge, plain[0], true);
                            }
                            else if (!plain.empty() && plain[0] == 82)
                            {
                                SendPhotonCreateCharacterResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    plain.data(), static_cast<int>(plain.size()), true);
                            }
                            else if (!plain.empty() && plain[0] == 80)
                            {
                                std::printf("[PHOTON/UDP] op 80 identified from Assembly-CSharp.dll as CharacterService::GetMyCharacter\n");
                                SendPhotonSingleCharacterResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    plain[0], true, "GET_MY_CHARACTER");
                            }
                            else if (!plain.empty() && plain[0] == 81)
                            {
                                std::printf("[PHOTON/UDP] op 81 identified from Assembly-CSharp.dll as CharacterService::GetCharacter\n");
                                SendPhotonSingleCharacterResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    plain[0], true, "GET_CHARACTER");
                            }
                            else if (!plain.empty() && plain[0] == 83)
                            {
                                std::printf("[PHOTON/UDP] op 83 identified from Assembly-CSharp.dll as CharacterService::SelectCharacter\n");
                                SendPhotonSingleCharacterResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    plain[0], true, "SELECT_CHARACTER");
                            }
                            else if (!plain.empty() && plain[0] == 75)
                            {
                                std::printf("[PHOTON/UDP] op 75 identified from Assembly-CSharp.dll as CharacterService::GetPersonalSettings\n");
                                SendPhotonPersonalSettingsResponse(
                                    server, remote, remoteLength, sentTime, challenge, plain[0], true);
                            }
                            else if (!plain.empty() && plain[0] == 74)
                            {
                                std::printf("[PHOTON/UDP] op 74 identified from Assembly-CSharp.dll as CharacterService::SetPersonalSettings\n");
                                SendPhotonEmptySuccessResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    plain[0], true, "SET_PERSONAL_SETTINGS");
                            }
                            else if (!plain.empty() && plain[0] == 147)
                            {
                                std::printf("[PHOTON/UDP] op 147 identified from Assembly-CSharp.dll as PresenceService::GetPresence\n");
                                SendPhotonPresenceResponse(
                                    server, remote, remoteLength, sentTime, challenge, plain[0], true);
                            }
                            else if (!plain.empty() && plain[0] == 196)
                            {
                                std::printf("[PHOTON/UDP] op 196 identified from Assembly-CSharp.dll as InventoryService::GetInventory\n");
                                SendPhotonInventoryResponse(
                                    server, remote, remoteLength, sentTime, challenge, plain[0], true);
                            }
                            else if (!plain.empty() && plain[0] == 99)
                            {
                                std::printf("[PHOTON/UDP] op 99 identified from Assembly-CSharp.dll as InventoryService::GetWarehouse\n");
                                SendPhotonWarehouseResponse(
                                    server, remote, remoteLength, sentTime, challenge, plain[0], true);
                            }
                            else if (!plain.empty() && plain[0] == 141)
                            {
                                std::printf("[PHOTON/UDP] op 141 identified from Assembly-CSharp.dll as QuestService::GetPlayerDailyQuestsContent\n");
                                SendPhotonDailyQuestContentsResponse(
                                    server, remote, remoteLength, sentTime, challenge, plain[0], true);
                            }
                            else if (!plain.empty() && plain[0] == 185)
                            {
                                std::printf("[PHOTON/UDP] op 185 identified from Assembly-CSharp.dll OperationCode table as QuestService::GetPlayerQuests\n");
                                SendPhotonPlayerQuestsResponse(
                                    server, remote, remoteLength, sentTime, challenge, plain[0], true);
                            }
                            else if (!plain.empty() && plain[0] == 207)
                            {
                                std::printf("[PHOTON/UDP] op 207 identified from Assembly-CSharp.dll as FriendsListsService::RetrieveFriendsList\n");
                                SendPhotonFriendsListResponse(
                                    server, remote, remoteLength, sentTime, challenge, plain[0], true);
                            }
                            else if (!plain.empty() && plain[0] == 158)
                            {
                                std::printf("[PHOTON/UDP] op 158 identified from Assembly-CSharp.dll OperationCode table as GroupService::GetMyGroupInfo\n");
                                SendPhotonGroupInfoResponse(
                                    server, remote, remoteLength, sentTime, challenge, plain[0], true);
                            }
                            else if (!plain.empty() && plain[0] == 102)
                            {
                                std::printf("[PHOTON/UDP] op 102 identified from Assembly-CSharp.dll OperationCode table as GuildService::GetMyGuildId\n");
                                SendPhotonMyGuildIdResponse(
                                    server, remote, remoteLength, sentTime, challenge, plain[0], true);
                            }
                            else if (!plain.empty() && plain[0] == 173)
                            {
                                std::printf("[PHOTON/UDP] op 173 identified from full Assembly-CSharp export as SkillTreeService::GetMySkillPoints\n");
                                SendPhotonMySkillPointsResponse(
                                    server, remote, remoteLength, sentTime, challenge, plain[0], true);
                            }
                            else if (!plain.empty() && plain[0] == 95)
                            {
                                std::printf("[PHOTON/UDP] op 95 identified from full Assembly-CSharp export as MailService::GetMail\n");
                                SendPhotonMailBootstrapResponse(
                                    server, remote, remoteLength, sentTime, challenge, plain[0], true);
                            }
                            else if (!plain.empty() && plain[0] == 226 && !session.joinGameResponseSent)
                            {
                                std::printf("[PHOTON/UDP] op 226 identified as Photon JoinGame (GameServer room join)\n");
                                session.joinGameResponseSent = SendPhotonJoinGameResponse(
                                    server, remote, remoteLength, sentTime, challenge, plain[0],
                                    plain.data(), static_cast<int>(plain.size()), true);
                                if (session.joinGameResponseSent)
                                    BeginSessionViewProbeJoin(remote, remoteLength, challenge, true);
                            }
                            else if (!plain.empty() && plain[0] == 225 && !session.joinRandomGameResponseSent)
                            {
                                std::printf("[PHOTON/UDP] op 225 identified as Photon JoinRandomGame (Master -> GameServer redirect)\n");
                                session.joinRandomGameResponseSent = SendPhotonJoinRandomGameResponse(
                                    server, remote, remoteLength, sentTime, challenge, plain[0],
                                    plain.data(), static_cast<int>(plain.size()), true);
                            }
                            else if (!plain.empty() && plain[0] == 229 && !session.joinLobbyResponseSent)
                            {
                                std::printf("[PHOTON/UDP] op 229 identified as Photon JoinLobby\n");
                                session.joinLobbyResponseSent = SendPhotonJoinLobbyResponse(
                                    server, remote, remoteLength, sentTime, challenge, plain[0], true);
                            }
                            else if (!plain.empty() && plain[0] == 253)
                            {
                                std::printf("[PHOTON/UDP] op 253 identified as Photon RaiseEvent\n");
                                if (session.gameServerPeer)
                                    HandleRaiseEventAfterSession(
                                        server, remote, remoteLength, sentTime, challenge,
                                        plain.data(), static_cast<int>(plain.size()), true);
                            }
                            else if (!plain.empty() && plain[0] == 254)
                            {
                                std::printf("[PHOTON/UDP] op 254 identified as Photon Leave\n");
                                NoteSessionProbeLeaveObserved();
                                NotePlayerProbeLeaveObserved();
                                SendPhotonEmptySuccessResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    plain[0], true, "LEAVE");
                            }
                            else if (!plain.empty())
                            {
                                std::printf("[PHOTON/UDP] unimplemented BOL operation %u (0x%02X) %s::%s captured\n",
                                            static_cast<unsigned int>(plain[0]),
                                            static_cast<unsigned int>(plain[0]),
                                            LookupBOLServiceName(plain[0]),
                                            LookupBOLOperationName(plain[0]));
                            }
                        }
                        else
                        {
                            std::printf("[PHOTON/UDP] encrypted operation decrypt failed\n");
                        }
                    }
                    else if (isRtsMessage && !isEncryptedRts && messageType == 2)
                    {
                        if (payloadSize >= 5)
                        {
                            const unsigned char operationCode = payload[2];
                            const unsigned short parameterCount = ReadU16BE(payload + 3);
                            std::printf("[PHOTON/UDP] OP_REQUEST code=%u (0x%02X) params=%u\n",
                                        static_cast<unsigned int>(operationCode),
                                        static_cast<unsigned int>(operationCode),
                                        static_cast<unsigned int>(parameterCount));

                            if (operationCode == 61)
                            {
                                if (LogRecordClientDebugInfoText(payload + 2, payloadSize - 2))
                                    HandlePhotonViewCountMismatchReport(server, sentTime);
                                std::printf("[PHOTON/UDP] op 61 identified as DebugInfoManager::RecordClientDebugInfo\n");
                                SendPhotonEmptySuccessResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    operationCode, false, "RECORD_CLIENT_DEBUG_INFO");
                            }
                            else if (operationCode == 84)
                            {
                                std::printf("[PHOTON/UDP] op 84 identified from BOL CharacterService::GetCharacterList\n");
                                session.characterListResponseSent = SendPhotonCharacterListResponse(
                                    server, remote, remoteLength, sentTime, challenge, operationCode, false);
                            }
                            else if (operationCode == 82)
                            {
                                // payload = F3 02 <OperationRequest body>; hand the body
                                // beginning at op-code 82 to the CreateCharacter parser.
                                SendPhotonCreateCharacterResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    payload + 2, payloadSize - 2, false);
                            }
                            else if (operationCode == 80)
                            {
                                std::printf("[PHOTON/UDP] op 80 identified from Assembly-CSharp.dll as CharacterService::GetMyCharacter\n");
                                SendPhotonSingleCharacterResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    operationCode, false, "GET_MY_CHARACTER");
                            }
                            else if (operationCode == 81)
                            {
                                std::printf("[PHOTON/UDP] op 81 identified from Assembly-CSharp.dll as CharacterService::GetCharacter\n");
                                SendPhotonSingleCharacterResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    operationCode, false, "GET_CHARACTER");
                            }
                            else if (operationCode == 83)
                            {
                                std::printf("[PHOTON/UDP] op 83 identified from Assembly-CSharp.dll as CharacterService::SelectCharacter\n");
                                SendPhotonSingleCharacterResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    operationCode, false, "SELECT_CHARACTER");
                            }
                            else if (operationCode == 75)
                            {
                                std::printf("[PHOTON/UDP] op 75 identified from Assembly-CSharp.dll as CharacterService::GetPersonalSettings\n");
                                SendPhotonPersonalSettingsResponse(
                                    server, remote, remoteLength, sentTime, challenge, operationCode, false);
                            }
                            else if (operationCode == 74)
                            {
                                std::printf("[PHOTON/UDP] op 74 identified from Assembly-CSharp.dll as CharacterService::SetPersonalSettings\n");
                                SendPhotonEmptySuccessResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    operationCode, false, "SET_PERSONAL_SETTINGS");
                            }
                            else if (operationCode == 147)
                            {
                                std::printf("[PHOTON/UDP] op 147 identified from Assembly-CSharp.dll as PresenceService::GetPresence\n");
                                SendPhotonPresenceResponse(
                                    server, remote, remoteLength, sentTime, challenge, operationCode, false);
                            }
                            else if (operationCode == 196)
                            {
                                std::printf("[PHOTON/UDP] op 196 identified from Assembly-CSharp.dll as InventoryService::GetInventory\n");
                                SendPhotonInventoryResponse(
                                    server, remote, remoteLength, sentTime, challenge, operationCode, false);
                            }
                            else if (operationCode == 99)
                            {
                                std::printf("[PHOTON/UDP] op 99 identified from Assembly-CSharp.dll as InventoryService::GetWarehouse\n");
                                SendPhotonWarehouseResponse(
                                    server, remote, remoteLength, sentTime, challenge, operationCode, false);
                            }
                            else if (operationCode == 141)
                            {
                                std::printf("[PHOTON/UDP] op 141 identified from Assembly-CSharp.dll as QuestService::GetPlayerDailyQuestsContent\n");
                                SendPhotonDailyQuestContentsResponse(
                                    server, remote, remoteLength, sentTime, challenge, operationCode, false);
                            }
                            else if (operationCode == 185)
                            {
                                std::printf("[PHOTON/UDP] op 185 identified from Assembly-CSharp.dll OperationCode table as QuestService::GetPlayerQuests\n");
                                SendPhotonPlayerQuestsResponse(
                                    server, remote, remoteLength, sentTime, challenge, operationCode, false);
                            }
                            else if (operationCode == 207)
                            {
                                std::printf("[PHOTON/UDP] op 207 identified from Assembly-CSharp.dll as FriendsListsService::RetrieveFriendsList\n");
                                SendPhotonFriendsListResponse(
                                    server, remote, remoteLength, sentTime, challenge, operationCode, false);
                            }
                            else if (operationCode == 158)
                            {
                                std::printf("[PHOTON/UDP] op 158 identified from Assembly-CSharp.dll OperationCode table as GroupService::GetMyGroupInfo\n");
                                SendPhotonGroupInfoResponse(
                                    server, remote, remoteLength, sentTime, challenge, operationCode, false);
                            }
                            else if (operationCode == 102)
                            {
                                std::printf("[PHOTON/UDP] op 102 identified from Assembly-CSharp.dll OperationCode table as GuildService::GetMyGuildId\n");
                                SendPhotonMyGuildIdResponse(
                                    server, remote, remoteLength, sentTime, challenge, operationCode, false);
                            }
                            else if (operationCode == 173)
                            {
                                std::printf("[PHOTON/UDP] op 173 identified from full Assembly-CSharp export as SkillTreeService::GetMySkillPoints\n");
                                SendPhotonMySkillPointsResponse(
                                    server, remote, remoteLength, sentTime, challenge, operationCode, false);
                            }
                            else if (operationCode == 95)
                            {
                                std::printf("[PHOTON/UDP] op 95 identified from full Assembly-CSharp export as MailService::GetMail\n");
                                SendPhotonMailBootstrapResponse(
                                    server, remote, remoteLength, sentTime, challenge, operationCode, false);
                            }
                            else if (operationCode == 226 && !session.joinGameResponseSent)
                            {
                                std::printf("[PHOTON/UDP] op 226 identified as Photon JoinGame (GameServer room join)\n");
                                session.joinGameResponseSent = SendPhotonJoinGameResponse(
                                    server, remote, remoteLength, sentTime, challenge, operationCode,
                                    payload + 2, payloadSize - 2, false);
                                if (session.joinGameResponseSent)
                                    BeginSessionViewProbeJoin(remote, remoteLength, challenge, false);
                            }
                            else if (operationCode == 225 && !session.joinRandomGameResponseSent)
                            {
                                std::printf("[PHOTON/UDP] op 225 identified as Photon JoinRandomGame (Master -> GameServer redirect)\n");
                                session.joinRandomGameResponseSent = SendPhotonJoinRandomGameResponse(
                                    server, remote, remoteLength, sentTime, challenge, operationCode,
                                    payload + 2, payloadSize - 2, false);
                            }
                            else if (operationCode == 229 && !session.joinLobbyResponseSent)
                            {
                                std::printf("[PHOTON/UDP] op 229 identified as Photon JoinLobby\n");
                                session.joinLobbyResponseSent = SendPhotonJoinLobbyResponse(
                                    server, remote, remoteLength, sentTime, challenge, operationCode, false);
                            }
                            else if (operationCode == 253)
                            {
                                std::printf("[PHOTON/UDP] op 253 identified as Photon RaiseEvent\n");
                                if (session.gameServerPeer)
                                    HandleRaiseEventAfterSession(
                                        server, remote, remoteLength, sentTime, challenge,
                                        payload + 2, payloadSize - 2, false);
                            }
                            else if (operationCode == 254)
                            {
                                std::printf("[PHOTON/UDP] op 254 identified as Photon Leave\n");
                                NoteSessionProbeLeaveObserved();
                                NotePlayerProbeLeaveObserved();
                                SendPhotonEmptySuccessResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    operationCode, false, "LEAVE");
                            }
                            else if (operationCode != 230)
                            {
                                std::printf("[PHOTON/UDP] unimplemented unencrypted operation %u (0x%02X) %s::%s captured\n",
                                            static_cast<unsigned int>(operationCode),
                                            static_cast<unsigned int>(operationCode),
                                            LookupBOLServiceName(operationCode),
                                            LookupBOLOperationName(operationCode));
                            }
                        }
                        else if (payloadSize >= 3)
                        {
                            std::printf("[PHOTON/UDP] OP_REQUEST code=%u (0x%02X) (short body)\n",
                                        static_cast<unsigned int>(payload[2]),
                                        static_cast<unsigned int>(payload[2]));
                        }
                        else
                        {
                            std::printf("[PHOTON/UDP] OP_REQUEST captured (short payload)\n");
                        }
                    }

                    if ((flags & 1) != 0)
                        SendPhotonAck(server, remote, remoteLength, channel, reliableSequence, sentTime, challenge);
                }
            }
            else if (type == 1 && payloadSize >= 8)
            {
                const unsigned int acknowledgedSequence = ReadU32BE(payload);
                std::printf("[PHOTON/UDP] ACK received: seq=%u sentTime=%u\n",
                            acknowledgedSequence, ReadU32BE(payload + 4));
                HandleSessionViewProbeAck(remote, acknowledgedSequence, sentTime);
                HandlePlayerViewProbeAck(remote, acknowledgedSequence, sentTime);
            }
            else if (type == 4)
            {
                std::printf("[PHOTON/UDP] DISCONNECT received (reserved/reason=%u)\n",
                            static_cast<unsigned int>(reserved));
                if ((flags & 1) != 0)
                {
                    std::printf("[PHOTON/UDP] acknowledging reliable DISCONNECT seq=%u\n", reliableSequence);
                    SendPhotonAck(server, remote, remoteLength, channel, reliableSequence, sentTime, challenge);
                }
            }
            else if (type == 8 && payloadSize >= 20)
            {
                const unsigned int startSequence = ReadU32BE(payload + 0);
                const unsigned int fragmentCount = ReadU32BE(payload + 4);
                const unsigned int fragmentNumber = ReadU32BE(payload + 8);
                const unsigned int totalLength = ReadU32BE(payload + 12);
                const unsigned int fragmentOffset = ReadU32BE(payload + 16);
                const unsigned char* fragmentData = payload + 20;
                const unsigned int fragmentLength = static_cast<unsigned int>(payloadSize - 20);

                std::printf("[PHOTON/UDP] FRAGMENT startSeq=%u part=%u/%u offset=%u bytes=%u total=%u\n",
                            startSequence, fragmentNumber + 1, fragmentCount,
                            fragmentOffset, fragmentLength, totalLength);

                const bool valid =
                    fragmentCount > 0 && fragmentCount <= 1024 &&
                    fragmentNumber < fragmentCount &&
                    totalLength > 0 && totalLength <= (16u * 1024u * 1024u) &&
                    fragmentOffset <= totalLength &&
                    fragmentLength <= totalLength - fragmentOffset;

                if (!valid)
                {
                    std::printf("[PHOTON/UDP] invalid fragment metadata; dropping fragment\n");
                }
                else
                {
                    PhotonFragmentState& fragments = session.fragments;
                    if (!fragments.active ||
                        fragments.startSequence != startSequence ||
                        fragments.fragmentCount != fragmentCount ||
                        fragments.totalLength != totalLength)
                    {
                        fragments = PhotonFragmentState{};
                        fragments.active = true;
                        fragments.startSequence = startSequence;
                        fragments.fragmentCount = fragmentCount;
                        fragments.totalLength = totalLength;
                        fragments.data.assign(totalLength, 0);
                        fragments.received.assign(fragmentCount, 0);
                        std::printf("[PHOTON/UDP] fragment assembly started startSeq=%u count=%u total=%u\n",
                                    startSequence, fragmentCount, totalLength);
                    }

                    std::copy(fragmentData, fragmentData + fragmentLength,
                              fragments.data.begin() + fragmentOffset);
                    if (fragments.received[fragmentNumber] == 0)
                    {
                        fragments.received[fragmentNumber] = 1;
                        ++fragments.receivedCount;
                    }

                    std::printf("[PHOTON/UDP] fragment assembly progress %u/%u\n",
                                fragments.receivedCount, fragments.fragmentCount);

                    if (fragments.receivedCount == fragments.fragmentCount)
                    {
                        std::vector<unsigned char> assembled;
                        assembled.swap(fragments.data);
                        const unsigned int completedStartSequence = fragments.startSequence;
                        fragments = PhotonFragmentState{};

                        std::printf("[PHOTON/UDP] fragment assembly complete startSeq=%u bytes=%u\n",
                                    completedStartSequence,
                                    static_cast<unsigned int>(assembled.size()));
                        PrintHex("[PHOTON/UDP] reassembled payload",
                                 assembled.data(), static_cast<int>(assembled.size()));
                        HandleReassembledPhotonPayload(
                            server, remote, remoteLength, sentTime, challenge, assembled);
                    }
                }

                if ((flags & 1) != 0)
                    SendPhotonAck(server, remote, remoteLength, channel, reliableSequence, sentTime, challenge);
            }
            else
            {
                if (payloadSize > 0)
                    PrintHex("[PHOTON/UDP] command payload", payload, payloadSize);
                if ((flags & 1) != 0)
                    SendPhotonAck(server, remote, remoteLength, channel, reliableSequence, sentTime, challenge);
            }

            offset += static_cast<int>(commandSize);
        }

        std::fflush(stdout);
    }

    DWORD WINAPI PhotonUdpThread(void*)
    {
        SOCKET server = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (server == INVALID_SOCKET)
        {
            std::printf("[PHOTON/UDP] socket failed: %d\n", WSAGetLastError());
            return 0;
        }

        BOOL exclusive = TRUE;
        setsockopt(server, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kPhotonPort);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

        if (bind(server, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
        {
            std::printf("[PHOTON/UDP] bind 127.0.0.1:%u failed: %d\n", kPhotonPort, WSAGetLastError());
            closesocket(server);
            return 0;
        }

        std::printf("[PHOTON/UDP] listening on 127.0.0.1:%u\n", kPhotonPort);
        std::printf("[PHOTON/UDP] Photon 3.2 master + online + GameServer responder enabled (Session safe auto-view probe + helper cleanup + BOL debug-text decoder)\n");
        std::fflush(stdout);

        for (;;)
        {
            unsigned char buffer[4096];
            sockaddr_in remote{};
            int remoteLength = sizeof(remote);
            const int received = recvfrom(
                server,
                reinterpret_cast<char*>(buffer),
                sizeof(buffer),
                0,
                reinterpret_cast<sockaddr*>(&remote),
                &remoteLength);

            if (received == SOCKET_ERROR)
                break;

            PrintHex("[PHOTON/UDP] recv", buffer, received);
            HandlePhotonUdpDatagram(server, remote, remoteLength, buffer, received);
        }

        closesocket(server);
        return 0;
    }
}
