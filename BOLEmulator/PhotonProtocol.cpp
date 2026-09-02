#include "EmulatorShared.h"

namespace bolemu
{
    unsigned short ReadU16BE(const unsigned char* p)
    {
        return static_cast<unsigned short>((static_cast<unsigned int>(p[0]) << 8) |
                                           static_cast<unsigned int>(p[1]));
    }

    unsigned int ReadU32BE(const unsigned char* p)
    {
        return (static_cast<unsigned int>(p[0]) << 24) |
               (static_cast<unsigned int>(p[1]) << 16) |
               (static_cast<unsigned int>(p[2]) << 8) |
               static_cast<unsigned int>(p[3]);
    }

    void AppendU16BE(std::vector<unsigned char>& out, unsigned short value)
    {
        out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
        out.push_back(static_cast<unsigned char>(value & 0xFF));
    }

    void AppendU32BE(std::vector<unsigned char>& out, unsigned int value)
    {
        out.push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
        out.push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
        out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
        out.push_back(static_cast<unsigned char>(value & 0xFF));
    }

    void AppendProtocol16TypedInt(std::vector<unsigned char>& out, unsigned int value)
    {
        out.push_back(0x69); // GpType.Int32
        AppendU32BE(out, value);
    }

    void AppendProtocol16TypedByte(std::vector<unsigned char>& out, unsigned char value)
    {
        out.push_back(0x62); // GpType.Byte
        out.push_back(value);
    }

    void AppendProtocol16TypedString(std::vector<unsigned char>& out, const std::string& value)
    {
        out.push_back(0x73); // GpType.String
        AppendU16BE(out, static_cast<unsigned short>(value.size()));
        out.insert(out.end(), value.begin(), value.end());
    }

    void AppendProtocol16TypedNull(std::vector<unsigned char>& out)
    {
        out.push_back(0x2A); // GpType.Null
    }

    void AppendProtocol16EmptyHashtableArray(std::vector<unsigned char>& out)
    {
        // Photon Protocol16 generic Array ('y' / 0x79). The managed callback
        // casts parameter 3 directly to Hashtable[], so preserve the element type.
        out.push_back(0x79); // GpType.Array
        AppendU16BE(out, 0); // zero elements
        out.push_back(0x68); // element type = Hashtable
    }

    void AppendProtocol16HashtableHeader(std::vector<unsigned char>& out, unsigned short count)
    {
        out.push_back(0x68); // GpType.Hashtable
        AppendU16BE(out, count);
    }

    void AppendProtocol16HashtableIntKey(std::vector<unsigned char>& out, unsigned int key)
    {
        AppendProtocol16TypedInt(out, key);
    }

    void AppendProtocol16IntEntry(std::vector<unsigned char>& out, unsigned int key, unsigned int value)
    {
        AppendProtocol16HashtableIntKey(out, key);
        AppendProtocol16TypedInt(out, value);
    }

    void AppendProtocol16ByteEntry(std::vector<unsigned char>& out, unsigned int key, unsigned char value)
    {
        AppendProtocol16HashtableIntKey(out, key);
        AppendProtocol16TypedByte(out, value);
    }

    void AppendProtocol16StringEntry(std::vector<unsigned char>& out, unsigned int key, const std::string& value)
    {
        AppendProtocol16HashtableIntKey(out, key);
        AppendProtocol16TypedString(out, value);
    }

    void AppendProtocol16NullEntry(std::vector<unsigned char>& out, unsigned int key)
    {
        AppendProtocol16HashtableIntKey(out, key);
        AppendProtocol16TypedNull(out);
    }

    void AppendEmptyHashtableValue(std::vector<unsigned char>& out)
    {
        AppendProtocol16HashtableHeader(out, 0);
    }

    void AppendAmmoPairHashtableValue(std::vector<unsigned char>& out)
    {
        AppendProtocol16HashtableHeader(out, 2);
        AppendProtocol16IntEntry(out, 0, 0);
        AppendProtocol16IntEntry(out, 1, 0);
    }

