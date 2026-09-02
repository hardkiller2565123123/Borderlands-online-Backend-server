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

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Bcrypt.lib")

namespace
{
    constexpr unsigned short kListenPort = 80;
    constexpr unsigned short kPhotonPort = 5055;
    constexpr const char* kUsername = "admin";
    constexpr const char* kPassword = "admin";

    struct PhotonSessionState
    {
        unsigned int serverReliableSequenceCh0 = 1;
        bool initResponseSent = false;
        bool encryptionResponseSent = false;
        bool authResponseSent = false;
        bool joinLobbyResponseSent = false;
        bool characterListResponseSent = false;
        bool onlinePeer = false;
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

    LocalCharacterState g_localCharacter;

    // BOL uses two simultaneous Photon peers: NetworkingPeer (master/lobby) and
    // OnlineServerPeer (BOL account/character services). Keep transport sequence
    // and bootstrap state per UDP source port so the second CONNECT cannot reset
    // the first peer's reliable sequence numbers.
    std::map<unsigned long long, PhotonSessionState> g_photonSessions;

    unsigned long long PhotonSessionKey(const sockaddr_in& remote)
    {
        return (static_cast<unsigned long long>(remote.sin_addr.s_addr) << 16) |
               static_cast<unsigned long long>(ntohs(remote.sin_port));
    }

    PhotonSessionState& GetPhotonSession(const sockaddr_in& remote)
    {
        return g_photonSessions[PhotonSessionKey(remote)];
    }

    std::string CharacterStatePath()
    {
        char modulePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
        std::string path(modulePath, modulePath + (length ? length : 0));
        const std::size_t slash = path.find_last_of("\\/");
        if (slash != std::string::npos)
            path.resize(slash + 1);
        else
            path.clear();
        path += "bol_local_character.dat";
        return path;
    }

    bool WriteStateString(FILE* file, const std::string& value)
    {
        const unsigned int length = static_cast<unsigned int>(value.size());
        return std::fwrite(&length, sizeof(length), 1, file) == 1 &&
               (length == 0 || std::fwrite(value.data(), 1, length, file) == length);
    }

    bool ReadStateString(FILE* file, std::string& value)
    {
        unsigned int length = 0;
        if (std::fread(&length, sizeof(length), 1, file) != 1 || length > 4096)
            return false;
        value.assign(length, '\0');
        return length == 0 || std::fread(&value[0], 1, length, file) == length;
    }

    bool SaveLocalCharacter()
    {
        if (!g_localCharacter.exists)
            return false;

        FILE* file = nullptr;
        if (fopen_s(&file, CharacterStatePath().c_str(), "wb") != 0 || !file)
            return false;

        const unsigned int magic = 0x31434C42u; // BLC1
        const unsigned int version = 1;
        bool ok =
            std::fwrite(&magic, sizeof(magic), 1, file) == 1 &&
            std::fwrite(&version, sizeof(version), 1, file) == 1 &&
            std::fwrite(&g_localCharacter.id, sizeof(g_localCharacter.id), 1, file) == 1 &&
            std::fwrite(&g_localCharacter.accountId, sizeof(g_localCharacter.accountId), 1, file) == 1 &&
            WriteStateString(file, g_localCharacter.name) &&
            std::fwrite(&g_localCharacter.experience, sizeof(g_localCharacter.experience), 1, file) == 1 &&
            std::fwrite(&g_localCharacter.level, sizeof(g_localCharacter.level), 1, file) == 1 &&
            std::fwrite(&g_localCharacter.avatarType, sizeof(g_localCharacter.avatarType), 1, file) == 1 &&
            WriteStateString(file, g_localCharacter.avatarParts) &&
            WriteStateString(file, g_localCharacter.avatarMaterials) &&
            std::fwrite(&g_localCharacter.avatarIdBits, sizeof(g_localCharacter.avatarIdBits), 1, file) == 1 &&
            std::fwrite(&g_localCharacter.timePlayed, sizeof(g_localCharacter.timePlayed), 1, file) == 1;

        std::fclose(file);
        return ok;
    }

