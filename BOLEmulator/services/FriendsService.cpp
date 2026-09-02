#include "../EmulatorShared.h"

namespace bolemu
{
    bool SendPhotonFriendsListResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        bool encrypted)
    {
        // Assembly-CSharp.dll -> OperationCode.RetrieveFriendsList = 207 (0xCF).
        // FriendsListsService::OnRetrieveFriendsList allocates the returned
        // friend-id array from response.Parameters.Count and then enumerates
        // each response parameter as a friend Hashtable. A new local account
        // therefore needs ZERO response parameters, which means an empty
        // friends list without inventing any remote player records.
        const bool ok = SendPhotonEmptySuccessResponse(
            server,
            remote,
            remoteLength,
            receivedSentTime,
            challenge,
            operationCode,
            encrypted,
            "GET_FRIENDS_LIST");

        if (ok)
        {
            std::printf("[PHOTON/UDP] RetrieveFriendsList success: empty local friends list for character id=%u\n",
                        g_localCharacter.id);
            std::printf("[PHOTON/UDP] waiting for the next post-friends BOL operation\n");
            std::fflush(stdout);
        }
        return ok;
    }
}
