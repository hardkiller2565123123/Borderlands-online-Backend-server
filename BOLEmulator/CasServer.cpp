#include "EmulatorShared.h"

namespace bolemu
{
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
}