    void AppendInventoryHashtableValue(std::vector<unsigned char>& out)
    {
        // Exact new-character defaults reconstructed from
        // CharacterServiceOffline::CreateCharacter in Assembly-CSharp.dll.
        AppendProtocol16HashtableHeader(out, 15);
        AppendProtocol16IntEntry(out, 0, 0);
        AppendProtocol16ByteEntry(out, 1, 4);
        AppendProtocol16ByteEntry(out, 2, 0);

        AppendProtocol16HashtableIntKey(out, 3);
        AppendProtocol16HashtableHeader(out, 4);
        for (unsigned int slot = 0; slot < 4; ++slot)
        {
            AppendProtocol16HashtableIntKey(out, slot);
            AppendProtocol16TypedNull(out);
        }

        for (unsigned int key = 4; key <= 8; ++key)
        {
            AppendProtocol16HashtableIntKey(out, key);
            AppendAmmoPairHashtableValue(out);
        }

        AppendProtocol16IntEntry(out, 9, 0);
        AppendProtocol16HashtableIntKey(out, 10);
        AppendEmptyHashtableValue(out);
        AppendProtocol16NullEntry(out, 11);
        AppendProtocol16HashtableIntKey(out, 12);
        AppendAmmoPairHashtableValue(out);
        AppendProtocol16NullEntry(out, 13);
        AppendProtocol16NullEntry(out, 14);
    }

    void AppendCurrencyHashtableValue(std::vector<unsigned char>& out)
    {
        AppendProtocol16HashtableHeader(out, 6);
        AppendProtocol16IntEntry(out, 0, 0);
        AppendProtocol16IntEntry(out, 1, 500);
        AppendProtocol16IntEntry(out, 2, 0);
        AppendProtocol16IntEntry(out, 3, 0);
        AppendProtocol16HashtableIntKey(out, 4);
        AppendEmptyHashtableValue(out);
        AppendProtocol16HashtableIntKey(out, 5);
        AppendEmptyHashtableValue(out);
    }

    void AppendSkillTreeHashtableValue(std::vector<unsigned char>& out)
    {
        AppendProtocol16HashtableHeader(out, 4);
        AppendProtocol16IntEntry(out, 0, 0);
        AppendProtocol16IntEntry(out, 1, 24);
        AppendProtocol16IntEntry(out, 2, 24);
        AppendProtocol16HashtableIntKey(out, 3);
        AppendEmptyHashtableValue(out);
    }

    void AppendCharacterHashtableValue(std::vector<unsigned char>& out, const LocalCharacterState& character)
    {
        // Character::.ctor(Hashtable) consumes integer keys 0..14. Match the
        // game's own CharacterServiceOffline defaults so all dependent model
        // objects (inventory/currency/quests/instances/skills) can initialize.
        AppendProtocol16HashtableHeader(out, 15);
        AppendProtocol16IntEntry(out, 0, character.id);
        AppendProtocol16IntEntry(out, 1, character.accountId);
        AppendProtocol16StringEntry(out, 2, character.name);
        AppendProtocol16IntEntry(out, 3, character.experience);
        AppendProtocol16IntEntry(out, 4, character.level);
        AppendProtocol16IntEntry(out, 5, character.avatarType);
        AppendProtocol16StringEntry(out, 6, character.avatarParts);
        AppendProtocol16StringEntry(out, 7, character.avatarMaterials);
        AppendProtocol16IntEntry(out, 8, character.avatarIdBits);

        AppendProtocol16HashtableIntKey(out, 9);
        AppendInventoryHashtableValue(out);
        AppendProtocol16HashtableIntKey(out, 10);
        AppendCurrencyHashtableValue(out);
        AppendProtocol16HashtableIntKey(out, 11);
        AppendEmptyHashtableValue(out);
        AppendProtocol16HashtableIntKey(out, 12);
        AppendEmptyHashtableValue(out);
        AppendProtocol16HashtableIntKey(out, 13);
        AppendSkillTreeHashtableValue(out);
        AppendProtocol16IntEntry(out, 14, character.timePlayed);
    }