    bool LoadLocalCharacter()
    {
        FILE* file = nullptr;
        if (fopen_s(&file, CharacterStatePath().c_str(), "rb") != 0 || !file)
            return false;

        unsigned int magic = 0;
        unsigned int version = 0;
        LocalCharacterState loaded;
        bool ok =
            std::fread(&magic, sizeof(magic), 1, file) == 1 && magic == 0x31434C42u &&
            std::fread(&version, sizeof(version), 1, file) == 1 && version == 1 &&
            std::fread(&loaded.id, sizeof(loaded.id), 1, file) == 1 &&
            std::fread(&loaded.accountId, sizeof(loaded.accountId), 1, file) == 1 &&
            ReadStateString(file, loaded.name) &&
            std::fread(&loaded.experience, sizeof(loaded.experience), 1, file) == 1 &&
            std::fread(&loaded.level, sizeof(loaded.level), 1, file) == 1 &&
            std::fread(&loaded.avatarType, sizeof(loaded.avatarType), 1, file) == 1 &&
            ReadStateString(file, loaded.avatarParts) &&
            ReadStateString(file, loaded.avatarMaterials) &&
            std::fread(&loaded.avatarIdBits, sizeof(loaded.avatarIdBits), 1, file) == 1 &&
            std::fread(&loaded.timePlayed, sizeof(loaded.timePlayed), 1, file) == 1;
        std::fclose(file);

        if (!ok)
            return false;

        loaded.exists = true;
        g_localCharacter = loaded;
        return true;
    }

    std::string ToLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    std::string UrlDecode(const std::string& value)
    {
        std::string out;
        out.reserve(value.size());

        for (std::size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] == '+' )
            {
                out.push_back(' ');
            }
            else if (value[i] == '%' && i + 2 < value.size())
            {
                const char hex[3] = { value[i + 1], value[i + 2], 0 };
                char* end = nullptr;
                const long decoded = std::strtol(hex, &end, 16);
                if (end && *end == '\0')
                {
                    out.push_back(static_cast<char>(decoded));
                    i += 2;
                }
                else
                {
                    out.push_back(value[i]);
                }
            }
            else
            {
                out.push_back(value[i]);
            }
        }

