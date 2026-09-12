#include "../EmulatorShared.h"

namespace bolemu
{
    namespace
    {
        constexpr unsigned char kTypeNull = 0x2A;
        constexpr unsigned char kTypeDictionary = 0x44;
        constexpr unsigned char kTypeBool = 0x6F;
        constexpr unsigned char kTypeShort = 0x6B;
        constexpr unsigned char kTypeInt = 0x69;
        constexpr unsigned char kTypeDouble = 0x64;
        constexpr unsigned char kTypeString = 0x73;
        constexpr unsigned char kTypeHashtable = 0x68;
        constexpr unsigned char kTypeObjectArray = 0x7A;
        constexpr unsigned char kTypeArray = 0x79;

        void AppendTypedBool(std::vector<unsigned char>& out, bool value)
        {
            out.push_back(kTypeBool);
            out.push_back(value ? 1 : 0);
        }

        void AppendTypedShort(std::vector<unsigned char>& out, short value)
        {
            out.push_back(kTypeShort);
            AppendU16BE(out, static_cast<unsigned short>(value));
        }

        void AppendTypedDouble(std::vector<unsigned char>& out, double value)
        {
            out.push_back(kTypeDouble);
            unsigned char bytes[sizeof(double)]{};
            std::memcpy(bytes, &value, sizeof(double));
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
            for (int i = static_cast<int>(sizeof(double)) - 1; i >= 0; --i)
                out.push_back(bytes[i]);
#else
            out.insert(out.end(), bytes, bytes + sizeof(double));
#endif
        }

        void AppendEmptyObjectArray(std::vector<unsigned char>& out)
        {
            out.push_back(kTypeObjectArray);
            AppendU16BE(out, 0);
        }

        void AppendEmptyTypedArray(std::vector<unsigned char>& out, unsigned char elementType)
        {
            out.push_back(kTypeArray);
            AppendU16BE(out, 0);
            out.push_back(elementType);
        }

        void AppendEmptyDictionary(std::vector<unsigned char>& out, unsigned char keyType, unsigned char valueType)
        {
            out.push_back(kTypeDictionary);
            out.push_back(keyType);
            out.push_back(valueType);
            AppendU16BE(out, 0);
        }

        void AppendBoolEntry(std::vector<unsigned char>& out, unsigned int key, bool value)
        {
            AppendProtocol16HashtableIntKey(out, key);
            AppendTypedBool(out, value);
        }

        void AppendDoubleEntry(std::vector<unsigned char>& out, unsigned int key, double value)
        {
            AppendProtocol16HashtableIntKey(out, key);
            AppendTypedDouble(out, value);
        }

        void AppendHashtableEntryHeader(std::vector<unsigned char>& out, unsigned int key, unsigned short count)
        {
            AppendProtocol16HashtableIntKey(out, key);
            AppendProtocol16HashtableHeader(out, count);
        }

        void AppendMinimalMoneyItem(std::vector<unsigned char>& out)
        {
            // ItemDNA::FromHashtable only needs type=Money, price and quantity for
            // this compact placeholder. It produces a real managed ItemDNA object.
            AppendProtocol16HashtableHeader(out, 3);
            AppendProtocol16IntEntry(out, 0, 3); // ItemType.Money
            AppendProtocol16IntEntry(out, 5, 0); // price
            AppendProtocol16IntEntry(out, 6, 0); // amount
        }

        void AppendLocalLobby(std::vector<unsigned char>& out)
        {
            // Lobby::.ctor reads keys 0..9 unconditionally. Empty member/group
            // tables are valid and keep this as a one-client local lobby.
            AppendProtocol16HashtableHeader(out, 10);
            AppendProtocol16IntEntry(out, 0, 1); // lobby id
            AppendProtocol16IntEntry(out, 1, g_localCharacter.id); // owner character id
            AppendProtocol16IntEntry(out, 2, 4); // total slots
            AppendBoolEntry(out, 3, false); // is group game
            AppendProtocol16StringEntry(out, 4, "LocalGame");
            AppendHashtableEntryHeader(out, 5, 0); // Presence members
            AppendHashtableEntryHeader(out, 6, 0); // Group entries
            AppendProtocol16IntEntry(out, 7, 1); // GamemodeTown
            AppendProtocol16IntEntry(out, 8, 3); // observed local playlist
            AppendProtocol16IntEntry(out, 9, 0); // Normal
        }

