#include "../EmulatorShared.h"

namespace bolemu
{
    bool SendPhotonInventoryResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        bool encrypted)
    {
        std::vector<unsigned char> plain;
        plain.reserve(96);
        plain.push_back(operationCode);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // null DebugMessage
        AppendU16BE(plain, 1); // one response parameter
        plain.push_back(0x00); // key 0: Inventory Hashtable
        // Inventory::.ctor directly consumes keys 0..14, including ammo key
        // 12 and nullable equipment keys 11/13/14. Use the already-recovered
        // complete new-character schema rather than the old truncated 0..10 map.
        AppendInventoryHashtableValue(plain);

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/UDP] GET_INVENTORY response encryption failed\n");
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
            std::printf("[PHOTON/UDP] GET_INVENTORY response send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        PrintHex("[PHOTON/UDP] inventory response plaintext",
                 plain.data(), static_cast<int>(plain.size()));
        PrintHex("[PHOTON/UDP] -> GET_INVENTORY_RESPONSE",
                 reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] GetInventory success: complete 15-key starter inventory for character id=%u ch=0 seq=%u%s\n",
                    g_localCharacter.id, sequence, encrypted ? " encrypted" : "");
        std::printf("[PHOTON/UDP] waiting for the next post-inventory BOL operation\n");
        std::fflush(stdout);
        return true;
    }

    bool SendPhotonWarehouseResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        bool encrypted)
    {
        // Assembly-CSharp.dll -> InventoryService::.ctor registers operation 99
        // (0x63) with InventoryService::OnNewWarehouse. OnNewWarehouse reads
        // response parameter 0 as a Hashtable and constructs Warehouse from it.
        // Warehouse::.ctor consumes exactly:
        //   key 0 = account id (Int32)
        //   key 1 = max warehouse slots (Int32, default 60)
        //   key 2 = item Hashtable
        std::vector<unsigned char> plain;
        plain.reserve(64);
        plain.push_back(operationCode);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // null DebugMessage
        AppendU16BE(plain, 1); // one response parameter
        plain.push_back(0x00); // key 0: Warehouse Hashtable

        AppendProtocol16HashtableHeader(plain, 3);
        AppendProtocol16IntEntry(plain, 0, static_cast<int>(g_localCharacter.accountId));
        AppendProtocol16IntEntry(plain, 1, 60);
        AppendProtocol16HashtableIntKey(plain, 2);
        AppendEmptyHashtableValue(plain);

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/UDP] GET_WAREHOUSE response encryption failed\n");
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
            std::printf("[PHOTON/UDP] GET_WAREHOUSE response send failed: %d\n", WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        PrintHex("[PHOTON/UDP] warehouse response plaintext",
                 plain.data(), static_cast<int>(plain.size()));
        PrintHex("[PHOTON/UDP] -> GET_WAREHOUSE_RESPONSE",
                 reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] GetWarehouse success: accountId=%u slots=60 empty items ch=0 seq=%u%s\n",
                    g_localCharacter.accountId, sequence, encrypted ? " encrypted" : "");
        std::printf("[PHOTON/UDP] waiting for the next post-warehouse BOL operation\n");
        std::fflush(stdout);
        return true;
    }

}
