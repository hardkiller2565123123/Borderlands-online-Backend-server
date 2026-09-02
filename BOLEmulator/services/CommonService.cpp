#include "../EmulatorShared.h"

namespace bolemu
{
    bool SendPhotonEmptySuccessResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        bool encrypted,
        const char* label)
    {
        std::vector<unsigned char> plain;
        plain.reserve(8);
        plain.push_back(operationCode);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // null DebugMessage
        AppendU16BE(plain, 0); // no response parameters

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
            std::printf("[PHOTON/UDP] %s response send failed: %d\n", label, WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        char replyLabel[128]{};
        std::snprintf(replyLabel, sizeof(replyLabel), "[PHOTON/UDP] -> %s_RESPONSE", label);
        PrintHex(replyLabel, reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] %s success ch=0 seq=%u%s\n",
                    label, sequence, encrypted ? " encrypted" : "");
        std::fflush(stdout);
        return true;
    }

    bool SendPhotonJoinLobbyResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        bool encrypted)
    {
        // Photon LoadBalancing operation 229 is JoinLobby. A successful response
        // has no parameters; PhotonHandler/LoadBalancingPeer turns this into the
        // managed OnJoinedLobby path.
        std::vector<unsigned char> plain;
        plain.reserve(8);
        plain.push_back(operationCode);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // GpType.Null DebugMessage
        AppendU16BE(plain, 0); // parameter count

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/UDP] JoinLobby response encryption failed\n");
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
            std::printf("[PHOTON/UDP] JoinLobby response send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        PrintHex("[PHOTON/UDP] JoinLobby response plaintext", plain.data(), static_cast<int>(plain.size()));
        PrintHex("[PHOTON/UDP] -> JOIN_LOBBY_RESPONSE", reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] JoinLobby success sent op=%u ch=0 seq=%u%s\n",
                    static_cast<unsigned int>(operationCode), sequence,
                    encrypted ? " encrypted" : "");
        std::printf("[PHOTON/UDP] waiting for the first post-lobby BOL operation\n");
        std::fflush(stdout);
        return true;
    }


    static bool SkipProtocol16Value(
        const unsigned char* data,
        int size,
        int& offset)
    {
        if (!data || offset >= size)
            return false;

        const unsigned char type = data[offset++];
        switch (type)
        {
            case 0x2A: // Null
                return true;

            case 0x62: // Byte
            case 0x6F: // Boolean
                if (offset + 1 > size)
                    return false;
                offset += 1;
                return true;

            case 0x6B: // Int16
                if (offset + 2 > size)
                    return false;
                offset += 2;
                return true;

            case 0x69: // Int32
            case 0x66: // Single
                if (offset + 4 > size)
                    return false;
                offset += 4;
                return true;

            case 0x6C: // Int64
            case 0x64: // Double
                if (offset + 8 > size)
                    return false;
                offset += 8;
                return true;

            case 0x73: // String
            case 0x78: // ByteArray
            {
                if (offset + 2 > size)
                    return false;
                const unsigned short length = ReadU16BE(data + offset);
                offset += 2;
                if (offset + static_cast<int>(length) > size)
                    return false;
                offset += static_cast<int>(length);
                return true;
            }

            case 0x68: // Hashtable
            {
                if (offset + 2 > size)
                    return false;
                const unsigned short count = ReadU16BE(data + offset);
                offset += 2;
                for (unsigned int i = 0; i < count; ++i)
                {
                    if (!SkipProtocol16Value(data, size, offset) ||
                        !SkipProtocol16Value(data, size, offset))
                        return false;
                }
                return true;
            }

            case 0x7A: // Object[] / object array
            {
                if (offset + 2 > size)
                    return false;
                const unsigned short count = ReadU16BE(data + offset);
                offset += 2;
                for (unsigned int i = 0; i < count; ++i)
                {
                    if (!SkipProtocol16Value(data, size, offset))
                        return false;
                }
                return true;
            }

            case 0x79: // Strongly typed array
            {
                if (offset + 3 > size)
                    return false;
                const unsigned short count = ReadU16BE(data + offset);
                offset += 2;
                const unsigned char elementType = data[offset++];

                // The BOL room path only needs primitive/hashtable arrays, but
                // supporting the common Protocol16 element forms makes this
                // extractor safe for future room-property growth.
                for (unsigned int i = 0; i < count; ++i)
                {
                    switch (elementType)
                    {
                        case 0x62:
                        case 0x6F:
                            if (offset + 1 > size) return false;
                            offset += 1;
                            break;
                        case 0x6B:
                            if (offset + 2 > size) return false;
                            offset += 2;
                            break;
                        case 0x69:
                        case 0x66:
                            if (offset + 4 > size) return false;
                            offset += 4;
                            break;
                        case 0x6C:
                        case 0x64:
                            if (offset + 8 > size) return false;
                            offset += 8;
                            break;
                        case 0x73:
                        case 0x78:
                        {
                            if (offset + 2 > size) return false;
                            const unsigned short length = ReadU16BE(data + offset);
                            offset += 2;
                            if (offset + static_cast<int>(length) > size) return false;
                            offset += static_cast<int>(length);
                            break;
                        }
                        case 0x68:
                        {
                            // A typed Hashtable[] element omits the type byte.
                            if (offset + 2 > size) return false;
                            const unsigned short pairs = ReadU16BE(data + offset);
                            offset += 2;
                            for (unsigned int pair = 0; pair < pairs; ++pair)
                            {
                                if (!SkipProtocol16Value(data, size, offset) ||
                                    !SkipProtocol16Value(data, size, offset))
                                    return false;
                            }
                            break;
                        }
                        default:
                            return false;
                    }
                }
                return true;
            }

            default:
                return false;
        }
    }

    static bool ExtractTopLevelParameterValue(
        const unsigned char* plain,
        int plainSize,
        unsigned char wantedKey,
        std::vector<unsigned char>& encodedValue)
    {
        encodedValue.clear();
        if (!plain || plainSize < 3)
            return false;

        const unsigned short parameterCount = ReadU16BE(plain + 1);
        int offset = 3;
        for (unsigned int parameter = 0; parameter < parameterCount; ++parameter)
        {
            if (offset >= plainSize)
                return false;

            const unsigned char key = plain[offset++];
            const int valueStart = offset;
            int valueEnd = offset;
            if (!SkipProtocol16Value(plain, plainSize, valueEnd))
                return false;

            if (key == wantedKey)
            {
                encodedValue.assign(plain + valueStart, plain + valueEnd);
                return true;
            }
            offset = valueEnd;
        }
        return false;
    }

    static void AppendProtocol16EncodedParameter(
        std::vector<unsigned char>& out,
        unsigned char key,
        const std::vector<unsigned char>& encodedValue)
    {
        out.push_back(key);
        out.insert(out.end(), encodedValue.begin(), encodedValue.end());
    }


    bool SendPhotonJoinRandomGameResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        const unsigned char* requestPlain,
        int requestPlainSize,
        bool encrypted)
    {
        // Photon LoadBalancing JoinRandomGame (225) is handled by the Master.
        // On success the response tells the client which room was selected and
        // which Game Server to connect to. Keep both endpoints on our local
        // Photon listener so the next connection is captured by this emulator.
        constexpr unsigned char kRoomNameParameter = 255;
        constexpr unsigned char kAddressParameter = 230;
        const std::string roomName = "LocalGame";
        const std::string gameServerAddress = "127.0.0.1:5055";

        constexpr unsigned char kGamePropertiesParameter = 248;
        std::vector<unsigned char> matchmakingProperties;
        const bool haveMatchmakingProperties = ExtractTopLevelParameterValue(
            requestPlain, requestPlainSize, kGamePropertiesParameter, matchmakingProperties);

        std::vector<unsigned char> plain;
        plain.reserve(128 + matchmakingProperties.size());
        plain.push_back(operationCode);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // GpType.Null DebugMessage
        AppendU16BE(plain, haveMatchmakingProperties ? 3 : 2);
        AppendProtocol16StringParameter(plain, kRoomNameParameter, roomName);
        if (haveMatchmakingProperties)
            AppendProtocol16EncodedParameter(plain, kGamePropertiesParameter, matchmakingProperties);
        AppendProtocol16StringParameter(plain, kAddressParameter, gameServerAddress);

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/UDP] JoinRandomGame response encryption failed\n");
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
            std::printf("[PHOTON/UDP] JoinRandomGame response send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        PrintHex("[PHOTON/UDP] JoinRandomGame response plaintext", plain.data(), static_cast<int>(plain.size()));
        PrintHex("[PHOTON/UDP] -> JOIN_RANDOM_GAME_RESPONSE", reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] JoinRandomGame success: room=%s gameServer=%s ch=0 seq=%u%s\n",
                    roomName.c_str(), gameServerAddress.c_str(), sequence,
                    encrypted ? " encrypted" : "");
        if (haveMatchmakingProperties)
            std::printf("[PHOTON/UDP] JoinRandomGame mirrored matchmaking/game properties from request param 248 (%u byte(s))\n",
                        static_cast<unsigned int>(matchmakingProperties.size()));
        else
            std::printf("[PHOTON/UDP] WARNING: JoinRandomGame could not mirror request param 248; using legacy room metadata\n");
        std::printf("[PHOTON/UDP] expecting GameServer transition: disconnect/reconnect then JoinGame (op 226)\n");
        std::fflush(stdout);
        return true;
    }


    static void AppendProtocol16IntParameter(
        std::vector<unsigned char>& out,
        unsigned char key,
        unsigned int value)
    {
        out.push_back(key);
        AppendProtocol16TypedInt(out, value);
    }

    static void AppendProtocol16EmptyHashtableParameter(
        std::vector<unsigned char>& out,
        unsigned char key)
    {
        out.push_back(key);
        AppendEmptyHashtableValue(out);
    }

    static void AppendProtocol16IntArrayParameter(
        std::vector<unsigned char>& out,
        unsigned char key,
        const unsigned int* values,
        unsigned short count)
    {
        // Protocol16 strongly typed array: 'y', short length, element type, values.
        // Deserializes as System.Int32[] in Photon3Unity3D.
        out.push_back(key);
        out.push_back(0x79); // GpType.Array
        AppendU16BE(out, count);
        out.push_back(0x69); // GpType.Integer element type
        for (unsigned short i = 0; i < count; ++i)
            AppendU32BE(out, values[i]);
    }

    static void AppendProtocol16ServerActorPropertiesValue(std::vector<unsigned char>& out)
    {
        // Minimal PhotonPlayer properties for the emulated host/master actor.
        // NetworkingPeer expects property key byte.MaxValue (255) to be a string.
        AppendProtocol16HashtableHeader(out, 1);
        AppendProtocol16TypedByte(out, 0xFF);
        AppendProtocol16TypedString(out, "LocalServer");
    }

    static bool SendPhotonJoinEvent(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        const std::vector<unsigned char>& actorProperties,
        bool encrypted)
    {
        constexpr unsigned char kJoinEventCode = 255;
        constexpr unsigned char kPlayerPropertiesParameter = 249;
        constexpr unsigned char kActorListParameter = 252;
        constexpr unsigned char kActorNumberParameter = 254;
        constexpr unsigned int kServerActorNumber = 1;
        constexpr unsigned int kLocalActorNumber = 2;

        // Protocol16 EventData body: event code + parameter table.
        std::vector<unsigned char> plain;
        plain.reserve(32);
        plain.push_back(kJoinEventCode);
        AppendU16BE(plain, 3);
        if (!actorProperties.empty())
            AppendProtocol16EncodedParameter(plain, kPlayerPropertiesParameter, actorProperties);
        else
            AppendProtocol16EmptyHashtableParameter(plain, kPlayerPropertiesParameter);
        AppendProtocol16IntParameter(plain, kActorNumberParameter, kLocalActorNumber);
        const unsigned int actorList[] = { kServerActorNumber, kLocalActorNumber };
        AppendProtocol16IntArrayParameter(plain, kActorListParameter, actorList, 2);

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/UDP] Join event encryption failed\n");
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
            static_cast<int>(reply.size()),
            0,
            reinterpret_cast<const sockaddr*>(&remote),
            remoteLength);
        if (sent != static_cast<int>(reply.size()))
        {
            std::printf("[PHOTON/UDP] Join event send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        PrintHex("[PHOTON/UDP] Join event plaintext", plain.data(), static_cast<int>(plain.size()));
        PrintHex("[PHOTON/UDP] -> JOIN_EVENT_255", reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] Join event 255 sent: localActor=2 actors=[1,2] masterActor=1 ch=0 seq=%u%s\n",
                    sequence, encrypted ? " encrypted" : "");
        std::printf("[PHOTON/UDP] client should now enter NetworkingPeer.OnJoinedRoom\n");
        std::fflush(stdout);
        return true;
    }

    static unsigned int g_sessionViewCountCandidate = 0;

    static std::string SessionViewCountCachePath()
    {
        char modulePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
        std::string path(modulePath, modulePath + (length > 0 ? length : 0));
        const size_t slash = path.find_last_of("\\/");
        if (slash != std::string::npos)
            path.resize(slash + 1);
        else
            path.clear();
        path += "bol_session_view_count.txt";
        return path;
    }

    static void SaveSessionViewCountCandidate(unsigned int value)
    {
        const std::string path = SessionViewCountCachePath();
        FILE* file = nullptr;
        if (fopen_s(&file, path.c_str(), "wb") == 0 && file)
        {
            std::fprintf(file, "%u\n", value);
            std::fclose(file);
        }
    }

    unsigned int GetSessionViewCountCandidate()
    {
        // v40 clean-peer probing proved the shipped BOL Session prefab has
        // exactly six PhotonViews. Lock this now so stale debug uploads can no
        // longer move a known-good Session count.
        if (g_sessionViewCountCandidate != 6)
        {
            g_sessionViewCountCandidate = 6;
            SaveSessionViewCountCandidate(6);
        }
        return g_sessionViewCountCandidate;
    }

    unsigned int AdvanceSessionViewCountCandidate()
    {
        // Retained for compatibility with the probe plumbing. Count 6 is now
        // known-good and must not be advanced.
        g_sessionViewCountCandidate = 6;
        SaveSessionViewCountCandidate(6);
        return 6;
    }

    void ConfirmSessionViewCountCandidate()
    {
        const unsigned int current = GetSessionViewCountCandidate();
        SaveSessionViewCountCandidate(current);
        std::printf("[PHOTON/AUTO-VIEW] Session PhotonView count accepted: %u (cached for future runs)\n", current);
        std::fflush(stdout);
    }

    static bool SendPhotonSessionInstantiateEvent(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        bool encrypted)
    {
        // BOL's PostJoinAsyncTask does not complete just because Photon reports
        // OnJoinedRoom. It explicitly waits for the network-instantiated
        // Session singleton. NetworkingPeer handles PUN instantiate event 202
        // by reading parameter 245 as the instantiate-data Hashtable.
        constexpr unsigned char kInstantiateEventCode = 202;
        constexpr unsigned char kEventDataParameter = 245;
        constexpr unsigned char kActorNumberParameter = 254;
        constexpr unsigned int kServerActorNumber = 1;
        constexpr unsigned int kSessionViewId = 1001; // first PhotonView / instantiation ID
        const unsigned int sessionViewCount = GetSessionViewCountCandidate();

        std::vector<unsigned char> instantiateData;
        instantiateData.reserve(80);
        AppendProtocol16HashtableHeader(instantiateData, 4);

        // PUN instantiate Hashtables use BYTE keys. NetworkingPeer reads
        // evData[(byte)0], evData[(byte)6], and evData[(byte)7]; encoding
        // these keys as Int32 makes the lookup fail even though the numbers match.
        // Key 0 is the prefab name; use the resource name directly.
        AppendProtocol16TypedByte(instantiateData, 0);
        AppendProtocol16TypedString(instantiateData, "Session");

        // Hashtable key 4: the complete PhotonView ID array. The exact count
        // is learned automatically from BOL's own RecordClientDebugInfo stream.
        // A rejected candidate is cached as candidate+1 and the next GameServer
        // room join retries without requiring another emulator build.
        AppendProtocol16TypedByte(instantiateData, 4);
        instantiateData.push_back(0x79); // GpType.Array
        AppendU16BE(instantiateData, static_cast<unsigned short>(sessionViewCount));
        instantiateData.push_back(0x69); // GpType.Integer elements
        for (unsigned int i = 0; i < sessionViewCount; ++i)
            AppendU32BE(instantiateData, kSessionViewId + i);

        // Hashtable key 6: server timestamp used by the PUN instantiate path.
        AppendProtocol16TypedByte(instantiateData, 6);
        AppendProtocol16TypedInt(instantiateData, receivedSentTime);

        // Hashtable key 7: instantiation ID. For a multi-view PUN prefab this
        // remains the first allocated view ID.
        AppendProtocol16TypedByte(instantiateData, 7);
        AppendProtocol16TypedInt(instantiateData, kSessionViewId);

        // Protocol16 EventData body: event code + parameter table.
        std::vector<unsigned char> plain;
        plain.reserve(16 + instantiateData.size());
        plain.push_back(kInstantiateEventCode);
        AppendU16BE(plain, 2);
        AppendProtocol16EncodedParameter(plain, kEventDataParameter, instantiateData);
        AppendProtocol16IntParameter(plain, kActorNumberParameter, kServerActorNumber);

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/UDP] Session instantiate event encryption failed\n");
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
            static_cast<int>(reply.size()),
            0,
            reinterpret_cast<const sockaddr*>(&remote),
            remoteLength);
        if (sent != static_cast<int>(reply.size()))
        {
            std::printf("[PHOTON/UDP] Session instantiate event send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        PrintHex("[PHOTON/UDP] Session instantiate event plaintext", plain.data(), static_cast<int>(plain.size()));
        PrintHex("[PHOTON/UDP] -> INSTANTIATE_EVENT_202_SESSION", reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] Session instantiate event 202 sent: prefab=Session sender/masterActor=1 instantiationId=%u viewCount=%u viewIds=%u..%u byteKeys=[0,4,6,7] ch=0 seq=%u%s\n",
                    kSessionViewId,
                    sessionViewCount,
                    kSessionViewId,
                    kSessionViewId + sessionViewCount - 1,
                    sequence,
                    encrypted ? " encrypted" : "");
        std::printf("[PHOTON/AUTO-VIEW] testing Session PhotonView count candidate %u; wrong counts will use PUN LeaveRoom on a fresh peer\n",
                    sessionViewCount);
        std::printf("[PHOTON/UDP] PostJoinAsyncTask should now observe the Session singleton; next expected client traffic is RPC/RaiseEvent to master actor 1\n");
        std::fflush(stdout);
        return true;
    }

    bool RetryPhotonSessionInstantiateEvent(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        bool encrypted)
    {
        return SendPhotonSessionInstantiateEvent(
            server, remote, remoteLength, receivedSentTime, challenge, encrypted);
    }

    static unsigned int g_playerViewCountCandidate = 0;

    static std::string PlayerViewCountCachePath()
    {
        char modulePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
        std::string path(modulePath, modulePath + (length > 0 ? length : 0));
        const size_t slash = path.find_last_of("\\/");
        if (slash != std::string::npos)
            path.resize(slash + 1);
        else
            path.clear();
        path += "bol_player_view_count.txt";
        return path;
    }

    static void SavePlayerViewCountCandidate(unsigned int value)
    {
        const std::string path = PlayerViewCountCachePath();
        FILE* file = nullptr;
        if (fopen_s(&file, path.c_str(), "wb") == 0 && file)
        {
            std::fprintf(file, "%u\n", value);
            std::fclose(file);
        }
    }

    unsigned int GetPlayerViewCountCandidate()
    {
        if (g_playerViewCountCandidate != 0)
            return g_playerViewCountCandidate;

        g_playerViewCountCandidate = 1;
        const std::string path = PlayerViewCountCachePath();
        FILE* file = nullptr;
        if (fopen_s(&file, path.c_str(), "rb") == 0 && file)
        {
            unsigned int cached = 0;
            if (std::fscanf(file, "%u", &cached) == 1 && cached >= 1 && cached <= 128)
                g_playerViewCountCandidate = cached;
            std::fclose(file);
        }
        return g_playerViewCountCandidate;
    }

    unsigned int AdvancePlayerViewCountCandidate()
    {
        unsigned int current = GetPlayerViewCountCandidate();
        if (current < 128)
            ++current;
        g_playerViewCountCandidate = current;
        SavePlayerViewCountCandidate(current);
        return current;
    }

    void ConfirmPlayerViewCountCandidate()
    {
        const unsigned int current = GetPlayerViewCountCandidate();
        SavePlayerViewCountCandidate(current);
        std::printf("[PHOTON/AUTO-PLAYER] Player PhotonView count accepted: %u (cached for future runs)\n", current);
        std::fflush(stdout);
    }

    bool SendPhotonPlayerInstantiateEvent(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        bool encrypted)
    {
        // Session occupies 1001..1006. The first Player instance starts at
        // 1007 so its view IDs never collide with the known-good Session.
        constexpr unsigned char kInstantiateEventCode = 202;
        constexpr unsigned char kEventDataParameter = 245;
        constexpr unsigned char kActorNumberParameter = 254;
        constexpr unsigned int kServerActorNumber = 1;
        constexpr unsigned int kLocalActorNumber = 2;
        constexpr unsigned int kPlayerViewId = 1007;
        const unsigned int playerViewCount = GetPlayerViewCountCandidate();

        std::vector<unsigned char> instantiateData;
        instantiateData.reserve(128);
        AppendProtocol16HashtableHeader(instantiateData, 5);

        // key 0: Resources prefab name.
        AppendProtocol16TypedByte(instantiateData, 0);
        AppendProtocol16TypedString(instantiateData, "Player");

        // key 4: complete PhotonView ID array for the Player prefab.
        AppendProtocol16TypedByte(instantiateData, 4);
        instantiateData.push_back(0x79); // GpType.Array
        AppendU16BE(instantiateData, static_cast<unsigned short>(playerViewCount));
        instantiateData.push_back(0x69); // Int32 elements
        for (unsigned int i = 0; i < playerViewCount; ++i)
            AppendU32BE(instantiateData, kPlayerViewId + i);

        // key 5: InstantiateManager spawn data:
        // [actorId, name, accountId, characterId, experience].
        AppendProtocol16TypedByte(instantiateData, 5);
        instantiateData.push_back(0x7A); // GpType.ObjectArray
        AppendU16BE(instantiateData, 5);
        AppendProtocol16TypedInt(instantiateData, kLocalActorNumber);
        AppendProtocol16TypedString(instantiateData, "");
        AppendProtocol16TypedInt(instantiateData, g_localCharacter.accountId);
        AppendProtocol16TypedInt(instantiateData, g_localCharacter.id);
        AppendProtocol16TypedInt(instantiateData, g_localCharacter.experience);

        AppendProtocol16TypedByte(instantiateData, 6);
        AppendProtocol16TypedInt(instantiateData, receivedSentTime);

        AppendProtocol16TypedByte(instantiateData, 7);
        AppendProtocol16TypedInt(instantiateData, kPlayerViewId);

        std::vector<unsigned char> plain;
        plain.reserve(20 + instantiateData.size());
        plain.push_back(kInstantiateEventCode);
        AppendU16BE(plain, 2);
        AppendProtocol16EncodedParameter(plain, kEventDataParameter, instantiateData);
        AppendProtocol16IntParameter(plain, kActorNumberParameter, kServerActorNumber);

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/AUTO-PLAYER] Player instantiate event encryption failed\n");
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
            static_cast<int>(reply.size()),
            0,
            reinterpret_cast<const sockaddr*>(&remote),
            remoteLength);
        if (sent != static_cast<int>(reply.size()))
        {
            std::printf("[PHOTON/AUTO-PLAYER] Player instantiate event send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        PrintHex("[PHOTON/UDP] Player instantiate event plaintext", plain.data(), static_cast<int>(plain.size()));
        PrintHex("[PHOTON/UDP] -> INSTANTIATE_EVENT_202_PLAYER", reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/AUTO-PLAYER] Player Event202 sent: prefab=Player actor=2 characterId=%u instantiationId=%u viewCount=%u viewIds=%u..%u ch=0 seq=%u%s\n",
                    g_localCharacter.id,
                    kPlayerViewId,
                    playerViewCount,
                    kPlayerViewId,
                    kPlayerViewId + playerViewCount - 1,
                    sequence,
                    encrypted ? " encrypted" : "");
        std::fflush(stdout);
        return true;
    }

    bool SendPhotonJoinGameResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        const unsigned char* requestPlain,
        int requestPlainSize,
        bool encrypted)
    {
        constexpr unsigned char kPlayerPropertiesParameter = 249;
        constexpr unsigned char kGamePropertiesParameter = 248;
        constexpr unsigned char kActorNumberParameter = 254;
        constexpr unsigned int kServerActorNumber = 1;
        constexpr unsigned int kLocalActorNumber = 2;

        // BOL expects a real master/host actor to exist separately from the local
        // client. Actor 1 is the emulated GameServer/master and actor 2 is BOL.
        // This prevents PhotonTargets.MasterClient RPCs from being executed locally
        // against ClientSession (whose host-session accessor is intentionally null).

        // NetworkingPeer consumes these three parameters unconditionally on a
        // successful GameServer JoinGame response. Preserve the exact room and
        // actor properties BOL sent in the request so its Room/PhotonPlayer
        // models keep AccountId, CharacterId, playlist, difficulty, etc.
        std::vector<unsigned char> actorProperties;
        std::vector<unsigned char> roomProperties;
        const bool haveActorProperties = ExtractTopLevelParameterValue(
            requestPlain, requestPlainSize, kPlayerPropertiesParameter, actorProperties);
        const bool haveRoomProperties = ExtractTopLevelParameterValue(
            requestPlain, requestPlainSize, kGamePropertiesParameter, roomProperties);

        std::vector<unsigned char> plain;
        plain.reserve(96 + actorProperties.size() + roomProperties.size());
        plain.push_back(operationCode);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // null DebugMessage
        AppendU16BE(plain, 3);

        // Response 249 is Hashtable<int actorNr, Hashtable actorProperties>.
        // Include the emulated server/master first, then the local client.
        plain.push_back(kPlayerPropertiesParameter);
        AppendProtocol16HashtableHeader(plain, 2);
        AppendProtocol16TypedInt(plain, kServerActorNumber);
        AppendProtocol16ServerActorPropertiesValue(plain);
        AppendProtocol16TypedInt(plain, kLocalActorNumber);
        if (haveActorProperties)
            plain.insert(plain.end(), actorProperties.begin(), actorProperties.end());
        else
            AppendEmptyHashtableValue(plain);

        if (haveRoomProperties)
            AppendProtocol16EncodedParameter(plain, kGamePropertiesParameter, roomProperties);
        else
            AppendProtocol16EmptyHashtableParameter(plain, kGamePropertiesParameter);

        AppendProtocol16IntParameter(plain, kActorNumberParameter, kLocalActorNumber);

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/UDP] JoinGame response encryption failed\n");
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
        const unsigned int responseSequence = GetPhotonSession(remote).serverReliableSequenceCh0++;
        AppendPhotonCommandHeader(
            reply, 6, 0, 1, 4,
            static_cast<unsigned int>(12 + message.size()), responseSequence);
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
            std::printf("[PHOTON/UDP] JoinGame response send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        PrintHex("[PHOTON/UDP] JoinGame response plaintext", plain.data(), static_cast<int>(plain.size()));
        PrintHex("[PHOTON/UDP] -> JOIN_GAME_RESPONSE", reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] JoinGame success: room=LocalGame masterActor=1 localActor=2 actors=[1,2] ch=0 seq=%u%s\n",
                    responseSequence, encrypted ? " encrypted" : "");
        if (haveRoomProperties && haveActorProperties)
            std::printf("[PHOTON/UDP] JoinGame mirrored room properties (%u byte(s)) and local actor properties (%u byte(s))\n",
                        static_cast<unsigned int>(roomProperties.size()),
                        static_cast<unsigned int>(actorProperties.size()));
        else
            std::printf("[PHOTON/UDP] WARNING: JoinGame property mirror incomplete: room=%s actor=%s\n",
                        haveRoomProperties ? "ok" : "missing",
                        haveActorProperties ? "ok" : "missing");

        // PUN does not invoke OnJoinedRoom from op 226 itself. Its event-255
        // handler does so when ActorNr matches the local actor, and expects an
        // Int32[] actor list. Emit that server-side join event immediately.
        if (!SendPhotonJoinEvent(
                server, remote, remoteLength, receivedSentTime, challenge, actorProperties, encrypted))
        {
            std::printf("[PHOTON/UDP] JoinGame response sent but Join event 255 failed\n");
            std::fflush(stdout);
            return false;
        }

        // The next BOL-side gate is PostJoinAsyncTask, which waits until a
        // networked Session prefab has been instantiated. Send the exact PUN
        // instantiate event shape NetworkingPeer consumes (event 202).
        if (!SendPhotonSessionInstantiateEvent(
                server, remote, remoteLength, receivedSentTime, challenge, encrypted))
        {
            std::printf("[PHOTON/UDP] JoinGame/Join event succeeded but Session instantiate event 202 failed\n");
            std::fflush(stdout);
            return false;
        }

        std::fflush(stdout);
        return true;
    }

}