        void AppendLocalGuild(std::vector<unsigned char>& out)
        {
            // Guild::.ctor reads 0..14 and conditionally consumes 15.
            AppendProtocol16HashtableHeader(out, 16);
            AppendProtocol16IntEntry(out, 0, 1);
            AppendProtocol16StringEntry(out, 1, "Local Guild");
            AppendProtocol16StringEntry(out, 2, "Local");
            AppendProtocol16IntEntry(out, 3, 1);
            AppendProtocol16IntEntry(out, 4, 0);
            AppendProtocol16IntEntry(out, 5, 1);
            AppendBoolEntry(out, 6, true);
            AppendBoolEntry(out, 7, true);
            AppendBoolEntry(out, 8, true);
            AppendBoolEntry(out, 9, true);
            AppendProtocol16IntEntry(out, 10, 0);
            AppendBoolEntry(out, 11, true);
            AppendProtocol16IntEntry(out, 12, 32);
            AppendProtocol16IntEntry(out, 13, g_localCharacter.accountId);
            AppendProtocol16IntEntry(out, 14, g_localCharacter.id);
            AppendProtocol16NullEntry(out, 15);
        }

        void AppendLocalShop(std::vector<unsigned char>& out)
        {
            AppendProtocol16HashtableHeader(out, 5);
            AppendProtocol16IntEntry(out, 0, 1);
            AppendHashtableEntryHeader(out, 1, 0); // item list
            AppendProtocol16IntEntry(out, 2, 0);
            AppendProtocol16IntEntry(out, 3, 3600);
            AppendDoubleEntry(out, 4, 1.0);
        }

        void AppendLocalMatchRecord(std::vector<unsigned char>& out)
        {
            AppendProtocol16HashtableHeader(out, 13);
            AppendProtocol16IntEntry(out, 0, g_localCharacter.id);
            for (unsigned int i = 1; i <= 8; ++i)
                AppendProtocol16IntEntry(out, i, 0);
            for (unsigned int i = 9; i <= 12; ++i)
                AppendProtocol16NullEntry(out, i);
        }

        void AppendLocalPresence(std::vector<unsigned char>& out)
        {
            // Presence::.ctor requires keys 0..11 plus 13 and 14. This is the
            // same local shape used by PresenceService::GetPresence.
            AppendProtocol16HashtableHeader(out, 14);
            AppendProtocol16IntEntry(out, 0, g_localCharacter.id);
            AppendProtocol16HashtableIntKey(out, 1);
            AppendTypedBool(out, true);
            AppendProtocol16IntEntry(out, 2, 0);
            AppendProtocol16IntEntry(out, 3, g_localServices.currentTown);
            AppendProtocol16IntEntry(out, 4, g_localCharacter.level);
            AppendProtocol16IntEntry(out, 5, g_localCharacter.experience);
            AppendProtocol16IntEntry(out, 6, g_localServices.bondCurrency);
            AppendProtocol16IntEntry(out, 7, g_localCharacter.avatarType);
            AppendProtocol16StringEntry(out, 8, g_localCharacter.avatarParts);
            AppendProtocol16StringEntry(out, 9, g_localCharacter.avatarMaterials);
            AppendProtocol16IntEntry(out, 10, g_localCharacter.avatarIdBits);
            AppendProtocol16StringEntry(out, 11, g_localCharacter.name);
            AppendProtocol16IntEntry(out, 13, 1);
            AppendProtocol16IntEntry(out, 14, 1);
        }

        void AppendEmptyQuestMap(std::vector<unsigned char>& out)
        {
            AppendProtocol16HashtableHeader(out, 0);
        }