    bool ReadProtocol16TypedString(const unsigned char* data, int size, int& offset, std::string& value)
    {
        if (!data || offset + 3 > size || data[offset++] != 0x73)
            return false;
        const unsigned short length = ReadU16BE(data + offset);
        offset += 2;
        if (offset + length > size)
            return false;
        value.assign(reinterpret_cast<const char*>(data + offset), length);
        offset += length;
        return true;
    }

    bool ReadProtocol16TypedInt(const unsigned char* data, int size, int& offset, unsigned int& value)
    {
        if (!data || offset + 5 > size || data[offset++] != 0x69)
            return false;
        value = ReadU32BE(data + offset);
        offset += 4;
        return true;
    }

    bool ParseCreateCharacterRequest(const unsigned char* plain, int plainSize, LocalCharacterState& character)
    {
        if (!plain || plainSize < 8 || plain[0] != 82)
            return false;

        const unsigned short parameterCount = ReadU16BE(plain + 1);
        int offset = 3;
        for (unsigned int parameter = 0; parameter < parameterCount && offset + 2 <= plainSize; ++parameter)
        {
            const unsigned char key = plain[offset++];
            const unsigned char type = plain[offset++];
            if (key != 112 || type != 0x7A || offset + 2 > plainSize)
                return false;

            const unsigned short objectCount = ReadU16BE(plain + offset);
            offset += 2;
            if (objectCount < 5)
                return false;

            LocalCharacterState parsed;
            parsed.exists = true;
            if (!ReadProtocol16TypedString(plain, plainSize, offset, parsed.name))
                return false;
            if (!ReadProtocol16TypedInt(plain, plainSize, offset, parsed.avatarType))
                return false;

            std::string avatarId;
            if (!ReadProtocol16TypedString(plain, plainSize, offset, avatarId))
                return false;
            char* end = nullptr;
            const unsigned long long avatarBits = std::strtoull(avatarId.c_str(), &end, 10);
            if (!end || *end != '\0')
                return false;
            parsed.avatarIdBits = static_cast<unsigned int>(avatarBits & 0xFFFFFFFFull);

            if (!ReadProtocol16TypedString(plain, plainSize, offset, parsed.avatarParts))
                return false;
            if (!ReadProtocol16TypedString(plain, plainSize, offset, parsed.avatarMaterials))
                return false;

            if (parsed.name.empty())
                return false;

            character = parsed;
            return true;
        }
        return false;
    }

    void AppendPhotonHeader(
        std::vector<unsigned char>& out,
        unsigned char commandCount,
        unsigned int sentTime,
        unsigned int challenge)
    {
        // Photon 3.2 UDP packet header: peerId, flags, commandCount, time, challenge.
        // The client ignores the header peer id while connecting; the assigned peer id
        // is carried by VERIFY_CONNECT below.
        AppendU16BE(out, 0);
        out.push_back(0);
        out.push_back(commandCount);
        AppendU32BE(out, sentTime);
        AppendU32BE(out, challenge);
    }

    void AppendPhotonHeaderForPeer(
        std::vector<unsigned char>& out,
        unsigned short peerId,
        unsigned char commandCount,
        unsigned int sentTime,
        unsigned int challenge)
    {
        AppendU16BE(out, peerId);
        out.push_back(0);
        out.push_back(commandCount);
        AppendU32BE(out, sentTime);
        AppendU32BE(out, challenge);
    }

    void AppendPhotonCommandHeader(
        std::vector<unsigned char>& out,
        unsigned char type,
        unsigned char channel,
        unsigned char flags,
        unsigned char reserved,
        unsigned int size,
        unsigned int reliableSequence)
    {
        out.push_back(type);
        out.push_back(channel);
        out.push_back(flags);
        out.push_back(reserved);
        AppendU32BE(out, size);
        AppendU32BE(out, reliableSequence);
    }