        return out;
    }

    void ParseForm(const std::string& text, std::map<std::string, std::string>& params)
    {
        std::size_t start = 0;
        while (start <= text.size())
        {
            const std::size_t amp = text.find('&', start);
            const std::string part = text.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
            const std::size_t equals = part.find('=');
            if (equals != std::string::npos)
            {
                params[UrlDecode(part.substr(0, equals))] = UrlDecode(part.substr(equals + 1));
            }
            else if (!part.empty())
            {
                params[UrlDecode(part)] = "";
            }

            if (amp == std::string::npos)
                break;
            start = amp + 1;
        }
    }

    std::string RedactPassword(std::string text)
    {
        std::string lower = ToLower(text);
        std::size_t pos = 0;
        while ((pos = lower.find("password=", pos)) != std::string::npos)
        {
            const std::size_t valueStart = pos + std::strlen("password=");
            std::size_t valueEnd = text.find('&', valueStart);
            if (valueEnd == std::string::npos)
                valueEnd = text.size();
            text.replace(valueStart, valueEnd - valueStart, "<redacted>");
            lower = ToLower(text);
            pos = valueStart + std::strlen("<redacted>");
        }
        return text;
    }

    std::string GetParamInsensitive(const std::map<std::string, std::string>& params, const char* name)
    {
        const std::string wanted = ToLower(name);
        for (const auto& [key, value] : params)
        {
            if (ToLower(key) == wanted)
                return value;
        }
        return {};
    }

    bool ValidateKnownCredentials(const std::map<std::string, std::string>& params)
    {
        const char* usernameKeys[] = { "username", "userName", "account", "accountName", "loginName" };
        for (const char* key : usernameKeys)
        {
            const std::string value = GetParamInsensitive(params, key);
            if (!value.empty() && value != kUsername)
                return false;
        }

        const std::string password = GetParamInsensitive(params, "password");
        const std::string encryptFlag = GetParamInsensitive(params, "encryptFlag");

        // loginType 2 uses an encrypted password. Until the matching old CAS crypto is
        // reconstructed, accept encrypted payloads. Plain encryptFlag=0 is enforced.
        if (!password.empty() && (encryptFlag.empty() || encryptFlag == "0"))
            return password == kPassword;

        return true;
    }

    std::string SuccessData(bool includeGuid = true)
    {
        std::string data = "{";
        data += "\"resultCode\":\"0\",";
        if (includeGuid)
        {
            data += "\"guid\":\"LOCAL-GUID-ADMIN\",";
            data += "\"dynamicKey\":\"LOCAL-DYNAMIC-KEY\",";
        }
        data += "\"nextAction\":\"0\",";
        data += "\"tgt\":\"LOCAL-TGT-ADMIN\",";
        data += "\"sessionId\":\"LOCAL-SESSION-ADMIN\",";
        data += "\"ticket\":\"LOCAL-TICKET-ADMIN\",";
        data += "\"sndaId\":\"100000001\",";
        data += "\"clientVKey\":\"LOCAL-CLIENT-VKEY\",";
        data += "\"key\":\"\",";
        data += "\"autoLoginSessionKey\":\"LOCAL-AUTOLOGIN-ADMIN\",";
        data += "\"autoLoginMaxAge\":\"86400\",";
        data += "\"challenge\":\"\",";
        data += "\"deviceDisplayType\":\"0\",";
        data += "\"deviceType\":\"0\",";
        data += "\"redirectURL\":\"\",";
        data += "\"popWindowFlag\":\"0\",";
        data += "\"mobile\":\"\",";
        data += "\"accountUpgradeUrl\":\"\",";
        data += "\"failReason\":\"\"";
        data += "}";
        return data;
    }

    std::string SuccessResponse(bool includeGuid = true)
    {
        return "{\"return_code\":0,\"return_message\":\"\",\"data\":" + SuccessData(includeGuid) + "}";
    }

    std::string FailureResponse()
    {
        return "{\"return_code\":1001,\"return_message\":\"invalid account\",\"data\":{"
               "\"resultCode\":\"1001\","
               "\"nextAction\":\"0\","
               "\"failReason\":\"Invalid username or password\"}}";
    }

    std::string AccountInfoResponse()
    {
        return "{\"return_code\":0,\"return_message\":\"\",\"data\":{"
               "\"resultCode\":\"0\","
               "\"companyId\":\"1\","
               "\"bindPhoneStatus\":\"1\","
               "\"appInstallStatus\":\"1\","
               "\"failReason\":\"\"}}";
    }

    std::string LoginUserInfoResponse()
    {
        return "{\"return_code\":0,\"return_message\":\"\",\"data\":{"
               "\"resultCode\":\"0\","
               "\"inputUserId\":\"admin\","
               "\"displayAccount\":\"admin\","
               "\"sndaId\":\"100000001\","
               "\"failReason\":\"\"}}";
    }

    std::string LoginAreaInfoResponse()
    {
        // SdoBaseClient's area handler explicitly consumes areaList and
        // areaGroupList, then reads areaCode/areaName/groupCode/groupName.
        return "{\"return_code\":0,\"return_message\":\"\",\"data\":{"
               "\"resultCode\":\"0\","
               "\"areaList\":[{\"areaCode\":\"1\",\"areaName\":\"Local\"}],"
               "\"areaGroupList\":[{\"areaCode\":\"1\",\"areaName\":\"Local\",\"groupCode\":\"1\",\"groupName\":\"Local\"}],"
               "\"failReason\":\"\"}}";
    }

    std::string LoginStateResponse()
    {
        return "{\"return_code\":0,\"return_message\":\"\",\"data\":{"
               "\"resultCode\":\"0\","
               "\"failReason\":\"\"}}";
    }

    std::string LoginStatesResponse()
    {
        return "{\"return_code\":0,\"return_message\":\"\",\"data\":{"
               "\"resultCode\":\"0\","
               "\"loginStateArray\":[\"0\"],"
               "\"tgtArray\":[\"LOCAL-TGT-ADMIN\"],"
               "\"failReason\":\"\"}}";
    }

    std::string EmulatorStatusResponse()
    {
        return "{\"status\":\"ok\","
               "\"account\":\"admin\","
               "\"sndaId\":\"100000001\","
               "\"areaCode\":\"1\","
               "\"groupCode\":\"1\","
               "\"ticket\":\"LOCAL-TICKET-ADMIN\","
               "\"sessionId\":\"LOCAL-SESSION-ADMIN\"}";
    }

    std::string RouteRequest(
        const std::string& method,
        const std::string& target,
        const std::string& body,
        bool& knownEndpoint)
    {
        knownEndpoint = true;

        std::string path = target;
        std::string query;
        const std::size_t question = path.find('?');
        if (question != std::string::npos)
        {
            query = path.substr(question + 1);
            path.resize(question);
        }

        const std::string lowerPath = ToLower(path);
        std::map<std::string, std::string> params;
        ParseForm(query, params);
        ParseForm(body, params);

        if (!ValidateKnownCredentials(params))
            return FailureResponse();

        if (lowerPath == "/" || lowerPath == "/health" || lowerPath == "/emu/status")
            return EmulatorStatusResponse();

        if (lowerPath.find("/authen/getguid.json") != std::string::npos)
        {
            const std::string accountKey = GetParamInsensitive(params, "key");
            if (!accountKey.empty() && accountKey != kUsername)
                return FailureResponse();

            return "{\"return_code\":0,\"return_message\":\"\",\"data\":{"
                   "\"resultCode\":\"0\","
                   "\"guid\":\"LOCAL-GUID-ADMIN\","
                   "\"dynamicKey\":\"LOCAL-DYNAMIC-KEY\","
                   "\"failReason\":\"\"}}";
        }

        if (lowerPath.find("/authen/getclientvkey.json") != std::string::npos)
        {
            return "{\"return_code\":0,\"return_message\":\"\",\"data\":{"
                   "\"resultCode\":\"0\","
                   "\"clientVKey\":\"LOCAL-CLIENT-VKEY\","
                   "\"failReason\":\"\"}}";
        }

        if (lowerPath.find("/authen/getpublickey.json") != std::string::npos)
        {
            // The legacy client only consumes this for RSA/encrypted login modes.
            // An empty key keeps loginType 3 / encryptFlag 0 on the non-RSA path.
            return "{\"return_code\":0,\"return_message\":\"\",\"data\":{"
                   "\"resultCode\":\"0\","
                   "\"key\":\"\","
                   "\"failReason\":\"\"}}";
        }

        if (lowerPath.find("/authen/getaccountinfo.json") != std::string::npos)
            return AccountInfoResponse();

        if (lowerPath.find("/authen/getloginuserinfo.json") != std::string::npos)
            return LoginUserInfoResponse();

        if (lowerPath.find("/authen/getloginareainfo.json") != std::string::npos)
            return LoginAreaInfoResponse();

        if (lowerPath.find("/authen/getloginstates.json") != std::string::npos)
            return LoginStatesResponse();

        if (lowerPath.find("/authen/getloginstate.json") != std::string::npos ||
            lowerPath.find("/authen/extendloginstate.json") != std::string::npos)
        {
            return LoginStateResponse();
        }

        if (lowerPath.find("/authen/checkaccounttype.json") != std::string::npos)
        {
            return "{\"return_code\":0,\"return_message\":\"\",\"data\":{"
                   "\"resultCode\":\"0\",\"accountType\":\"0\",\"failReason\":\"\"}}";
        }

        if (lowerPath.find("/authen/getcodekey.json") != std::string::npos)
        {
            return "{\"return_code\":0,\"return_message\":\"\",\"data\":{"
                   "\"resultCode\":\"0\",\"codeKey\":\"LOCAL-CODE-KEY\",\"failReason\":\"\"}}";
        }

        if (lowerPath.find("/authen/logout.json") != std::string::npos)
            return SuccessResponse(false);

        const char* loginEndpoints[] =
        {
            "/authen/dynamiclogin.json",
            "/authen/staticlogin.json",
            "/authen/checkcodelogin.json",
            "/authen/fcmlogin.json",
            "/authen/autologin.json",
            "/authen/ssologin.json",
            "/authen/fastlogin.json",
            "/authen/phonecheckcodelogin.json",
            "/authen/codekeylogin.json",
            "/authen/rltlogin.json"
        };

        for (const char* endpoint : loginEndpoints)
        {
            if (lowerPath.find(endpoint) != std::string::npos)
                return SuccessResponse(true);
        }

        // Bring-up behavior: return the complete success envelope for unknown CAS
        // methods so the console log tells us the next endpoint without immediately
        // killing the old login state machine. We can make each route exact as it appears.
        if (lowerPath.find("/authen/") != std::string::npos)
        {
            knownEndpoint = false;
            return SuccessResponse(true);
        }

        knownEndpoint = false;
        return "{\"return_code\":0,\"return_message\":\"\",\"data\":{\"resultCode\":\"0\"}}";
    }

    bool ReceiveRequest(SOCKET client, std::string& request)
    {
        request.clear();
        char buffer[8192];
        std::size_t expectedSize = 0;

        for (;;)
        {
            const int received = recv(client, buffer, sizeof(buffer), 0);
            if (received <= 0)
                return !request.empty();

            request.append(buffer, buffer + received);

            const std::size_t headerEnd = request.find("\r\n\r\n");
            if (headerEnd != std::string::npos)
            {
                if (expectedSize == 0)
                {
                    std::size_t contentLength = 0;
                    const std::string headersLower = ToLower(request.substr(0, headerEnd));
                    const std::string key = "content-length:";
                    const std::size_t pos = headersLower.find(key);
                    if (pos != std::string::npos)
                    {
                        const std::size_t valueStart = pos + key.size();
                        const std::size_t valueEnd = headersLower.find("\r\n", valueStart);
                        const std::string value = headersLower.substr(valueStart, valueEnd - valueStart);
                        contentLength = static_cast<std::size_t>(std::strtoul(value.c_str(), nullptr, 10));
                    }
                    expectedSize = headerEnd + 4 + contentLength;
                }

                if (request.size() >= expectedSize)
                    return true;
            }

            if (request.size() > 1024 * 1024)
                return false;
        }
    }

    void HandleClient(SOCKET client)
    {
        std::string request;
        if (!ReceiveRequest(client, request))
            return;

        const std::size_t lineEnd = request.find("\r\n");
        if (lineEnd == std::string::npos)
            return;

        const std::string firstLine = request.substr(0, lineEnd);
        const std::size_t firstSpace = firstLine.find(' ');
        const std::size_t secondSpace = firstLine.find(' ', firstSpace == std::string::npos ? 0 : firstSpace + 1);
        if (firstSpace == std::string::npos || secondSpace == std::string::npos)
            return;

        const std::string method = firstLine.substr(0, firstSpace);
        const std::string target = firstLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);

        const std::size_t headerEnd = request.find("\r\n\r\n");
        const std::string body = headerEnd == std::string::npos ? std::string() : request.substr(headerEnd + 4);

        bool knownEndpoint = false;
        const std::string json = RouteRequest(method, target, body, knownEndpoint);

        std::printf("[EMU]  %s %s%s\n",
                    method.c_str(),
                    RedactPassword(target).c_str(),
                    knownEndpoint ? "" : "  [new/unknown route]");
        std::fflush(stdout);
        if (!body.empty())
            std::printf("[EMU]  body: %s\n", RedactPassword(body).c_str());

        const std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json; charset=utf-8\r\n"
            "Connection: close\r\n"
            "Cache-Control: no-store\r\n"
            "Content-Length: " + std::to_string(json.size()) + "\r\n"
            "\r\n" + json;

        send(client, response.data(), static_cast<int>(response.size()), 0);
    }

    void PrintHex(const char* prefix, const unsigned char* data, int size)
    {
        std::printf("%s %d byte(s):", prefix, size);
        const int shown = (size > 128) ? 128 : size;
        for (int i = 0; i < shown; ++i)
            std::printf(" %02X", static_cast<unsigned int>(data[i]));
        if (size > shown)
            std::printf(" ...");
        std::printf("\n");
        std::fflush(stdout);
    }



    // v10 intentionally advertises DH public value 1. The legacy Photon client
    // therefore derives shared secret BigInteger(1), whose GetBytes() result is
    // the single byte 0x01. DiffieHellmanCryptoProvider then SHA-256 hashes that
    // byte and uses the 32-byte digest as its Rijndael/AES key with a zero IV,
    // CBC mode and PKCS7 padding. This fixed key was verified against the first
    // encrypted BOL Authenticate packet captured from Photon3Unity3D 3.2.0.1.
    const unsigned char kPhotonAesKey[32] = {
        0x4B, 0xF5, 0x12, 0x2F, 0x34, 0x45, 0x54, 0xC5,
        0x3B, 0xDE, 0x2E, 0xBB, 0x8C, 0xD2, 0xB7, 0xE3,
        0xD1, 0x60, 0x0A, 0xD6, 0x31, 0xC3, 0x85, 0xA5,
        0xD7, 0xCC, 0xE2, 0x3C, 0x77, 0x85, 0x45, 0x9A
    };

    bool PhotonAesCrypt(
        bool encrypt,
        const unsigned char* input,
        int inputSize,
        std::vector<unsigned char>& output)
    {
        output.clear();
        if (!input || inputSize <= 0)
            return false;

        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_KEY_HANDLE key = nullptr;
        std::vector<unsigned char> keyObject;
        unsigned char iv[16] = {};
        ULONG objectLength = 0;
        ULONG resultLength = 0;
        ULONG bytesDone = 0;
        NTSTATUS status = BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0);
        if (status < 0)
            goto cleanup;

        status = BCryptSetProperty(
            algorithm,
            BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
            static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_CBC)),
            0);
        if (status < 0)
            goto cleanup;

        status = BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength),
            sizeof(objectLength),
            &bytesDone,
            0);
        if (status < 0 || objectLength == 0)
            goto cleanup;

        keyObject.resize(objectLength);
        status = BCryptGenerateSymmetricKey(
            algorithm,
            &key,
            keyObject.data(),
            static_cast<ULONG>(keyObject.size()),
            const_cast<PUCHAR>(kPhotonAesKey),
            static_cast<ULONG>(sizeof(kPhotonAesKey)),
            0);
        if (status < 0)
            goto cleanup;

        if (encrypt)
        {
            status = BCryptEncrypt(
                key,
                const_cast<PUCHAR>(input),
                static_cast<ULONG>(inputSize),
                nullptr,
                iv,
                sizeof(iv),
                nullptr,
                0,
                &resultLength,
                BCRYPT_BLOCK_PADDING);
        }
        else
        {
            status = BCryptDecrypt(
                key,
                const_cast<PUCHAR>(input),
                static_cast<ULONG>(inputSize),
                nullptr,
                iv,
                sizeof(iv),
                nullptr,
                0,
                &resultLength,
                BCRYPT_BLOCK_PADDING);
        }
        if (status < 0 || resultLength == 0)
            goto cleanup;

        output.resize(resultLength);
        std::memset(iv, 0, sizeof(iv));
        if (encrypt)
        {
            status = BCryptEncrypt(
                key,
                const_cast<PUCHAR>(input),
                static_cast<ULONG>(inputSize),
                nullptr,
                iv,
                sizeof(iv),
                output.data(),
                static_cast<ULONG>(output.size()),
                &resultLength,
                BCRYPT_BLOCK_PADDING);
        }
        else
        {
            status = BCryptDecrypt(
                key,
                const_cast<PUCHAR>(input),
                static_cast<ULONG>(inputSize),
                nullptr,
                iv,
                sizeof(iv),
                output.data(),
                static_cast<ULONG>(output.size()),
                &resultLength,
                BCRYPT_BLOCK_PADDING);
        }
        if (status < 0)
        {
            output.clear();
            goto cleanup;
        }

        output.resize(resultLength);

    cleanup:
        if (key)
            BCryptDestroyKey(key);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
        return status >= 0 && !output.empty();
    }

    bool PhotonDecrypt(const unsigned char* input, int inputSize, std::vector<unsigned char>& plain)
    {
        return PhotonAesCrypt(false, input, inputSize, plain);
    }

    bool PhotonEncrypt(const unsigned char* input, int inputSize, std::vector<unsigned char>& cipher)
    {
        return PhotonAesCrypt(true, input, inputSize, cipher);
    }

    DWORD WINAPI PhotonTcpThread(void*)
    {
        SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server == INVALID_SOCKET)
        {
            std::printf("[PHOTON/TCP] socket failed: %d\n", WSAGetLastError());
            return 0;
        }

        BOOL exclusive = TRUE;
        setsockopt(server, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kPhotonPort);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

        if (bind(server, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
            listen(server, SOMAXCONN) == SOCKET_ERROR)
        {
            std::printf("[PHOTON/TCP] bind/listen 127.0.0.1:%u failed: %d\n", kPhotonPort, WSAGetLastError());
            closesocket(server);
            return 0;
        }

        std::printf("[PHOTON/TCP] listening on 127.0.0.1:%u\n", kPhotonPort);
        std::fflush(stdout);

        for (;;)
        {
            SOCKET client = accept(server, nullptr, nullptr);
            if (client == INVALID_SOCKET)
                break;

            std::printf("[PHOTON/TCP] client connected\n");
            std::fflush(stdout);

            unsigned char buffer[4096];
            for (;;)
            {
                const int received = recv(client, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
                if (received <= 0)
                    break;
                PrintHex("[PHOTON/TCP] recv", buffer, received);
            }

            shutdown(client, SD_BOTH);
            closesocket(client);
            std::printf("[PHOTON/TCP] client disconnected\n");
            std::fflush(stdout);
        }

        closesocket(server);
        return 0;
    }

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

        PrintHex(onlinePeerAuthenticate
                     ? "[PHOTON/UDP] -> ONLINE_AUTHENTICATE_RESPONSE"
                     : "[PHOTON/UDP] -> MASTER_AUTHENTICATE_RESPONSE",
                 reply.data(), static_cast<int>(reply.size()));

        if (onlinePeerAuthenticate)
        {
            std::printf("[PHOTON/UDP] OnlineServerPeer Authenticate success: accountId=100000001 ch=0 seq=%u\n", sequence);
            std::printf("[PHOTON/UDP] AuthenticateAsyncTask should now continue to GetCharacterList (op 84)\n");
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

    bool SendPhotonCharacterListResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        bool encrypted)
    {
        // CharacterService::OnGetCharacterListResponse expects parameter 0 to be
        // a Hashtable whose values are character Hashtables. Before the first
        // create this is empty; after op 82 succeeds we return the locally saved
        // character so relaunches land on character selection instead of create.
        std::vector<unsigned char> plain;
        plain.reserve(1024);
        plain.push_back(operationCode);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // null DebugMessage
        AppendU16BE(plain, 1); // one response parameter
        plain.push_back(0x00); // key 0: character table
        if (g_localCharacter.exists)
        {
            AppendProtocol16HashtableHeader(plain, 1);
            AppendProtocol16TypedInt(plain, g_localCharacter.id);
            AppendCharacterHashtableValue(plain, g_localCharacter);
        }
        else
        {
            AppendProtocol16HashtableHeader(plain, 0);
        }

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
                return false;
            message.push_back(0x83);
            message.insert(message.end(), cipher.begin(), cipher.end());
        }
        else
        {
            message.push_back(0x03);
            message.insert(message.end(), plain.begin(), plain.end());
        }

        std::vector<unsigned char> reply;
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
            std::printf("[PHOTON/UDP] character-list response send failed: %d\n", WSAGetLastError());
            return false;
        }

        PrintHex("[PHOTON/UDP] character-list response plaintext", plain.data(), static_cast<int>(plain.size()));
        PrintHex("[PHOTON/UDP] -> GET_CHARACTER_LIST_RESPONSE", reply.data(), static_cast<int>(reply.size()));
        if (g_localCharacter.exists)
            std::printf("[PHOTON/UDP] GetCharacterList success: local character id=%u name=%s sent\n",
                        g_localCharacter.id, g_localCharacter.name.c_str());
        else
            std::printf("[PHOTON/UDP] GetCharacterList success: empty character list sent; frontend should enter CharacterCreate\n");
        std::fflush(stdout);
        return true;
    }

    bool SendPhotonCreateCharacterResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        const unsigned char* requestPlain,
        int requestPlainSize,
        bool encrypted)
    {
        LocalCharacterState created;
        if (!ParseCreateCharacterRequest(requestPlain, requestPlainSize, created))
        {
            std::printf("[PHOTON/UDP] CreateCharacter request parse failed; leaving request captured for research\n");
            std::fflush(stdout);
            return false;
        }

        // Local preservation behavior: every non-empty nickname is available.
        // The old production name database is intentionally not consulted.
        g_localCharacter = created;
        g_localCharacter.exists = true;

        std::printf("[PHOTON/UDP] op 82 identified as BOL CharacterService::CreateCharacter\n");
        std::printf("[PHOTON/UDP]   nickname=%s avatarType=%u avatarId=%u\n",
                    g_localCharacter.name.c_str(), g_localCharacter.avatarType,
                    g_localCharacter.avatarIdBits);
        std::printf("[PHOTON/UDP]   parts=%s materials=%s\n",
                    g_localCharacter.avatarParts.c_str(), g_localCharacter.avatarMaterials.c_str());

        // CharacterService::OnCreateCharacterResponse expects parameter 0 to be
        // the Character Hashtable itself. ReturnCode 0 selects the success path;
        // production return code 32732 is the "nickname already exists" path the
        // current UI was showing while v13 left op 82 unanswered.
        std::vector<unsigned char> plain;
        plain.reserve(1024);
        plain.push_back(82);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // null DebugMessage
        AppendU16BE(plain, 1);
        plain.push_back(0x00);
        AppendCharacterHashtableValue(plain, g_localCharacter);

        std::vector<unsigned char> message;
        message.push_back(0xF3);
        if (encrypted)
        {
            std::vector<unsigned char> cipher;
            if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
            {
                std::printf("[PHOTON/UDP] CreateCharacter response encryption failed\n");
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
            std::printf("[PHOTON/UDP] CreateCharacter response send failed: %d\n", WSAGetLastError());
            return false;
        }

        const bool saved = SaveLocalCharacter();
        PrintHex("[PHOTON/UDP] create-character response plaintext", plain.data(), static_cast<int>(plain.size()));
        PrintHex("[PHOTON/UDP] -> CREATE_CHARACTER_RESPONSE", reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] CreateCharacter success: id=%u name=%s ch=0 seq=%u%s\n",
                    g_localCharacter.id, g_localCharacter.name.c_str(), sequence,
                    encrypted ? " encrypted" : "");
        std::printf("[PHOTON/UDP] local character persistence: %s (%s)\n",
                    saved ? "saved" : "save failed", CharacterStatePath().c_str());
        std::printf("[PHOTON/UDP] waiting for the first post-character BOL operation\n");
        std::fflush(stdout);
        return true;
    }

    bool SendPhotonSingleCharacterResponse(
        SOCKET server,
        const sockaddr_in& remote,
        int remoteLength,
        unsigned int receivedSentTime,
        unsigned int challenge,
        unsigned char operationCode,
        bool encrypted,
        const char* label)
    {
        // Assembly-CSharp.dll CharacterService uses the same response callback
        // shape for GetMyCharacter (80), GetCharacter (81), and SelectCharacter
        // (83): response parameter 0 is the selected Character Hashtable.
        // v14 stopped on op 80 after CreateCharacter because it only ACKed the
        // request and never delivered this response.
        if (!g_localCharacter.exists)
        {
            std::printf("[PHOTON/UDP] %s requested but no local character exists\n", label);
            std::fflush(stdout);
            return false;
        }

        std::vector<unsigned char> plain;
        plain.reserve(1024);
        plain.push_back(operationCode);
        AppendU16BE(plain, 0); // ReturnCode = OK
        plain.push_back(0x2A); // null DebugMessage
        AppendU16BE(plain, 1); // one response parameter
        plain.push_back(0x00); // key 0: Character Hashtable
        AppendCharacterHashtableValue(plain, g_localCharacter);

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
            std::printf("[PHOTON/UDP] %s response send failed: %d\n", label, WSAGetLastError());
            std::fflush(stdout);
            return false;
        }

        char plainLabel[128]{};
        char replyLabel[128]{};
        std::snprintf(plainLabel, sizeof(plainLabel), "[PHOTON/UDP] %s response plaintext", label);
        std::snprintf(replyLabel, sizeof(replyLabel), "[PHOTON/UDP] -> %s_RESPONSE", label);
        PrintHex(plainLabel, plain.data(), static_cast<int>(plain.size()));
        PrintHex(replyLabel, reply.data(), static_cast<int>(reply.size()));
        std::printf("[PHOTON/UDP] %s success: character id=%u name=%s ch=0 seq=%u%s\n",
                    label, g_localCharacter.id, g_localCharacter.name.c_str(), sequence,
                    encrypted ? " encrypted" : "");
        std::printf("[PHOTON/UDP] waiting for the next post-character BOL operation\n");
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
                                session.onlinePeer = onlinePeerAuthenticate;
                                std::printf("[PHOTON/UDP] Authenticate phase: %s remote=%s:%u\n",
                                            onlinePeerAuthenticate ? "OnlineServerPeer" : "NetworkingPeer/master",
                                            remoteIp, static_cast<unsigned int>(ntohs(remote.sin_port)));
                                session.authResponseSent = SendPhotonAuthenticateResponse(
                                    server, remote, remoteLength, sentTime, challenge, plain[0], onlinePeerAuthenticate);
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
                            else if (!plain.empty() && plain[0] == 229 && !session.joinLobbyResponseSent)
                            {
                                std::printf("[PHOTON/UDP] op 229 identified as Photon JoinLobby\n");
                                session.joinLobbyResponseSent = SendPhotonJoinLobbyResponse(
                                    server, remote, remoteLength, sentTime, challenge, plain[0], true);
                            }
                            else if (!plain.empty())
                            {
                                std::printf("[PHOTON/UDP] unimplemented BOL operation %u (0x%02X) captured\n",
                                            static_cast<unsigned int>(plain[0]),
                                            static_cast<unsigned int>(plain[0]));
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

                            if (operationCode == 84)
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
                            else if (operationCode == 229 && !session.joinLobbyResponseSent)
                            {
                                std::printf("[PHOTON/UDP] op 229 identified as Photon JoinLobby\n");
                                session.joinLobbyResponseSent = SendPhotonJoinLobbyResponse(
                                    server, remote, remoteLength, sentTime, challenge, operationCode, false);
                            }
                            else if (operationCode != 230)
                            {
                                std::printf("[PHOTON/UDP] unimplemented unencrypted operation %u (0x%02X) captured\n",
                                            static_cast<unsigned int>(operationCode),
                                            static_cast<unsigned int>(operationCode));
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
                std::printf("[PHOTON/UDP] ACK received: seq=%u sentTime=%u\n",
                            ReadU32BE(payload), ReadU32BE(payload + 4));
            }
            else if (type == 4)
            {
                std::printf("[PHOTON/UDP] DISCONNECT received (reserved/reason=%u)\n",
                            static_cast<unsigned int>(reserved));
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
        std::printf("[PHOTON/UDP] Photon 3.2 master + online-service + character create/select responder enabled\n");
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

int main()
{
    SetConsoleTitleW(L"Borderlands Online Emulator");

    const bool loadedCharacter = LoadLocalCharacter();

    std::printf("Borderlands Online local emulator\n");
    std::printf("Listen: 127.0.0.1:%u\n", kListenPort);
    std::printf("Premade account: admin / admin\n");
    std::printf("CAS compatibility service: ready\n");
    std::printf("Game login route: Photon 127.0.0.1:%u\n", kPhotonPort);
    std::printf("Local area/group: 1 / 1 (Local)\n");
    if (loadedCharacter)
        std::printf("Local character: id=%u name=%s (loaded)\n", g_localCharacter.id, g_localCharacter.name.c_str());
    else
        std::printf("Local character: none yet (Create Character will be accepted locally)\n");
    std::printf("Waiting for emulator requests...\n\n");

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::printf("WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    if (HANDLE thread = CreateThread(nullptr, 0, PhotonTcpThread, nullptr, 0, nullptr))
        CloseHandle(thread);
    if (HANDLE thread = CreateThread(nullptr, 0, PhotonUdpThread, nullptr, 0, nullptr))
        CloseHandle(thread);

    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET)
    {
        std::printf("socket failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    BOOL exclusive = TRUE;
    setsockopt(server, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kListenPort);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    if (bind(server, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        std::printf("bind 127.0.0.1:%u failed: %d\n", kListenPort, WSAGetLastError());
        closesocket(server);
        WSACleanup();
        return 1;
    }

    if (listen(server, SOMAXCONN) == SOCKET_ERROR)
    {
        std::printf("listen failed: %d\n", WSAGetLastError());
        closesocket(server);
        WSACleanup();
        return 1;
    }

    for (;;)
    {
        SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET)
        {
            std::printf("accept failed: %d\n", WSAGetLastError());
            break;
        }

        HandleClient(client);
        shutdown(client, SD_BOTH);
        closesocket(client);
    }

    closesocket(server);
    WSACleanup();
    return 0;
}