        void AppendResponseParameterPrefix(std::vector<unsigned char>& out, unsigned char key)
        {
            out.push_back(key);
        }


        bool BeginRequestObjectArray(
            const unsigned char* plain,
            int plainSize,
            unsigned char expectedOperation,
            int& offset,
            unsigned short& objectCount)
        {
            if (!plain || plainSize < 7 || plain[0] != expectedOperation)
                return false;

            const unsigned short parameterCount = ReadU16BE(plain + 1);
            if (parameterCount < 1)
                return false;

            offset = 3;
            if (offset + 4 > plainSize)
                return false;

            const unsigned char key = plain[offset++];
            const unsigned char type = plain[offset++];
            if (key != 112 || type != kTypeObjectArray)
                return false;

            objectCount = ReadU16BE(plain + offset);
            offset += 2;
            return true;
        }

        bool SendServiceEventPacket(
            SOCKET server,
            const sockaddr_in& remote,
            int remoteLength,
            unsigned int receivedSentTime,
            unsigned int challenge,
            unsigned char eventCode,
            bool encrypted,
            const std::vector<unsigned char>& parameters,
            unsigned short parameterCount,
            const char* label)
        {
            std::vector<unsigned char> plain;
            plain.reserve(4 + parameters.size());
            plain.push_back(eventCode);
            AppendU16BE(plain, parameterCount);
            plain.insert(plain.end(), parameters.begin(), parameters.end());

            std::vector<unsigned char> message;
            message.push_back(0xF3);
            if (encrypted)
            {
                std::vector<unsigned char> cipher;
                if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
                {
                    std::printf("[PHOTON/CONTRACT-EVENT] %s encryption failed\n", label);
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
                std::printf("[PHOTON/CONTRACT-EVENT] %s send failed: %d\n", label, WSAGetLastError());
                std::fflush(stdout);
                return false;
            }

            char wireLabel[160]{};
            std::snprintf(wireLabel, sizeof(wireLabel), "[PHOTON/CONTRACT-EVENT] -> %s", label);
            PrintHex(wireLabel, reply.data(), static_cast<int>(reply.size()));
            std::printf("[PHOTON/CONTRACT-EVENT] %s event=%u params=%u seq=%u%s\n",
                        label, static_cast<unsigned int>(eventCode),
                        static_cast<unsigned int>(parameterCount), sequence,
                        encrypted ? " encrypted" : "");
            std::fflush(stdout);
            return true;
        }

        bool SendOperationResponsePacket(
            SOCKET server,
            const sockaddr_in& remote,
            int remoteLength,
            unsigned int receivedSentTime,
            unsigned int challenge,
            unsigned char operationCode,
            bool encrypted,
            const std::vector<unsigned char>& parameters,
            unsigned short parameterCount,
            const char* label)
        {
            std::vector<unsigned char> plain;
            plain.reserve(8 + parameters.size());
            plain.push_back(operationCode);
            AppendU16BE(plain, 0); // ReturnCode = OK
            plain.push_back(kTypeNull); // DebugMessage = null
            AppendU16BE(plain, parameterCount);
            plain.insert(plain.end(), parameters.begin(), parameters.end());

            std::vector<unsigned char> message;
            message.push_back(0xF3);
            if (encrypted)
            {
                std::vector<unsigned char> cipher;
                if (!PhotonEncrypt(plain.data(), static_cast<int>(plain.size()), cipher))
                {
                    std::printf("[PHOTON/CONTRACT] %s encryption failed\n", label);
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
                std::printf("[PHOTON/CONTRACT] %s send failed: %d\n", label, WSAGetLastError());
                std::fflush(stdout);
                return false;
            }

            char wireLabel[160]{};
            std::snprintf(wireLabel, sizeof(wireLabel), "[PHOTON/CONTRACT] -> %s_RESPONSE", label);
            PrintHex(wireLabel, reply.data(), static_cast<int>(reply.size()));
            std::printf("[PHOTON/CONTRACT] %s::%s op=%u contract response params=%u seq=%u%s\n",
                        LookupBOLServiceName(operationCode), LookupBOLOperationName(operationCode),
                        static_cast<unsigned int>(operationCode), static_cast<unsigned int>(parameterCount),
                        sequence, encrypted ? " encrypted" : "");
            std::fflush(stdout);
            return true;
        }
    }

    bool SendPhotonDecompiledContractResponse(
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
        std::vector<unsigned char> p;
        unsigned short count = 0;
        bool emitExperienceEvent = false;
        bool emitCurrencyEvent = false;
        bool emitPresenceEvent = false;

        auto IntParam = [&](unsigned char key, unsigned int value)
        {
            AppendResponseParameterPrefix(p, key);
            AppendProtocol16TypedInt(p, value);
            ++count;
        };
        auto ShortParam = [&](unsigned char key, short value)
        {
            AppendResponseParameterPrefix(p, key);
            AppendTypedShort(p, value);
            ++count;
        };
        auto BoolParam = [&](unsigned char key, bool value)
        {
            AppendResponseParameterPrefix(p, key);
            AppendTypedBool(p, value);
            ++count;
        };
        auto StringParam = [&](unsigned char key, const char* value)
        {
            AppendResponseParameterPrefix(p, key);
            AppendProtocol16TypedString(p, value ? value : "");
            ++count;
        };
        auto NullParam = [&](unsigned char key)
        {
            AppendResponseParameterPrefix(p, key);
            AppendProtocol16TypedNull(p);
            ++count;
        };
        auto EmptyHashtableParam = [&](unsigned char key)
        {
            AppendResponseParameterPrefix(p, key);
            AppendProtocol16HashtableHeader(p, 0);
            ++count;
        };
        auto EmptyHashtableArrayParam = [&](unsigned char key)
        {
            AppendResponseParameterPrefix(p, key);
            AppendProtocol16EmptyHashtableArray(p);
            ++count;
        };
        auto EmptyIntArrayParam = [&](unsigned char key)
        {
            AppendResponseParameterPrefix(p, key);
            AppendEmptyTypedArray(p, kTypeInt);
            ++count;
        };
        auto EmptyIntHashtableDictionaryParam = [&](unsigned char key)
        {
            AppendResponseParameterPrefix(p, key);
            AppendEmptyDictionary(p, kTypeInt, kTypeHashtable);
            ++count;
        };
        auto EmptyIntIntDictionaryParam = [&](unsigned char key)
        {
            AppendResponseParameterPrefix(p, key);
            AppendEmptyDictionary(p, kTypeInt, kTypeInt);
            ++count;
        };

        switch (operationCode)
        {
            // Character / settings. Specialized startup handlers own 74/75/80-84;
            // the remaining callbacks are return-code-only or small primitives.
            case 62: // GPKAuthReply
            case 76: // CancelMyGuildApplication
            case 78: // GetMyGuildNews callback only checks return code
                break;

            case 74: // SetPersonalSettings(string)
            {
                int offset = 0;
                unsigned short objectCount = 0;
                std::string settings;
                if (BeginRequestObjectArray(requestPlain, requestPlainSize, operationCode, offset, objectCount) &&
                    objectCount >= 1 && ReadProtocol16TypedString(requestPlain, requestPlainSize, offset, settings))
                {
                    g_localServices.personalSettings = settings;
                    SaveLocalCharacter();
                    std::printf("[PHOTON/STATE] personal settings persisted: %zu byte(s)\n", settings.size());
                }
                else
                {
                    std::printf("[PHOTON/STATE] SetPersonalSettings request could not be decoded; preserving previous value\n");
                }
                std::fflush(stdout);
                break;
            }

            case 79: // UpdateCharacterExperience(characterId,newExperience)
            {
                int offset = 0;
                unsigned short objectCount = 0;
                unsigned int characterId = 0;
                unsigned int experience = 0;
                if (BeginRequestObjectArray(requestPlain, requestPlainSize, operationCode, offset, objectCount) &&
                    objectCount >= 2 &&
                    ReadProtocol16TypedInt(requestPlain, requestPlainSize, offset, characterId) &&
                    ReadProtocol16TypedInt(requestPlain, requestPlainSize, offset, experience) &&
                    characterId == g_localCharacter.id)
                {
                    g_localCharacter.experience = experience;
                    SaveLocalCharacter();
                    emitExperienceEvent = true;
                    std::printf("[PHOTON/STATE] experience updated: character=%u experience=%u\n", characterId, experience);
                }
                else
                {
                    std::printf("[PHOTON/STATE] UpdateCharacterExperience request could not be applied\n");
                }
                std::fflush(stdout);
                break;
            }

            case 63: // GetGuildManagerList -> Hashtable values
                EmptyHashtableParam(0);
                break;
            case 64: // ActivateGuildSkill -> skill id
                IntParam(0, 0);
                break;
            case 65: // GetMyGuildSkillList - callback tolerates empty response
            case 68: // UpgradeStarLevel
            case 69: // UpgradeStarExperience
            case 70: // AwardLuckyBox
                break;
            case 71: // OpenLuckyBox(bool locked,int index,ItemDNA,ItemDNA,ItemDNA)
                BoolParam(0, false);
                IntParam(1, 0);
                AppendResponseParameterPrefix(p, 2); AppendMinimalMoneyItem(p); ++count;
                AppendResponseParameterPrefix(p, 3); AppendMinimalMoneyItem(p); ++count;
                AppendResponseParameterPrefix(p, 4); AppendMinimalMoneyItem(p); ++count;
                break;
            case 72: // DischargeGuildManager -> character id
            case 73: // AssignGuildManager -> character id
                IntParam(0, 0);
                break;
            case 77: // GetMyGuildApplications: callback enumerates response values when non-null.
                NullParam(0);
                break;
            case 85: // GetAntiLevel -> characterId, anti level
                IntParam(0, g_localCharacter.id);
                IntParam(1, 0);
                break;

            // Lobby model contracts.
            case 87: // CreateLobby
            case 96: // OpenLobby
            case 126: // GetLobby
            case 131: // AcceptLobbyInvitation
                AppendResponseParameterPrefix(p, 0); AppendLocalLobby(p); ++count;
                break;
            case 122: // SetTeamID
            case 125: // CloseLobby
            case 130: // RejectLobbyInvitation
            case 132: // InviteToLobby
            case 136: // KickPlayerFromGame
                break;
            case 124: // AcceptLobbyGameInvitation -> map/session name + playlist
            case 137: // AcceptGameInvitation
            case 199: // AcceptGroupGameInvitation
                StringParam(0, "lvl_Floasm_Small");
                IntParam(1, 3);
                break;
            case 129: // KickPlayerFromLobby -> kicked character id
                IntParam(0, 0);
                break;

            case 127: // StartLobbyGame - NetworkingPeer consumes Master->GameServer redirect fields
            case 198: // JoinSessionInProgress - same redirect contract
                StringParam(255, "LocalGame");
                EmptyHashtableParam(248); // Room/game properties (valid empty fallback)
                EmptyHashtableParam(234); // fallback session table
                StringParam(230, "127.0.0.1:5055");
                break;

            // Inventory / warehouse / mail. Startup inventory/warehouse are specialized.
            case 88: // Tidy -> complete Inventory
                AppendResponseParameterPrefix(p, 0); AppendInventoryHashtableValue(p); ++count;
                break;
            case 91: // DeleteMail
            case 92: // GetMailItems
            case 93: // UpdateMailState
                IntParam(0, 0);
                break;
            case 94: // SendMail -> result short
                ShortParam(0, 0);
                break;
            case 95: // GetMail specialized bootstrap; generic callback itself only checks return code
                break;

            // Guild contracts.
            case 100: // ChangeGuildBanner
            case 105: // DismissGuild
            case 110: // GetGuildApplications callback tolerates empty response
            case 112: // QuitGuild
            case 115: // GetMyGuildInvitations callback tolerates empty response
                break;
            case 101: // GetMyGuildInfo -> guild id + optional Guild
                IntParam(0, 0);
                NullParam(1);
                break;
            case 102: // GetMyGuildId
                IntParam(0, 0);
                break;
            case 103: // GetGuildInfo -> Guild
                AppendResponseParameterPrefix(p, 0); AppendLocalGuild(p); ++count;
                break;
            case 104: // CreateGuild -> GuildCreationResult + Guild
                IntParam(0, 0);
                AppendResponseParameterPrefix(p, 1); AppendLocalGuild(p); ++count;
                break;
            case 107: // ChangeGuildMaster
            case 109: // ApplyToGuild
            case 113: // ExpelFromGuild
                IntParam(0, 0);
                break;
            case 108: // SearchGuild: callback accepts zero parameters as empty list
                break;
            case 111: // ProcessGuildApplications -> result + character id
                IntParam(0, 0);
                IntParam(1, 0);
                break;
            case 114: // InviteToGuild -> GuildInvitationResult enum
                IntParam(0, 0);
                break;
            case 116: // ProcessGuildInvitations -> Guild
                AppendResponseParameterPrefix(p, 0); AppendLocalGuild(p); ++count;
                break;

            // Shop / crafting / LuckyBox.
            case 119: // SellItem -> sell price + ItemDNA
                IntParam(0, 0);
                AppendResponseParameterPrefix(p, 1); AppendMinimalMoneyItem(p); ++count;
                break;
            case 120: // BuyItem -> ItemDNA
                AppendResponseParameterPrefix(p, 0); AppendMinimalMoneyItem(p); ++count;
                break;
            case 121: // GetShop -> Shop
                AppendResponseParameterPrefix(p, 0); AppendLocalShop(p); ++count;
                break;
            case 139: // Chat response -> short status for Online chat path
                ShortParam(0, 0);
                break;
            case 143: // GetMyCraftedWeapons -> int[]
                EmptyIntArrayParam(0);
                break;
            case 153: // CraftWeapon -> ItemDNA + crafted weapon id
                AppendResponseParameterPrefix(p, 0); AppendMinimalMoneyItem(p); ++count;
                IntParam(1, 0);
                break;
            case 154: // SmeltWeapon callback only needs return code
                break;

            // Friends / presence / matchmaking records.
            case 144: // GetAllFriendsCharacterInfo -> Hashtable
                EmptyHashtableParam(0);
                break;
            case 146: // GetAllFriendsPresence callback handles response status only
                break;
            case 148: // MatchRecord
                AppendResponseParameterPrefix(p, 0); AppendLocalMatchRecord(p); ++count;
                break;
            case 152: // SearchForCharacter -> Hashtable of matches
                EmptyHashtableParam(0);
                break;
            case 201: // IsUserIgnored
            case 206: // IsUserAFriend
                IntParam(0, g_localCharacter.id);
                IntParam(1, 0);
                BoolParam(2, false);
                break;
            case 207: // RetrieveFriendsList: callback only signals completion; events fill list
                break;
            case 197: // IsFriendsGameJoinable is consumed directly by NetworkingPeer
                BoolParam(0, false);
                break;

            // Currency.
            case 156: // GetMyCurrencies
            case 187: // GetCurrencies
                IntParam(0, g_localCharacter.id);
                IntParam(1, g_localServices.bondCurrency);
                IntParam(2, g_localServices.specialCurrency);
                IntParam(3, g_localServices.circulateCurrency);
                break;

            // Group.
            case 151: // RejoinGroup
            case 158: // GetMyGroupInfo
                IntParam(0, 0); // no group; callback does not consume members when <=0
                break;
            case 159: // LeaveGroup
                break;
            case 160: // KickPlayerFromGroup -> character id
            case 163: // SendGroupInvitation -> result/int id
                IntParam(0, 0);
                break;
            case 162: // AcceptGroupInvitation -> group id, leader id, Presence[]
                IntParam(0, 0);
                IntParam(1, 0);
                EmptyHashtableArrayParam(2);
                break;

            // Skill tree.
            case 168: // ResetSkillPoints -> character id
                IntParam(0, g_localCharacter.id);
                break;
            case 169: // InvestMultipleSkillPoints -> character id + Dictionary<int,Hashtable>
                IntParam(0, g_localCharacter.id);
                EmptyIntHashtableDictionaryParam(1);
                break;
            case 170: // InvestSkillPoint -> character id + remaining points
                IntParam(0, g_localCharacter.id);
                IntParam(1, 0);
                break;
            case 171: // GetInvestedSkillPoints -> character id + Hashtable[]
                IntParam(0, g_localCharacter.id);
                EmptyHashtableArrayParam(1);
                break;
            case 173: // GetMySkillPoints (specialized also returns this exact shape)
                IntParam(0, g_localCharacter.id);
                IntParam(1, 0);
                IntParam(2, 0);
                EmptyHashtableArrayParam(3);
                break;

            // Progression.
            case 179: // CompleteInstance -> three scalar result fields
                IntParam(0, g_localCharacter.id);
                IntParam(1, 0);
                IntParam(2, 0);
                break;
            case 180: // GetUnlockedInstances -> Dictionary<int,int>
                EmptyIntIntDictionaryParam(0);
                break;

            // Quest. Existing 141/185 startup handlers are more complete.
            case 141: // daily quest contents
                EmptyHashtableParam(0);
                break;
            case 145: // BringToNPC -> character id + quest table
            case 150: // TalkToNPC
                IntParam(0, g_localCharacter.id);
                EmptyHashtableParam(1);
                break;
            case 181: // ClaimQuestRewards
                BoolParam(0, false);
                IntParam(1, g_localCharacter.id);
                IntParam(2, 0);
                NullParam(3);
                break;
            case 183: // UpdateQuestRequirement returns same shape as GetPlayerQuests
            case 185: // GetPlayerQuests
                IntParam(0, g_localCharacter.id);
                EmptyHashtableParam(1);
                break;
            case 184: // AcceptQuest -> character id, quest id, accepted
                IntParam(0, g_localCharacter.id);
                IntParam(1, 0);
                BoolParam(2, true);
                break;

            // Presence / local session preference.
            case 155: // SetCurrentTown(characterId,townId)
            {
                int offset = 0;
                unsigned short objectCount = 0;
                unsigned int characterId = 0;
                unsigned int townId = 0;
                if (BeginRequestObjectArray(requestPlain, requestPlainSize, operationCode, offset, objectCount) &&
                    objectCount >= 2 &&
                    ReadProtocol16TypedInt(requestPlain, requestPlainSize, offset, characterId) &&
                    ReadProtocol16TypedInt(requestPlain, requestPlainSize, offset, townId) &&
                    characterId == g_localCharacter.id)
                {
                    g_localServices.currentTown = townId;
                    SaveLocalCharacter();
                    emitPresenceEvent = true;
                    std::printf("[PHOTON/STATE] current town updated: character=%u town=%u\n", characterId, townId);
                }
                else
                {
                    std::printf("[PHOTON/STATE] SetCurrentTown request could not be applied\n");
                }
                std::fflush(stdout);
                break;
            }

            case 186: // UpdateCurrencies(characterId,bond,special,circulate)
            {
                int offset = 0;
                unsigned short objectCount = 0;
                unsigned int characterId = 0;
                unsigned int bond = 0;
                unsigned int special = 0;
                unsigned int circulate = 0;
                if (BeginRequestObjectArray(requestPlain, requestPlainSize, operationCode, offset, objectCount) &&
                    objectCount >= 4 &&
                    ReadProtocol16TypedInt(requestPlain, requestPlainSize, offset, characterId) &&
                    ReadProtocol16TypedInt(requestPlain, requestPlainSize, offset, bond) &&
                    ReadProtocol16TypedInt(requestPlain, requestPlainSize, offset, special) &&
                    ReadProtocol16TypedInt(requestPlain, requestPlainSize, offset, circulate) &&
                    characterId == g_localCharacter.id)
                {
                    g_localServices.bondCurrency = bond;
                    g_localServices.specialCurrency = special;
                    g_localServices.circulateCurrency = circulate;
                    SaveLocalCharacter();
                    emitCurrencyEvent = true;
                    std::printf("[PHOTON/STATE] currencies updated: character=%u bond=%u special=%u circulate=%u\n",
                                characterId, bond, special, circulate);
                }
                else
                {
                    std::printf("[PHOTON/STATE] UpdateCurrencies request could not be applied\n");
                }
                std::fflush(stdout);
                break;
            }

            // Messaging.
            case 210: // RetrieveMessages -> Dictionary<int,Hashtable>
                EmptyIntHashtableDictionaryParam(0);
                break;

            // Known client service operations that either have no registered
            // response callback or only need ReturnCode. Returning a normal
            // OperationResponse keeps the client request queue deterministic.
            case 66: case 67: case 86: case 89: case 90:
            case 97: case 98: case 106: case 117: case 118:
            case 123: case 133: case 134: case 135:
            case 138: case 142: case 149: case 157:
            case 161: case 164: case 165: case 166: case 167:
            case 172: case 174: case 175: case 176: case 177:
            case 178: case 182: case 188: case 189:
            case 190: case 191: case 192: case 193: case 194:
            case 195: case 200: case 202:
            case 203: case 204: case 205: case 208: case 209:
            case 211: case 212: case 213: case 214: case 215:
            case 216: case 217: case 218: case 219: case 220:
            case 221: case 222: case 228:
                break;

            // Photon LoadBalancing utility operations used by PUN after joining.
            case 248: // ChangeGroups - response is status only
            case 252: // SetProperties - response is status only; property events are emitted by authoritative game paths as needed
                break;
            case 251: // GetProperties: NetworkingPeer unconditionally casts 249 and 248 to Hashtables
                EmptyHashtableParam(249);
                EmptyHashtableParam(248);
                break;

            default:
                return false;
        }

        const char* label = LookupBOLOperationName(operationCode);
        const bool responseSent = SendOperationResponsePacket(server, remote, remoteLength,
                                                              receivedSentTime, challenge,
                                                              operationCode, encrypted,
                                                              p, count, label);
        if (!responseSent)
            return false;

        if (emitExperienceEvent)
        {
            std::vector<unsigned char> eventParams;
            eventParams.push_back(0);
            AppendProtocol16TypedInt(eventParams, g_localCharacter.experience);
            SendServiceEventPacket(server, remote, remoteLength, receivedSentTime, challenge,
                                   206, encrypted, eventParams, 1, "EXPERIENCE_UPDATED_EVENT_206");
        }

        if (emitCurrencyEvent)
        {
            std::vector<unsigned char> eventParams;
            eventParams.push_back(0); AppendProtocol16TypedInt(eventParams, g_localServices.bondCurrency);
            eventParams.push_back(1); AppendProtocol16TypedInt(eventParams, g_localServices.specialCurrency);
            eventParams.push_back(2); AppendProtocol16TypedInt(eventParams, g_localServices.circulateCurrency);
            SendServiceEventPacket(server, remote, remoteLength, receivedSentTime, challenge,
                                   218, encrypted, eventParams, 3, "CURRENCIES_UPDATED_EVENT_218");
        }

        if (emitPresenceEvent)
        {
            std::vector<unsigned char> eventParams;
            eventParams.push_back(0);
            AppendLocalPresence(eventParams);
            SendServiceEventPacket(server, remote, remoteLength, receivedSentTime, challenge,
                                   195, encrypted, eventParams, 1, "PRESENCE_UPDATED_EVENT_195");
        }

        return true;
    }
}