    bool SendPhotonAck(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned char channel,
        unsigned int reliableSequence,
        unsigned int receivedSentTime,
        unsigned int challenge)
    {
        std::vector<unsigned char> reply;
        reply.reserve(32);
        AppendPhotonHeader(reply, 1, receivedSentTime, challenge);
        AppendPhotonCommandHeader(reply, 1, channel, 0, 4, 20, 0);
        AppendU32BE(reply, reliableSequence);
        AppendU32BE(reply, receivedSentTime);

        const int sent = sendto(
            server,
            reinterpret_cast<const char*>(reply.data()),
            static_cast<int>(reply.size()),
            0,
            reinterpret_cast<const sockaddr*>(&remote),
            remoteLength);

        if (sent != static_cast<int>(reply.size()))
        {
            std::printf("[PHOTON/UDP] ACK send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        std::printf("[PHOTON/UDP] -> ACK ch=%u seq=%u\n",
                    static_cast<unsigned int>(channel), reliableSequence);
        std::fflush(stdout);
        return true;
    }

    bool SendPhotonVerifyConnect(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        const unsigned char* connectPayload,
        int payloadSize,
        unsigned int receivedSentTime,
        unsigned int challenge)
    {
        if (!connectPayload || payloadSize < 32)
            return false;

        std::vector<unsigned char> reply;
        reply.reserve(56);
        AppendPhotonHeader(reply, 1, receivedSentTime, challenge);

        // Photon3Unity3D 3.2.0.1 uses a 44-byte CONNECT and 44-byte
        // VERIFY_CONNECT command. The 32-byte body is the negotiated transport
        // settings. Echo the client's settings and replace the first 16-bit field
        // with the locally assigned peer id.
        AppendPhotonCommandHeader(reply, 3, 0xFF, 1, 4, 44, 1);
        AppendU16BE(reply, 1); // assigned local peer id
        reply.insert(reply.end(), connectPayload + 2, connectPayload + 32);

        const int sent = sendto(
            server,
            reinterpret_cast<const char*>(reply.data()),
            static_cast<int>(reply.size()),
            0,
            reinterpret_cast<const sockaddr*>(&remote),
            remoteLength);

        if (sent != static_cast<int>(reply.size()))
        {
            std::printf("[PHOTON/UDP] VERIFY_CONNECT send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        PrintHex("[PHOTON/UDP] -> VERIFY_CONNECT", reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] transport connected: assigned peer=1; waiting for Photon init request\n");
        std::fflush(stdout);
        return true;
    }

    bool SendPhotonInitResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge)
    {
        std::vector<unsigned char> reply;
        reply.reserve(31);

        // Established packets use the peer id negotiated by VERIFY_CONNECT.
        AppendPhotonHeaderForPeer(reply, 1, 1, receivedSentTime, challenge);

        // Photon documents the legacy server INIT_RESPONSE eNet command as 19
        // bytes total: 12-byte SEND_RELIABLE command + 7-byte RTS payload.
        // GpBinary v1.6 uses F3 as the message magic and message type 1 for
        // InitResponse. The 5-byte response object is Protocol16 Int32(0):
        // 'i' (0x69) followed by the big-endian zero value.
        const unsigned int sequence = GetPhotonSession(remote).serverReliableSequenceCh0++;
        AppendPhotonCommandHeader(reply, 6, 0, 1, 4, 19, sequence);
        reply.push_back(0xF3);
        reply.push_back(0x01);
        reply.push_back(0x69);
        AppendU32BE(reply, 0);

        const int sent = sendto(
            server,
            reinterpret_cast<const char*>(reply.data()),
            static_cast<int>(reply.size()),
            0,
            reinterpret_cast<const sockaddr*>(&remote),
            remoteLength);

        if (sent != static_cast<int>(reply.size()))
        {
            std::printf("[PHOTON/UDP] INIT_RESPONSE send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        PrintHex("[PHOTON/UDP] -> INIT_RESPONSE", reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] application init response sent ch=0 seq=%u; waiting for first operation\n", sequence);
        std::fflush(stdout);
        return true;
    }

    bool SendPhotonEncryptionResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge)
    {
        std::vector<unsigned char> message;
        message.reserve(110);

        // Photon 3 / GpBinary v1.6 internal operation response.
        // The client request we captured is:
        //   F3 06 00 00 01 01 78 00000060 <96-byte client public key>
        // which is InternalOperationRequest op=0 (InitEncryption), parameter 1
        // (ClientKey), byte[96]. The matching response is message type 7,
        // op=0, ReturnCode=0, null DebugMessage, one parameter 1 (ServerKey).
        //
        // For this capture stage use the valid DH value 1 as the server public
        // value. With the old 768-bit Oakley group this yields a deterministic
        // shared secret on the client and, most importantly, lets us confirm the
        // exact post-key-exchange wire format before adding payload decryption.
        message.push_back(0xF3); // GpBinary v2 magic
        message.push_back(0x07); // InternalOperationResponse
        message.push_back(0x00); // InitEncryption operation code
        AppendU16BE(message, 0); // ReturnCode = OK
        message.push_back(0x2A); // GpType.Null DebugMessage
        AppendU16BE(message, 1); // parameter count
        message.push_back(0x01); // PhotonCodes.ServerKey
        message.push_back(0x78); // GpType.ByteArray
        AppendU32BE(message, 96);
        for (int i = 0; i < 95; ++i)
            message.push_back(0);
        message.push_back(1);

        std::vector<unsigned char> reply;
        reply.reserve(12 + 12 + message.size());
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
            std::printf("[PHOTON/UDP] encryption response send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        PrintHex("[PHOTON/UDP] -> INTERNAL_ENCRYPTION_RESPONSE", reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] key exchange response sent ch=0 seq=%u; server public value=1\n", sequence);
        std::printf("[PHOTON/UDP] waiting for the first post-encryption application message\n");
        std::fflush(stdout);
        return true;
    }



    void AppendProtocol16StringParameter(
        std::vector<unsigned char>& plain,
        unsigned char key,
        const std::string& value)
    {
        plain.push_back(key);
        plain.push_back(0x73); // GpType.String
        AppendU16BE(plain, static_cast<unsigned short>(value.size()));
        plain.insert(plain.end(), value.begin(), value.end());
    }

    bool LooksLikeOnlinePeerAuthenticate(const unsigned char* plain, int plainSize)
    {
        if (!plain || plainSize < 5 || plain[0] != 230)
            return false;

        // The initial NetworkingPeer Authenticate contains username/password keys
        // (104/109). OnlineServerPeer's second Authenticate instead contains the
        // master-issued account id (110) and token (221). Search for the typed
        // key markers so dictionary serialization order does not matter.
        bool hasAccountId = false;
        bool hasToken = false;
        for (int i = 3; i + 1 < plainSize; ++i)
        {
            if (plain[i] == 110 && plain[i + 1] == 0x69) // Int32
                hasAccountId = true;
            if (plain[i] == 221 && plain[i + 1] == 0x73) // String
                hasToken = true;
        }
        return hasAccountId || hasToken;
    }


    bool LooksLikeGameServerAuthenticate(const unsigned char* plain, int plainSize)
    {
        if (!plain || plainSize < 9 || plain[0] != 230)
            return false;

        // BOL reuses the token/account Authenticate on the GameServer, but its
        // parameter 0 changes from 0 on OnlineServerPeer to 1 on GameServerPeer.
        for (int i = 3; i + 5 < plainSize; ++i)
        {
            if (plain[i] == 0 && plain[i + 1] == 0x69 && ReadU32BE(plain + i + 2) == 1)
                return true;
        }
        return false;
    }

}
