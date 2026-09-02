#include "../EmulatorShared.h"

namespace bolemu
{
    bool SendPhotonMailBootstrapResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        bool encrypted)
    {
        // MailService::OnGotMail does not read operation response parameters.
        // Mail list contents arrive through event 161, so an empty success is
        // enough for the request-side bootstrap.
        const bool ok = SendPhotonEmptySuccessResponse(
            server, remote, remoteLength, receivedSentTime, challenge,
            operationCode, encrypted, "GET_MAIL");
        if (ok)
        {
            std::printf("[PHOTON/UDP] GetMail bootstrap success: empty local mailbox\n");
            std::fflush(stdout);
        }
        return ok;
    }
}
