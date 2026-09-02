#pragma once

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace bolemu
{
    inline constexpr unsigned short kListenPort = 80;
    inline constexpr unsigned short kPhotonPort = 5055;
    inline constexpr const char* kUsername = "admin";
    inline constexpr const char* kPassword = "admin";

    struct PhotonFragmentState
    {
        bool active = false;
        unsigned int startSequence = 0;
        unsigned int fragmentCount = 0;
        unsigned int totalLength = 0;
        unsigned int receivedCount = 0;
        std::vector<unsigned char> data;
        std::vector<unsigned char> received;
    };

    struct PhotonSessionState
    {
        unsigned int serverReliableSequenceCh0 = 1;
        unsigned int serverReliableSequenceCh255 = 2;
        bool initResponseSent = false;
        bool encryptionResponseSent = false;
        bool authResponseSent = false;
        bool joinLobbyResponseSent = false;
        bool joinRandomGameResponseSent = false;
        bool joinGameResponseSent = false;
        bool characterListResponseSent = false;
        bool onlinePeer = false;
        bool gameServerPeer = false;
        PhotonFragmentState fragments;
    };

    struct LocalCharacterState
    {
        bool exists = false;
        unsigned int id = 1;
        unsigned int accountId = 100000001u;
        std::string name;
        unsigned int experience = 0;
        unsigned int level = 0;
        unsigned int avatarType = 0;
        std::string avatarParts = "0,0,0,0,0,0,0,0";
        std::string avatarMaterials = "0,0,0,0,0,0,0,0";
        unsigned int avatarIdBits = 0;
        unsigned int timePlayed = 0;
    };

    extern LocalCharacterState g_localCharacter;
    extern std::map<unsigned long long, PhotonSessionState> g_photonSessions;

    unsigned long long PhotonSessionKey(const sockaddr_in& remote);
    PhotonSessionState& GetPhotonSession(const sockaddr_in& remote);
    std::string CharacterStatePath();
    bool SaveLocalCharacter();
    bool LoadLocalCharacter();
    void PrintHex(const char* prefix, const unsigned char* data, int size);

    void HandleClient(SOCKET client);

    bool PhotonDecrypt(const unsigned char* input, int inputSize, std::vector<unsigned char>& plain);
    bool PhotonEncrypt(const unsigned char* input, int inputSize, std::vector<unsigned char>& cipher);
    DWORD WINAPI PhotonTcpThread(void*);
    DWORD WINAPI PhotonUdpThread(void*);

    unsigned short ReadU16BE(const unsigned char* p);
    unsigned int ReadU32BE(const unsigned char* p);
    void AppendU16BE(std::vector<unsigned char>& out, unsigned short value);
    void AppendU32BE(std::vector<unsigned char>& out, unsigned int value);
    void AppendProtocol16TypedInt(std::vector<unsigned char>& out, unsigned int value);
    void AppendProtocol16TypedByte(std::vector<unsigned char>& out, unsigned char value);
    void AppendProtocol16TypedString(std::vector<unsigned char>& out, const std::string& value);
    void AppendProtocol16TypedNull(std::vector<unsigned char>& out);
    void AppendProtocol16EmptyHashtableArray(std::vector<unsigned char>& out);
    void AppendProtocol16HashtableHeader(std::vector<unsigned char>& out, unsigned short count);
    void AppendProtocol16HashtableIntKey(std::vector<unsigned char>& out, unsigned int key);
    void AppendProtocol16IntEntry(std::vector<unsigned char>& out, unsigned int key, unsigned int value);
    void AppendProtocol16ByteEntry(std::vector<unsigned char>& out, unsigned int key, unsigned char value);
    void AppendProtocol16StringEntry(std::vector<unsigned char>& out, unsigned int key, const std::string& value);
    void AppendProtocol16NullEntry(std::vector<unsigned char>& out, unsigned int key);
    void AppendEmptyHashtableValue(std::vector<unsigned char>& out);
    void AppendAmmoPairHashtableValue(std::vector<unsigned char>& out);
    void AppendInventoryHashtableValue(std::vector<unsigned char>& out);
    void AppendCurrencyHashtableValue(std::vector<unsigned char>& out);
    void AppendSkillTreeHashtableValue(std::vector<unsigned char>& out);
    void AppendCharacterHashtableValue(std::vector<unsigned char>& out, const LocalCharacterState& character);
    bool ReadProtocol16TypedString(const unsigned char* data, int size, int& offset, std::string& value);
    bool ReadProtocol16TypedInt(const unsigned char* data, int size, int& offset, unsigned int& value);
    bool ParseCreateCharacterRequest(const unsigned char* plain, int plainSize, LocalCharacterState& character);

    void AppendPhotonHeader(std::vector<unsigned char>& out, unsigned char commandCount, unsigned int sentTime, unsigned int challenge);
    void AppendPhotonHeaderForPeer(std::vector<unsigned char>& out, unsigned short peerId, unsigned char commandCount, unsigned int sentTime, unsigned int challenge);
    void AppendPhotonCommandHeader(std::vector<unsigned char>& out, unsigned char type, unsigned char channel, unsigned char flags, unsigned char reserved, unsigned int commandSize, unsigned int reliableSequence);
    bool SendPhotonAck(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned char channel, unsigned int reliableSequence, unsigned int receivedSentTime, unsigned int challenge);
    bool SendPhotonVerifyConnect(SOCKET server, const sockaddr_in& remote, int remoteLength, const unsigned char* connectPayload, int payloadSize, unsigned int receivedSentTime, unsigned int challenge);
    bool SendPhotonInitResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge);
    bool SendPhotonEncryptionResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge);
    void AppendProtocol16StringParameter(std::vector<unsigned char>& out, unsigned char key, const std::string& value);
    bool LooksLikeOnlinePeerAuthenticate(const unsigned char* plain, int plainSize);
    bool LooksLikeGameServerAuthenticate(const unsigned char* plain, int plainSize);

    bool SendPhotonAuthenticateResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, unsigned char operationCode, bool onlinePeerAuthenticate);
    bool SendPhotonCharacterListResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, unsigned char operationCode, bool encrypted);
    bool SendPhotonCreateCharacterResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, const unsigned char* plain, int plainSize, bool encrypted);
    bool SendPhotonSingleCharacterResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, unsigned char operationCode, bool encrypted, const char* label);
    bool SendPhotonPersonalSettingsResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, unsigned char operationCode, bool encrypted);
    bool SendPhotonPresenceResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, unsigned char operationCode, bool encrypted);
    bool SendPhotonInventoryResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, unsigned char operationCode, bool encrypted);
    bool SendPhotonWarehouseResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, unsigned char operationCode, bool encrypted);
    bool SendPhotonDailyQuestContentsResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, unsigned char operationCode, bool encrypted);
    bool SendPhotonPlayerQuestsResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, unsigned char operationCode, bool encrypted);
    bool SendPhotonFriendsListResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, unsigned char operationCode, bool encrypted);
    bool SendPhotonGroupInfoResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, unsigned char operationCode, bool encrypted);
    bool SendPhotonMyGuildIdResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, unsigned char operationCode, bool encrypted);
    bool SendPhotonMySkillPointsResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, unsigned char operationCode, bool encrypted);
    bool SendPhotonMailBootstrapResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, unsigned char operationCode, bool encrypted);
    const char* LookupBOLOperationName(unsigned char operationCode);
    const char* LookupBOLServiceName(unsigned char operationCode);
    bool SendPhotonEmptySuccessResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, unsigned char operationCode, bool encrypted, const char* label);
    bool SendPhotonJoinLobbyResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, unsigned char operationCode, bool encrypted);
    bool SendPhotonJoinRandomGameResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, unsigned char operationCode, const unsigned char* requestPlain, int requestPlainSize, bool encrypted);
    bool SendPhotonJoinGameResponse(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, unsigned char operationCode, const unsigned char* requestPlain, int requestPlainSize, bool encrypted);
    unsigned int GetSessionViewCountCandidate();
    unsigned int AdvanceSessionViewCountCandidate();
    void ConfirmSessionViewCountCandidate();
    bool RetryPhotonSessionInstantiateEvent(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, bool encrypted);
    unsigned int GetPlayerViewCountCandidate();
    unsigned int AdvancePlayerViewCountCandidate();
    void ConfirmPlayerViewCountCandidate();
    bool SendPhotonPlayerInstantiateEvent(SOCKET server, const sockaddr_in& remote, int remoteLength, unsigned int receivedSentTime, unsigned int challenge, bool encrypted);
}
