#include "EmulatorShared.h"
#include <fstream>

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

    // Session prefab attribution history:
    //   1001..1004 = transport-safe but inert with the fixed Event206 payload.
    //   1005       = ACKed, then NetworkingPeer.OnEvent NRE about 2 seconds later.
    // v51 additionally proved 1006 inert. v53 locks to view1005 and tests the exact managed contract.
    static bool g_autoChainPlayerReadyAwaitingAck = false;
    static unsigned int g_autoChainPlayerReadySequence = 0;
    static bool g_autoChainMapStatePending = false;
    static unsigned int g_autoChainMapStateNotBefore = 0;

    struct AutoMapCandidate
    {
        const char* mapName;
        const char* gameModeName;
        const char* source;
    };

    static const AutoMapCandidate kAutoMapCandidates[] =
    {
        { "Bloodrage Deeprun",      "GamemodePvE", "display map + XsdSettings enum spelling" },
        { "Bloodrage Deeprun",      "GameModePvE", "display map + legacy spelling" },
        { "lvl_Bloodrage_Deeprun",  "GamemodePvE", "derived Unity level-name candidate" },
        { "lvl_bloodrage_deeprun",  "GamemodePvE", "derived lowercase Unity level-name candidate" },
        { "lvl_BloodrageDeeprun",   "GamemodePvE", "derived compact Unity level-name candidate" },
        { "lvl_Floasm_Small",       "GamemodePvE", "known level identifier present in Assembly-CSharp.dll" },
        { "lvl_Floasm_Small",       "GameModePvE", "known level identifier + legacy mode spelling" }
    };

    static bool g_autoMapProbeActive = false;
    static unsigned int g_autoMapProbeStartCandidate = 0;
    static unsigned int g_autoMapProbeAttempts = 0;
    static unsigned int g_autoMapProbeCurrentCandidate = 0;
    static unsigned int g_autoMapProbeNextAt = 0;

    // v49/v50 attribution results:
    //   1001..1004 = ACKed and inert with the fixed SessionInfo-shaped payload.
    //   1005       = ACKed, then NetworkingPeer.OnEvent NRE ~2s later.
    // v51 is a clean control run for the only untested Session PhotonView: 1006.
    static unsigned int g_sessionInfoViewProbeAttempt = 0;
    static unsigned int g_sessionInfoViewProbeLastViewId = 0;
    static unsigned int g_sessionInfoViewProbeNextAt = 0;
    enum class SessionInfoExactProbeStage
    {
        Inactive,
        WaitForContractBaseline,
        AwaitContractBaseline,
        AwaitMapTrigger,
        Complete
    };

    static SessionInfoExactProbeStage g_sessionInfoExactProbeStage = SessionInfoExactProbeStage::Inactive;
    static unsigned int g_sessionInfoExactProbeNextAt = 0;
    static unsigned int g_sessionInfoExactProbeStartedAt = 0;
    static unsigned int g_sessionInfoExactProbeLastStatusAt = 0;
    static std::string g_sessionInfoProbeMapName;
    static std::string g_sessionInfoProbeGameMode;
    static int g_joinRandomPlaylistId = -1;
    static int g_joinRandomGameMode = -1;
    static std::string g_lastDebugGameModeName;
    static std::string g_lastDebugInstanceName;
    static std::string g_lastDebugMapName;
    static bool g_lastDebugRelevantToSessionInfoProbe = false;
    static bool g_loadSyncSawGroup1 = false;
    static bool g_loadSyncSawGroup2 = false;
    static bool g_loadSyncSawGroup3 = false;
    static bool g_autoMapCacheLoaded = false;
    static int g_autoMapCachedWinner = -1;

    struct GeneratedPhotonEventTrace
    {
        std::string label;
        unsigned long long peerKey = 0;
        unsigned int clientTime = 0;
        unsigned int reliableSequence = 0;
    };

    static std::vector<GeneratedPhotonEventTrace> g_generatedPhotonEventHistory;

    void NoteGeneratedPhotonEvent(
        const char* label,
        const sockaddr_in& remote,
        unsigned int clientTime,
        unsigned int reliableSequence)
    {
        GeneratedPhotonEventTrace trace;
        trace.label = label ? label : "UNKNOWN";
        trace.peerKey = PhotonSessionKey(remote);
        trace.clientTime = clientTime;
        trace.reliableSequence = reliableSequence;
        g_generatedPhotonEventHistory.push_back(trace);
        if (g_generatedPhotonEventHistory.size() > 16)
            g_generatedPhotonEventHistory.erase(g_generatedPhotonEventHistory.begin());

        std::printf("[PHOTON/TRACE] generated=%s seq=%u clientTime=%u peerKey=%llu\n",
                    trace.label.c_str(), trace.reliableSequence, trace.clientTime, trace.peerKey);
        std::fflush(stdout);
    }

    static void LogGeneratedPhotonEventCorrelation(unsigned int reportClientTime)
    {
        std::printf("[BOL/DEBUG-CORRELATION] reportClientTime=%u generatedEventCount=%u\n",
                    reportClientTime,
                    static_cast<unsigned int>(g_generatedPhotonEventHistory.size()));

        if (g_generatedPhotonEventHistory.empty())
        {
            std::printf("[BOL/DEBUG-CORRELATION] no server-generated PUN event has been recorded yet\n");
            return;
        }

        const GeneratedPhotonEventTrace& last = g_generatedPhotonEventHistory.back();
        const unsigned int delta = reportClientTime - last.clientTime;
        std::printf("[BOL/DEBUG-CORRELATION] lastServerEvent=%s seq=%u eventClientTime=%u delta=%u peerKey=%llu\n",
                    last.label.c_str(), last.reliableSequence, last.clientTime, delta, last.peerKey);

        const size_t begin = g_generatedPhotonEventHistory.size() > 8
            ? g_generatedPhotonEventHistory.size() - 8
            : 0;
        for (size_t i = begin; i < g_generatedPhotonEventHistory.size(); ++i)
        {
            const GeneratedPhotonEventTrace& item = g_generatedPhotonEventHistory[i];
            std::printf("[BOL/DEBUG-CORRELATION] recent[%u]=%s seq=%u eventClientTime=%u delta=%u peerKey=%llu\n",
                        static_cast<unsigned int>(i - begin),
                        item.label.c_str(),
                        item.reliableSequence,
                        item.clientTime,
                        reportClientTime - item.clientTime,
                        item.peerKey);
        }
        std::fflush(stdout);
    }

    enum class AutoChainStage
    {
        Session,
        Player,
        WaitingForNextRequest,
        MapReady,
        UnknownRequest
    };

    static AutoChainStage g_autoChainStage = AutoChainStage::Session;
    static std::string g_autoChainLastRpc;


    // One manifest for every Photon prefab we learn. Server-created prefabs
    // can use the safe count sweep; client-created PUN Event202 payloads already
    // contain the exact view array, so those can be learned immediately without
    // wasting reconnects.
    static bool g_autoPrefabCacheLoaded = false;
    static std::map<std::string, unsigned int> g_autoPrefabViewCounts;

    static std::string AutoPrefabCachePath()
    {
        char modulePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
        std::string path(modulePath, modulePath + (length > 0 ? length : 0));
        const size_t slash = path.find_last_of("\\/");
        if (slash != std::string::npos)
            path.resize(slash + 1);
        else
            path.clear();
        path += "bol_prefab_view_counts.txt";
        return path;
    }

    static std::string AutoMapCachePath()
    {
        char modulePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
        std::string path(modulePath, modulePath + (length > 0 ? length : 0));
        const size_t slash = path.find_last_of("\\/");
        if (slash != std::string::npos)
            path.resize(slash + 1);
        else
            path.clear();
        path += "bol_map_state_candidate.txt";
        return path;
    }

    static void LoadAutoMapCache()
    {
        if (g_autoMapCacheLoaded)
            return;
        g_autoMapCacheLoaded = true;

        FILE* file = nullptr;
        const std::string path = AutoMapCachePath();
        if (fopen_s(&file, path.c_str(), "rb") != 0 || !file)
            return;

        int index = -1;
        if (std::fscanf(file, "candidate=%d", &index) == 1)
        {
            const unsigned int count = static_cast<unsigned int>(sizeof(kAutoMapCandidates) / sizeof(kAutoMapCandidates[0]));
            if (index >= 0 && static_cast<unsigned int>(index) < count)
                g_autoMapCachedWinner = index;
        }
        std::fclose(file);
    }

    static void SaveAutoMapWinner(unsigned int candidateIndex)
    {
        const unsigned int count = static_cast<unsigned int>(sizeof(kAutoMapCandidates) / sizeof(kAutoMapCandidates[0]));
        if (candidateIndex >= count)
            return;

        FILE* file = nullptr;
        const std::string path = AutoMapCachePath();
        if (fopen_s(&file, path.c_str(), "wb") != 0 || !file)
            return;

        const AutoMapCandidate& candidate = kAutoMapCandidates[candidateIndex];
        std::fprintf(file, "candidate=%u\nmap=%s\nmode=%s\n",
                     candidateIndex, candidate.mapName, candidate.gameModeName);
        std::fclose(file);
        g_autoMapCachedWinner = static_cast<int>(candidateIndex);
        std::printf("[PHOTON/AUTO-MAP] cached winning SessionInfo candidate %u at %s\n",
                    candidateIndex + 1, path.c_str());
    }

    static void LoadAutoPrefabCache()
    {
        if (g_autoPrefabCacheLoaded)
            return;
        g_autoPrefabCacheLoaded = true;

        FILE* file = nullptr;
        const std::string path = AutoPrefabCachePath();
        if (fopen_s(&file, path.c_str(), "rb") != 0 || !file)
            return;

        char line[256] = {};
        while (std::fgets(line, sizeof(line), file))
        {
            char name[128] = {};
            unsigned int count = 0;
            if (std::sscanf(line, "%127[^=]=%u", name, &count) == 2 && count >= 1 && count <= 128)
                g_autoPrefabViewCounts[name] = count;
        }
        std::fclose(file);
    }

    static void SaveAutoPrefabCache()
    {
        LoadAutoPrefabCache();
        FILE* file = nullptr;
        const std::string path = AutoPrefabCachePath();
        if (fopen_s(&file, path.c_str(), "wb") != 0 || !file)
            return;
        for (const auto& item : g_autoPrefabViewCounts)
            std::fprintf(file, "%s=%u\n", item.first.c_str(), item.second);
        std::fclose(file);
    }

    static void RecordAutoPrefabCount(const std::string& prefabName, unsigned int viewCount, const char* source)
    {
        if (prefabName.empty() || viewCount == 0 || viewCount > 128)
            return;
        LoadAutoPrefabCache();
        const auto it = g_autoPrefabViewCounts.find(prefabName);
        const bool firstSeen = (it == g_autoPrefabViewCounts.end());
        const bool changed = firstSeen || it->second != viewCount;
        g_autoPrefabViewCounts[prefabName] = viewCount;
        if (changed)
            SaveAutoPrefabCache();

        std::printf("[PHOTON/AUTO-PREFAB] %s prefab=%s PhotonViewCount=%u source=%s\n",
                    firstSeen ? "learned" : "confirmed",
                    prefabName.c_str(), viewCount, source ? source : "unknown");
        if (firstSeen)
            std::printf("[PHOTON/AUTO-PREFAB] next new prefab gets its own independent count state; previous prefab counts stay cached\n");
        std::fflush(stdout);
    }

    static bool ObserveClientInstantiateRaiseEvent(const unsigned char* plain, int plainSize)
    {
        if (!plain || plainSize < 8 || plain[0] != 253)
            return false;

        // Photon RaiseEvent op253 carries EventCode in parameter 244. PUN
        // Instantiate is event 202. We only need a tiny Protocol16 scanner here
        // because the event-data Hashtable has stable byte keys 0 and 4.
        int eventCode = -1;
        for (int i = 3; i + 2 < plainSize; ++i)
        {
            if (plain[i] == 244 && plain[i + 1] == 0x62) // key244, GpType.Byte
            {
                eventCode = plain[i + 2];
                break;
            }
        }
        if (eventCode != 202)
            return false;

        std::string prefabName;
        unsigned int viewCount = 0;

        for (int i = 3; i + 5 < plainSize; ++i)
        {
            // Instantiate Hashtable byte key 0 -> string prefab name.
            if (plain[i] == 0x62 && plain[i + 1] == 0x00 && plain[i + 2] == 0x73)
            {
                const unsigned short length = ReadU16BE(plain + i + 3);
                if (length > 0 && i + 5 + static_cast<int>(length) <= plainSize)
                {
                    prefabName.assign(reinterpret_cast<const char*>(plain + i + 5), length);
                    break;
                }
            }
        }

        for (int i = 3; i + 6 < plainSize; ++i)
        {
            // Instantiate Hashtable byte key 4 -> Int32[] view IDs.
            if (plain[i] == 0x62 && plain[i + 1] == 0x04 && plain[i + 2] == 0x79)
            {
                viewCount = ReadU16BE(plain + i + 3);
                if (plain[i + 5] != 0x69)
                    viewCount = 0;
                break;
            }
        }

        if (prefabName.empty())
        {
            std::printf("[PHOTON/AUTO-PREFAB] client PUN Instantiate event detected, but prefab name could not be decoded\n");
            std::fflush(stdout);
            return true;
        }

        if (viewCount == 0)
        {
            std::printf("[PHOTON/AUTO-PREFAB] new client prefab detected: %s; waiting for a decodable view array before caching count\n",
                        prefabName.c_str());
            std::fflush(stdout);
            return true;
        }

        RecordAutoPrefabCount(prefabName, viewCount, "client Event202 exact view array");
        std::printf("[PHOTON/AUTO-PREFAB] no count sweep needed for %s because the client supplied the exact Event202 array\n",
                    prefabName.c_str());
        std::fflush(stdout);
        return true;
    }

    static std::string ExtractRpcName(const unsigned char* data, int size)
    {
        if (!data || size <= 0)
            return std::string();

        static const char kPrefix[] = "RPC_";
        const int prefixLength = 4;
        for (int i = 0; i + prefixLength <= size; ++i)
        {
            if (std::memcmp(data + i, kPrefix, prefixLength) != 0)
                continue;

            // Protocol16 strings are encoded as: 0x73, uint16 length, bytes.
            // Prefer that explicit length whenever the RPC_ text is the start
            // of a typed string. The old scanner kept consuming bytes while
            // they happened to be ASCII, so the following parameter key 0x62
            // was incorrectly appended as a literal 'b' (for example
            // RPC_RequestPlayerReady -> RPC_RequestPlayerReadyb).
            if (i >= 3 && data[i - 3] == 0x73)
            {
                const unsigned int stringLength =
                    (static_cast<unsigned int>(data[i - 2]) << 8) |
                    static_cast<unsigned int>(data[i - 1]);
                if (stringLength >= static_cast<unsigned int>(prefixLength) &&
                    stringLength <= 96 &&
                    i + static_cast<int>(stringLength) <= size)
                {
                    bool valid = true;
                    for (unsigned int j = 0; j < stringLength; ++j)
                    {
                        const unsigned char c = data[i + static_cast<int>(j)];
                        const bool allowed =
                            (c >= 'A' && c <= 'Z') ||
                            (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') ||
                            c == '_';
                        if (!allowed)
                        {
                            valid = false;
                            break;
                        }
                    }
                    if (valid)
                        return std::string(
                            reinterpret_cast<const char*>(data + i),
                            static_cast<size_t>(stringLength));
                }
            }

            // Fallback for malformed/debug payloads where the type/length
            // prefix is unavailable. Stop at the first non-name character.
            std::string name;
            for (int j = i; j < size && name.size() < 96; ++j)
            {
                const unsigned char c = data[j];
                const bool allowed =
                    (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '_';
                if (!allowed)
                    break;
                name.push_back(static_cast<char>(c));
            }
            if (name.size() > static_cast<size_t>(prefixLength))
                return name;
        }
        return std::string();
    }

    static void NoteAutoChainRpc(const std::string& rpcName)
    {
        if (rpcName.empty())
            return;
        if (rpcName == g_autoChainLastRpc)
            return;

        g_autoChainLastRpc = rpcName;
        std::printf("[PHOTON/AUTO-CHAIN] detected next room RPC: %s\n", rpcName.c_str());

        if (rpcName == "RPC_RequestPlayerReady")
        {
            g_autoChainStage = AutoChainStage::Player;
            std::printf("[PHOTON/AUTO-CHAIN] stage=Player; Player prefab uses the generic count probe (start/cached candidate -> clean retry -> cache winner)\n");
        }
        else if (rpcName == "RPC_RequestMapReady")
        {
            g_autoChainStage = AutoChainStage::MapReady;
            if (g_autoMapProbeActive)
            {
                std::printf("[PHOTON/SESSIONINFO-VIEW] SUCCESS: RPC_RequestMapReady arrived after SessionInfo viewId=%u\n",
                            g_sessionInfoViewProbeLastViewId);
                g_autoMapProbeActive = false;
                g_autoChainMapStatePending = false;
                g_autoMapProbeNextAt = 0;
                g_sessionInfoViewProbeNextAt = 0;
                g_sessionInfoExactProbeStage = SessionInfoExactProbeStage::Complete;
            }
            std::printf("[PHOTON/SESSIONINFO-EXACT] SUCCESS: client emitted RPC_RequestMapReady; SessionInfo -> OnMapChange -> load/sync path advanced\n");
            std::printf("[PHOTON/AUTO-CHAIN] stage=MapReady; map-state probe resolved and AUTO-CHAIN will continue from the client's real RPC\n");
        }
        else
        {
            g_autoChainStage = AutoChainStage::UnknownRequest;
            std::printf("[PHOTON/AUTO-CHAIN] new stage discovered automatically; if it emits PUN Event202 the prefab/count will be learned automatically, otherwise the RPC is preserved for the server-side state handler\n");
        }
        std::fflush(stdout);
    }

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
        RecordAutoPrefabCount("Session", g_sessionViewProbeCandidateUsed, "safe server-owned probe");
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

    static bool FindProtocol16StringKeyIntValue(
        const unsigned char* data,
        int size,
        const char* key,
        int& value)
    {
        if (!data || size <= 0 || !key || !*key)
            return false;

        const size_t keyLength = std::strlen(key);
        if (keyLength > 0xFFFFu)
            return false;

        for (int i = 0; i + 3 + static_cast<int>(keyLength) + 5 <= size; ++i)
        {
            if (data[i] != 0x73) // Protocol16 String
                continue;
            if (ReadU16BE(data + i + 1) != keyLength)
                continue;
            if (std::memcmp(data + i + 3, key, keyLength) != 0)
                continue;

            const int valueOffset = i + 3 + static_cast<int>(keyLength);
            if (data[valueOffset] != 0x69) // Protocol16 Int32
                continue;

            value = static_cast<int>(ReadU32BE(data + valueOffset + 1));
            return true;
        }
        return false;
    }

    static const char* RuntimeGameModeNameFromMatchmakingValue(int gameMode)
    {
        // XsdSettings.GamemodeType from Assembly-CSharp:
        // 0=None, 1=Town, 2=PvE, 3=Survival, 4=Deathmatch,
        // 5=TeamDeathmatch, 6=Tutorial.
        switch (gameMode)
        {
        case 1: return "GameModeTown";
        case 2: return "GamemodePvE";
        case 3: return "GameModeSurvival";
        case 4: return "GameModePvP";
        case 5: return "GamemodeTeamDeathmatch";
        case 6: return "GamemodeTutorial";
        default: return "GamemodePvE";
        }
    }

    static void CaptureJoinRandomMatchmakingState(const unsigned char* operation, int operationSize)
    {
        int playlistId = -1;
        int gameMode = -1;
        if (FindProtocol16StringKeyIntValue(operation, operationSize, "PlaylistId", playlistId))
            g_joinRandomPlaylistId = playlistId;
        if (FindProtocol16StringKeyIntValue(operation, operationSize, "GameMode", gameMode))
            g_joinRandomGameMode = gameMode;

        std::printf("[PHOTON/MATCHMAKING] captured JoinRandom properties: PlaylistId=%d GameMode=%d runtimeMode=%s\n",
                    g_joinRandomPlaylistId,
                    g_joinRandomGameMode,
                    RuntimeGameModeNameFromMatchmakingValue(g_joinRandomGameMode));
        if (g_joinRandomGameMode == 1)
            std::printf("[PHOTON/MATCHMAKING] GameMode=1 is Town; playlist fallback name is lvl_Floasm_Small, but online LevelLoadingManager resolves Session.mapName against MapsDesc <Map name=...>\n");
            std::printf("[PHOTON/MATCHMAKING] v57 will scan the installed Unity resources and audit every MapsDesc-declared bundle for the exact Floasm MapsDesc/playlist map key before falling back to Floasm\n");
        std::fflush(stdout);
    }

    static const char* GetEnvironmentOverride(const char* name, const char* fallback)
    {
        const char* value = std::getenv(name);
        return (value && *value) ? value : fallback;
    }

    static std::string ExecutableDirectory()
    {
        char path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return ".";
        std::string result(path, path + length);
        const std::string::size_type slash = result.find_last_of("\\/");
        if (slash == std::string::npos)
            return ".";
        result.resize(slash);
        return result;
    }

    static bool FileExistsA(const std::string& path)
    {
        const DWORD attrs = GetFileAttributesA(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    static bool ExtractQuotedAttribute(const std::string& text,
                                       std::string::size_type start,
                                       std::string::size_type end,
                                       const char* attribute,
                                       std::string& value)
    {
        if (!attribute || start >= text.size())
            return false;
        end = std::min(end, text.size());
        const std::string key = std::string(attribute) + "=";
        std::string::size_type pos = text.find(key, start);
        if (pos == std::string::npos || pos >= end)
            return false;
        pos += key.size();
        if (pos >= end || (text[pos] != '"' && text[pos] != '\''))
            return false;
        const char quote = text[pos++];
        const std::string::size_type close = text.find(quote, pos);
        if (close == std::string::npos || close > end)
            return false;
        value.assign(text, pos, close - pos);
        return !value.empty();
    }

    static bool TryExtractMapKeyAroundScene(const std::string& data,
                                            std::string::size_type scenePos,
                                            std::string& mapKey)
    {
        // MapsDesc is an XML TextAsset. Find a real <Map ...> node enclosing
        // the target scene reference and read its name attribute. Ignore
        // similarly named nodes such as <MapSetting>.
        const std::string::size_type mapStart = data.rfind("<Map ", scenePos);
        if (mapStart == std::string::npos || scenePos - mapStart > 256 * 1024)
            return false;
        const std::string::size_type mapOpenEnd = data.find('>', mapStart);
        const std::string::size_type mapEnd = data.find("</Map>", scenePos);
        if (mapOpenEnd == std::string::npos || mapEnd == std::string::npos || mapOpenEnd > scenePos || mapEnd - scenePos > 256 * 1024)
            return false;
        const std::string::size_type bundlePos = data.find("AssetBundle", mapStart);
        if (bundlePos == std::string::npos || bundlePos > mapEnd)
            return false;
        return ExtractQuotedAttribute(data, mapStart, mapOpenEnd, "name", mapKey);
    }

    static bool TryExtractPlaylistSceneAroundName(const std::string& data,
                                                   std::string::size_type namePos,
                                                   std::string& sceneKey)
    {
        // Settings/Playlist is another XML TextAsset. The town playlist is
        // named lvl_Floasm_Small; its m_sceneToPlay/m_scene entry is the
        // logical map identifier consumed by Session.mapName.
        const std::string::size_type end = std::min(data.size(), namePos + 128 * 1024);
        const std::string::size_type listPos = data.find("m_sceneToPlay", namePos);
        if (listPos == std::string::npos || listPos >= end)
            return false;
        std::string::size_type sceneOpen = data.find("<m_scene>", listPos);
        if (sceneOpen != std::string::npos && sceneOpen < end)
        {
            sceneOpen += std::strlen("<m_scene>");
            const std::string::size_type close = data.find("</m_scene>", sceneOpen);
            if (close == std::string::npos || close >= end)
                return false;
            sceneKey.assign(data, sceneOpen, close - sceneOpen);
            return !sceneKey.empty();
        }

        sceneOpen = data.find("<scene>", listPos);
        if (sceneOpen == std::string::npos || sceneOpen >= end)
            return false;
        sceneOpen += std::strlen("<scene>");
        const std::string::size_type close = data.find("</scene>", sceneOpen);
        if (close == std::string::npos || close >= end)
            return false;
        sceneKey.assign(data, sceneOpen, close - sceneOpen);
        return !sceneKey.empty();
    }


    static unsigned long long FileSizeA(const std::string& path)
    {
        std::ifstream file(path.c_str(), std::ios::binary | std::ios::ate);
        if (!file)
            return 0;
        const std::streampos end = file.tellg();
        if (end <= 0)
            return 0;
        return static_cast<unsigned long long>(end);
    }

    static bool BinaryFileContains(const std::string& path, const std::string& needle)
    {
        if (needle.empty())
            return false;
        std::ifstream file(path.c_str(), std::ios::binary);
        if (!file)
            return false;

        constexpr std::size_t kChunk = 1024 * 1024;
        std::vector<char> buffer(kChunk);
        std::string carry;
        const std::size_t carrySize = needle.size() > 1 ? needle.size() - 1 : 0;
        while (file)
        {
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            if (count <= 0)
                break;
            std::string data = carry;
            data.append(buffer.data(), static_cast<std::size_t>(count));
            if (data.find(needle) != std::string::npos)
                return true;
            if (carrySize != 0 && data.size() > carrySize)
                carry.assign(data.end() - static_cast<std::ptrdiff_t>(carrySize), data.end());
            else
                carry = data;
        }
        return false;
    }

    static void AuditFloasmMapContent(const std::string& data,
                                      std::string::size_type scenePos,
                                      const std::string& sourcePath)
    {
        const std::string::size_type mapStart = data.rfind("<Map ", scenePos);
        const std::string::size_type mapEnd = data.find("</Map>", scenePos);
        if (mapStart == std::string::npos || mapEnd == std::string::npos || mapEnd <= mapStart)
            return;

        const std::string::size_type mapOpenEnd = data.find('>', mapStart);
        if (mapOpenEnd == std::string::npos || mapOpenEnd > mapEnd)
            return;

        std::string mapName;
        ExtractQuotedAttribute(data, mapStart, mapOpenEnd, "name", mapName);
        std::printf("[PHOTON/MAP-ASSET-AUDIT] MapsDesc node found: map=%s source=%s xmlBytes~%llu\n",
                    mapName.empty() ? "<unknown>" : mapName.c_str(),
                    sourcePath.c_str(),
                    static_cast<unsigned long long>(mapEnd + std::strlen("</Map>") - mapStart));

        const std::string root = ExecutableDirectory();
        const std::string bundleRoot = root + "\\assetbundles\\";
        unsigned int bundleCount = 0;
        unsigned int presentCount = 0;
        unsigned int missingCount = 0;
        unsigned int mapBundleCount = 0;
        bool sceneSeenInsideMapBundle = false;

        std::string::size_type cursor = mapStart;
        while ((cursor = data.find("<AssetBundle", cursor)) != std::string::npos && cursor < mapEnd)
        {
            const std::string::size_type tagEnd = data.find('>', cursor);
            if (tagEnd == std::string::npos || tagEnd > mapEnd)
                break;

            std::string bundleName;
            std::string bundleType;
            std::string mapScene;
            std::string behaviour;
            ExtractQuotedAttribute(data, cursor, tagEnd, "name", bundleName);
            ExtractQuotedAttribute(data, cursor, tagEnd, "type", bundleType);
            ExtractQuotedAttribute(data, cursor, tagEnd, "map", mapScene);
            ExtractQuotedAttribute(data, cursor, tagEnd, "behaviour", behaviour);

            if (!bundleName.empty())
            {
                ++bundleCount;
                const std::string bundlePath = bundleRoot + bundleName;
                const bool exists = FileExistsA(bundlePath);
                const unsigned long long size = exists ? FileSizeA(bundlePath) : 0;
                if (exists)
                    ++presentCount;
                else
                    ++missingCount;

                bool containsScene = false;
                if (exists && !mapScene.empty())
                {
                    ++mapBundleCount;
                    containsScene = BinaryFileContains(bundlePath, mapScene);
                    if (containsScene)
                        sceneSeenInsideMapBundle = true;
                }

                std::printf("[PHOTON/MAP-ASSET-AUDIT] bundle[%u] name=%s type=%s map=%s behaviour=%s file=%s size=%llu",
                            bundleCount,
                            bundleName.c_str(),
                            bundleType.empty() ? "-" : bundleType.c_str(),
                            mapScene.empty() ? "-" : mapScene.c_str(),
                            behaviour.empty() ? "-" : behaviour.c_str(),
                            exists ? "FOUND" : "MISSING",
                            size);
                if (!mapScene.empty())
                    std::printf(" sceneStringInBundle=%s", containsScene ? "YES" : "NO");
                std::printf("\n");
            }
            cursor = tagEnd + 1;
        }

        const std::string globalManagers = root + "\\BOL_Data\\globalgamemanagers";
        const bool globalManagersExists = FileExistsA(globalManagers);
        const bool hasResourcesLoading = globalManagersExists && BinaryFileContains(globalManagers, "ResourcesLoading");

        // Unity 4-era Windows players may store the build scene table in mainData
        // instead of globalgamemanagers.  A permanent 0%% loading screen is
        // consistent with LevelLoadingStep_Init waiting on
        // Application.LoadLevelAsync("ResourcesLoading"), so inspect every
        // plausible legacy scene-table/data file and report where the scene
        // name actually exists.
        struct SceneTableCandidate
        {
            const char* relativePath;
            const char* label;
        };
        const SceneTableCandidate sceneCandidates[] = {
            { "\\BOL_Data\\globalgamemanagers", "globalgamemanagers" },
            { "\\BOL_Data\\mainData", "mainData" },
            { "\\BOL_Data\\level0", "level0" },
            { "\\BOL_Data\\level1", "level1" },
            { "\\BOL_Data\\level2", "level2" },
            { "\\BOL_Data\\level3", "level3" },
            { "\\BOL_Data\\level4", "level4" },
            { "\\BOL_Data\\level5", "level5" },
            { "\\BOL_Data\\resources.assets", "resources.assets" }
        };
        bool resourcesLoadingSeenAnywhere = false;
        bool frontEndSeenAnywhere = false;
        bool floasmSeenInLegacyData = false;
        unsigned int sceneFileCount = 0;
        for (const SceneTableCandidate& candidate : sceneCandidates)
        {
            const std::string candidatePath = root + candidate.relativePath;
            const bool exists = FileExistsA(candidatePath);
            if (!exists)
                continue;
            ++sceneFileCount;
            const unsigned long long size = FileSizeA(candidatePath);
            const bool hasBootstrap = BinaryFileContains(candidatePath, "ResourcesLoading");
            const bool hasFrontEnd = BinaryFileContains(candidatePath, "FrontEnd");
            const bool hasFloasm = BinaryFileContains(candidatePath, "lvl_Floasm_Small");
            resourcesLoadingSeenAnywhere = resourcesLoadingSeenAnywhere || hasBootstrap;
            frontEndSeenAnywhere = frontEndSeenAnywhere || hasFrontEnd;
            floasmSeenInLegacyData = floasmSeenInLegacyData || hasFloasm;
            std::printf("[PHOTON/UNITY-SCENE-AUDIT] file=%s size=%llu ResourcesLoading=%s FrontEnd=%s FloasmKey=%s\n",
                        candidate.label, size,
                        hasBootstrap ? "YES" : "NO",
                        hasFrontEnd ? "YES" : "NO",
                        hasFloasm ? "YES" : "NO");
        }

        std::printf("[PHOTON/MAP-ASSET-AUDIT] SUMMARY requiredBundles=%u found=%u missing=%u mapBundles=%u mapSceneStringSeen=%s\n",
                    bundleCount, presentCount, missingCount, mapBundleCount,
                    sceneSeenInsideMapBundle ? "YES" : "NO");
        std::printf("[PHOTON/MAP-ASSET-AUDIT] loader bootstrap legacy check: globalgamemanagers=%s directResourcesLoading=%s candidateFiles=%u anyResourcesLoading=%s\n",
                    globalManagersExists ? "FOUND" : "MISSING",
                    hasResourcesLoading ? "YES" : "NO",
                    sceneFileCount,
                    resourcesLoadingSeenAnywhere ? "YES" : "NO");
        if (resourcesLoadingSeenAnywhere)
        {
            std::printf("[PHOTON/UNITY-SCENE-AUDIT] RESULT: ResourcesLoading exists in this client data; a persistent 0%% stall is after/between Unity bootstrap and LevelLoadingStep progression, not a missing bootstrap-scene string\n");
        }
        else
        {
            std::printf("[PHOTON/UNITY-SCENE-AUDIT] RESULT: ResourcesLoading string was NOT found in globalgamemanagers/mainData/level0-5/resources.assets; this build may use another scene-table file or lack the bootstrap scene expected by this Assembly-CSharp\n");
        }
        std::printf("[PHOTON/UNITY-SCENE-AUDIT] context: FrontEndSeen=%s FloasmKeySeen=%s\n",
                    frontEndSeenAnywhere ? "YES" : "NO",
                    floasmSeenInLegacyData ? "YES" : "NO");
        if (bundleCount != 0 && missingCount != 0)
        {
            std::printf("[PHOTON/MAP-ASSET-AUDIT] RESULT: client install is missing %u/%u asset bundle file(s) required by this MapsDesc node; server emulation cannot make those Unity assets appear\n",
                        missingCount, bundleCount);
        }
        else if (bundleCount != 0)
        {
            std::printf("[PHOTON/MAP-ASSET-AUDIT] RESULT: all MapsDesc-declared bundle files are present; if loading still stalls, trace the client LevelLoadingStepController/ResourcesLoading scene next\n");
        }
        else
        {
            std::printf("[PHOTON/MAP-ASSET-AUDIT] RESULT: no AssetBundle tags were parsed from the enclosing map node; audit parser could not validate the client content set\n");
        }
        std::fflush(stdout);
    }

    static bool ScanUnityAssetForFloasmMap(const std::string& path, std::string& mapKey, std::string& source)
    {
        std::ifstream file(path.c_str(), std::ios::binary);
        if (!file)
            return false;

        constexpr std::size_t kChunkSize = 1024 * 1024;
        constexpr std::size_t kCarrySize = 512 * 1024;
        std::vector<char> buffer(kChunkSize);
        std::string carry;
        carry.reserve(kCarrySize);
        const std::string needle = "lvl_Floasm_Small";
        unsigned long long totalRead = 0;

        while (file)
        {
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            if (count <= 0)
                break;
            totalRead += static_cast<unsigned long long>(count);

            std::string data = carry;
            data.append(buffer.data(), static_cast<std::size_t>(count));
            std::string::size_type pos = 0;
            while ((pos = data.find(needle, pos)) != std::string::npos)
            {
                std::string candidate;
                if (TryExtractMapKeyAroundScene(data, pos, candidate))
                {
                    mapKey = candidate;
                    source = "Maps/MapsDesc <Map name> enclosing lvl_Floasm_Small";
                    AuditFloasmMapContent(data, pos, path);
                    std::printf("[PHOTON/MAP-RESOURCE] resolved MapsDesc key '%s' from %s at approx offset %llu\n",
                                mapKey.c_str(), path.c_str(),
                                totalRead - static_cast<unsigned long long>(count) + static_cast<unsigned long long>(pos));
                    std::fflush(stdout);
                    return true;
                }
                if (TryExtractPlaylistSceneAroundName(data, pos, candidate))
                {
                    mapKey = candidate;
                    source = "Settings/Playlist m_sceneToPlay for lvl_Floasm_Small";
                    std::printf("[PHOTON/MAP-RESOURCE] resolved playlist scene key '%s' from %s at approx offset %llu\n",
                                mapKey.c_str(), path.c_str(),
                                totalRead - static_cast<unsigned long long>(count) + static_cast<unsigned long long>(pos));
                    std::fflush(stdout);
                    return true;
                }
                pos += needle.size();
            }

            if (data.size() > kCarrySize)
                carry.assign(data.end() - static_cast<std::ptrdiff_t>(kCarrySize), data.end());
            else
                carry = data;
        }
        return false;
    }

    static bool ResolveFloasmMapKeyFromInstalledClient(std::string& mapKey, std::string& source)
    {
        const std::string root = ExecutableDirectory();
        std::vector<std::string> candidates;
        candidates.push_back(root + "\\BOL_Data\\resources.assets");
        candidates.push_back(root + "\\resources.assets");

        // Resources.Load("Maps/MapsDesc") normally lives in resources.assets.
        // Fall back to a few shared assets for builds packed differently.
        for (int i = 0; i < 8; ++i)
            candidates.push_back(root + "\\BOL_Data\\sharedassets" + std::to_string(i) + ".assets");

        bool scannedAnything = false;
        for (const std::string& path : candidates)
        {
            if (!FileExistsA(path))
                continue;
            scannedAnything = true;
            std::printf("[PHOTON/MAP-RESOURCE] scanning %s for MapsDesc/Playlist reference to lvl_Floasm_Small\n", path.c_str());
            std::fflush(stdout);
            if (ScanUnityAssetForFloasmMap(path, mapKey, source))
                return true;
        }

        if (!scannedAnything)
            std::printf("[PHOTON/MAP-RESOURCE] no resources.assets/sharedassets*.assets found beside BOLEmulator.exe\n");
        else
            std::printf("[PHOTON/MAP-RESOURCE] lvl_Floasm_Small was not found inside a parseable MapsDesc/Playlist XML block; using fallback key\n");
        std::fflush(stdout);
        return false;
    }

    static void HandleAutoChainPlayerReadyAck(
        const sockaddr_in& remote,
        unsigned int acknowledgedSequence,
        unsigned int clientTime)
    {
        if (!g_autoChainPlayerReadyAwaitingAck ||
            acknowledgedSequence != g_autoChainPlayerReadySequence ||
            PhotonSessionKey(remote) != g_playerViewProbeGameServerKey)
        {
            return;
        }

        g_autoChainPlayerReadyAwaitingAck = false;
        g_autoMapProbeActive = true;
        g_autoMapProbeNextAt = 0;
        g_sessionInfoViewProbeAttempt = 0;
        g_sessionInfoViewProbeLastViewId = 0;
        g_sessionInfoViewProbeNextAt = 0;
        g_autoChainMapStatePending = true;
        g_autoChainMapStateNotBefore = clientTime + 3000;

        // IMPORTANT: Session.mapName is not the Unity scene/asset-bundle map field in normal online mode.
        // Decompiled LevelLoadingManager.MapsDescXml.c840... matches only <Map name="...">
        // unless AppManager.m_offlineMode is true. The playlist fallback is named
        // lvl_Floasm_Small, while UI/world-map identifiers consistently call the town Floasm.
        // Use the logical MapsDesc key by default and keep BOL_MAP_NAME as an exact override.
        const char* inferredMap = (g_joinRandomGameMode == 1) ? "Floasm" : "Floasm";
        const char* inferredMode = RuntimeGameModeNameFromMatchmakingValue(g_joinRandomGameMode);
        const char* explicitMapOverride = std::getenv("BOL_MAP_NAME");
        if (explicitMapOverride && *explicitMapOverride)
        {
            g_sessionInfoProbeMapName = explicitMapOverride;
            std::printf("[PHOTON/MAP-RESOURCE] BOL_MAP_NAME override wins: %s\n", g_sessionInfoProbeMapName.c_str());
        }
        else
        {
            std::string detectedMap;
            std::string detectedSource;
            if (ResolveFloasmMapKeyFromInstalledClient(detectedMap, detectedSource))
            {
                g_sessionInfoProbeMapName = detectedMap;
                std::printf("[PHOTON/MAP-RESOURCE] AUTO map key selected: %s (source=%s)\n",
                            g_sessionInfoProbeMapName.c_str(), detectedSource.c_str());
            }
            else
            {
                g_sessionInfoProbeMapName = inferredMap;
                std::printf("[PHOTON/MAP-RESOURCE] AUTO extraction unavailable; fallback MapsDesc key=%s\n",
                            g_sessionInfoProbeMapName.c_str());
            }
        }
        g_sessionInfoProbeGameMode = GetEnvironmentOverride("BOL_GAME_MODE", inferredMode);
        g_sessionInfoExactProbeStage = SessionInfoExactProbeStage::WaitForContractBaseline;
        g_sessionInfoExactProbeNextAt = clientTime + 3000;
        g_sessionInfoExactProbeStartedAt = clientTime;
        g_sessionInfoExactProbeLastStatusAt = clientTime;
        g_lastDebugGameModeName.clear();
        g_lastDebugInstanceName.clear();
        g_lastDebugMapName.clear();
        g_loadSyncSawGroup1 = false;
        g_loadSyncSawGroup2 = false;
        g_loadSyncSawGroup3 = false;

        std::printf("[PHOTON/SESSIONINFO-EXACT] PlayerReady ACKed seq=%u; exact Assembly-CSharp contract probe armed\n",
                    acknowledgedSequence);
        std::printf("[PHOTON/SESSIONINFO-EXACT] target viewId=1005 (only Session view that executed the Event206 path in v49-v51)\n");
        std::printf("[PHOTON/SESSIONINFO-EXACT] matchmaking context: PlaylistId=%d GameMode=%d -> runtimeMode=%s\n",
                    g_joinRandomPlaylistId, g_joinRandomGameMode, g_sessionInfoProbeGameMode.c_str());
        std::printf("[PHOTON/SESSIONINFO-EXACT] phase A after 3s: exact 11-value SessionInfo packet with mapLoadingCount=0 (NO map change)\n");
        std::printf("[PHOTON/SESSIONINFO-EXACT] phase B only if A survives 8s: same packet with count=1 map=%s mode=%s\n",
                    g_sessionInfoProbeMapName.c_str(), g_sessionInfoProbeGameMode.c_str());
        std::printf("[PHOTON/SESSIONINFO-EXACT] online loader contract: mapName must equal Maps/MapsDesc <Map name=...>, NOT AssetBundle @map / Unity scene name\n");
        std::printf("[PHOTON/SESSIONINFO-EXACT] map override: set BOL_MAP_NAME=<exact MapsDesc Map name>; mode override: BOL_GAME_MODE=<runtime Gamemode class>\n");
        std::fflush(stdout);
    }

    static void MaybeSendAutoChainMapState(
        SOCKET server,
        const sockaddr_in& remote,
        unsigned int clientTime)
    {
        if (!g_autoMapProbeActive ||
            PhotonSessionKey(remote) != g_playerViewProbeGameServerKey ||
            g_sessionInfoExactProbeStage == SessionInfoExactProbeStage::Inactive ||
            g_sessionInfoExactProbeStage == SessionInfoExactProbeStage::Complete)
        {
            return;
        }

        constexpr unsigned int kSessionInfoViewId = 1005;

        if (g_sessionInfoExactProbeStage == SessionInfoExactProbeStage::WaitForContractBaseline)
        {
            if (clientTime < g_sessionInfoExactProbeNextAt)
                return;

            std::printf("[PHOTON/SESSIONINFO-EXACT] PHASE A: sending count=0 contract baseline to viewId=%u; this must NOT call Session.SetMapAndModeName\n", kSessionInfoViewId);
            const bool sent = SendPhotonSessionMapStateSerializeEvent(
                server, remote, g_playerViewProbeGameServerRemoteLength, clientTime,
                g_playerViewProbeGameServerChallenge, g_playerViewProbeEncrypted,
                kSessionInfoViewId, "", g_sessionInfoProbeGameMode.c_str(), 0);
            if (!sent)
            {
                std::printf("[PHOTON/SESSIONINFO-EXACT] PHASE A send failed; probe stopped\n");
                g_autoMapProbeActive = false;
                g_sessionInfoExactProbeStage = SessionInfoExactProbeStage::Complete;
                return;
            }

            g_sessionInfoViewProbeLastViewId = kSessionInfoViewId;
            g_sessionInfoExactProbeStage = SessionInfoExactProbeStage::AwaitContractBaseline;
            g_sessionInfoExactProbeNextAt = clientTime + 8000;
            g_sessionInfoExactProbeLastStatusAt = clientTime;
            std::printf("[PHOTON/SESSIONINFO-EXACT] PHASE A sent; waiting 8s for Photon OnEvent/debug failure\n");
            std::fflush(stdout);
            return;
        }

        if (g_sessionInfoExactProbeStage == SessionInfoExactProbeStage::AwaitContractBaseline)
        {
            if (clientTime < g_sessionInfoExactProbeNextAt)
                return;

            std::printf("[PHOTON/SESSIONINFO-EXACT] PHASE A PASS: view1005 accepted the exact SessionInfo contract with count=0\n");
            std::printf("[PHOTON/SESSIONINFO-EXACT] this isolates the old NRE to the map-change/load path rather than Event206 framing/11-value decoding\n");
            std::printf("[PHOTON/SESSIONINFO-EXACT] PHASE B: sending count=1 map trigger map=%s mode=%s\n",
                        g_sessionInfoProbeMapName.c_str(), g_sessionInfoProbeGameMode.c_str());

            const bool sent = SendPhotonSessionMapStateSerializeEvent(
                server, remote, g_playerViewProbeGameServerRemoteLength, clientTime,
                g_playerViewProbeGameServerChallenge, g_playerViewProbeEncrypted,
                kSessionInfoViewId, g_sessionInfoProbeMapName.c_str(),
                g_sessionInfoProbeGameMode.c_str(), 1);
            if (!sent)
            {
                std::printf("[PHOTON/SESSIONINFO-EXACT] PHASE B send failed; probe stopped\n");
                g_autoMapProbeActive = false;
                g_sessionInfoExactProbeStage = SessionInfoExactProbeStage::Complete;
                return;
            }

            g_sessionInfoExactProbeStage = SessionInfoExactProbeStage::AwaitMapTrigger;
            g_sessionInfoExactProbeNextAt = clientTime + 60000;
            g_sessionInfoExactProbeLastStatusAt = clientTime;
            std::printf("[PHOTON/SESSIONINFO-EXACT] PHASE B sent; waiting for RPC_RequestMapReady or a correlated client debug report\n");
            std::fflush(stdout);
            return;
        }

        if (g_sessionInfoExactProbeStage == SessionInfoExactProbeStage::AwaitMapTrigger)
        {
            if (clientTime - g_sessionInfoExactProbeLastStatusAt >= 5000)
            {
                const unsigned int elapsed = clientTime - (g_sessionInfoExactProbeNextAt - 60000);
                std::printf("[PHOTON/SESSIONINFO-EXACT] PHASE B waiting: %us/60s MapsDescKey=%s; StaticSync(group1)=%s DynamicSync(group2)=%s MapReady=NO\n",
                            elapsed / 1000, g_sessionInfoProbeMapName.c_str(),
                            g_loadSyncSawGroup1 ? "YES" : "NO", g_loadSyncSawGroup2 ? "YES" : "NO");
                g_sessionInfoExactProbeLastStatusAt = clientTime;
                std::fflush(stdout);
            }

            if (clientTime >= g_sessionInfoExactProbeNextAt)
            {
                std::printf("[PHOTON/SESSIONINFO-EXACT] PHASE B TIMEOUT: exact SessionInfo packet survived 60s but RPC_RequestMapReady did not arrive\n");
                if (!g_loadSyncSawGroup1)
                    std::printf("[PHOTON/SESSIONINFO-EXACT] RESULT: no ChangeGroups add=1 was observed, so the client never reached StaticSynchronizing; inspect [GAME/LOADTRACE] / bol_revive.log [LOADTRACE] to distinguish pre-LoadMap vs Unity bootstrap/bundle stall\n");
                else
                    std::printf("[PHOTON/SESSIONINFO-EXACT] RESULT: StaticSynchronizing was reached but MapReady did not arrive; continue with group2/group3 and PlayerInfoSync synchronization\n");
                g_autoMapProbeActive = false;
                g_autoChainMapStatePending = false;
                g_sessionInfoExactProbeStage = SessionInfoExactProbeStage::Complete;
                std::fflush(stdout);
            }
        }
    }

    static void MaybeConfirmPlayerViewProbe(SOCKET server, const sockaddr_in& remote, unsigned int clientTime)
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
        RecordAutoPrefabCount("Player", g_playerViewProbeCandidateUsed, "safe server-owned probe");
        std::printf("[PHOTON/AUTO-PLAYER] Player candidate %u survived the mismatch window; Player prefab instantiation is now the active path\n",
                    g_playerViewProbeCandidateUsed);

        if (!SendPhotonPlayerReadySerializeEvent(
                server,
                g_playerViewProbeGameServerRemote,
                g_playerViewProbeGameServerRemoteLength,
                clientTime,
                g_playerViewProbeGameServerChallenge,
                g_playerViewProbeEncrypted))
        {
            std::printf("[PHOTON/AUTO-CHAIN] Player count is correct, but authoritative PlayerReady state send failed\n");
            g_autoChainPlayerReadyAwaitingAck = false;
            g_autoChainPlayerReadySequence = 0;
            g_autoChainMapStatePending = false;
        }
        else
        {
            PhotonSessionState& gameSession = GetPhotonSession(g_playerViewProbeGameServerRemote);
            g_autoChainPlayerReadySequence =
                gameSession.serverReliableSequenceCh0 > 0 ? gameSession.serverReliableSequenceCh0 - 1 : 0;
            g_autoChainPlayerReadyAwaitingAck = g_autoChainPlayerReadySequence != 0;
            g_autoChainMapStatePending = false;
            g_autoChainMapStateNotBefore = 0;
            g_autoMapProbeActive = false;
            g_autoMapProbeAttempts = 0;
            g_autoMapProbeNextAt = 0;
            g_sessionInfoViewProbeAttempt = 0;
            g_sessionInfoViewProbeLastViewId = 0;
            g_sessionInfoViewProbeNextAt = 0;
            std::printf("[PHOTON/AUTO-CHAIN] waiting for PlayerReady ACK seq=%u before starting AUTO-MAP SessionInfo discovery\n",
                        g_autoChainPlayerReadySequence);
        }

        g_autoChainStage = AutoChainStage::WaitingForNextRequest;
        std::printf("[PHOTON/AUTO-CHAIN] Player resolved; automatically watching RaiseEvent traffic for the next RPC/prefab stage\n");
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

        // Do not hard-code progression to one RPC anymore. Every room
        // RaiseEvent is inspected for its PUN RPC name so the bootstrap can
        // automatically identify which stage BOL reached next. Count probing
        // is only used for actual prefabs; RPC-only state transitions are
        // deliberately not treated as PhotonView-count problems.
        const bool sawClientInstantiate = ObserveClientInstantiateRaiseEvent(plain, plainSize);
        const std::string rpcName = ExtractRpcName(plain, plainSize);
        if (!rpcName.empty())
            NoteAutoChainRpc(rpcName);
        else if (sawClientInstantiate)
        {
            g_autoChainStage = AutoChainStage::WaitingForNextRequest;
            std::printf("[PHOTON/AUTO-CHAIN] client-created prefab learned; continuing to watch for the next room stage\n");
        }

        if (rpcName != "RPC_RequestPlayerReady")
        {
            if (!rpcName.empty())
                std::printf("[PHOTON/UDP] RaiseEvent captured after bootstrap: %s\n", rpcName.c_str());
            else
                std::printf("[PHOTON/UDP] RaiseEvent payload captured after bootstrap (no RPC name decoded)\n");
            std::fflush(stdout);
            return;
        }

        // Player is currently the first discovered post-Session prefab. Its
        // probe starts at its own cache (1 on a fresh install), completely
        // independent from Session's known-good count of 6. Once accepted,
        // AUTO-CHAIN goes back to watching for the next request.
        std::printf("[PHOTON/AUTO-PLAYER] RPC_RequestPlayerReady captured for characterId=%u; creating LOCAL actor-2 Player prefab\n",
                    g_localCharacter.id);
        BeginPlayerViewProbe(
            server, remote, remoteLength, clientTime, challenge, encrypted);
    }
    static void LogChangeGroupsOperation(const unsigned char* plain, int plainSize)
    {
        if (!plain || plainSize < 3 || plain[0] != 248)
            return;

        const unsigned short parameterCount = ReadU16BE(plain + 1);
        int offset = 3;
        std::vector<unsigned int> addedGroups;
        std::vector<unsigned int> removedGroups;

        for (unsigned int i = 0; i < parameterCount && offset + 2 <= plainSize; ++i)
        {
            const unsigned char key = plain[offset++];
            const unsigned char type = plain[offset++];

            // Photon GpType.ByteArray. ChangeGroups uses ParameterCode.Add=238
            // and ParameterCode.Remove=239, each as byte[].
            if (type == 0x78 && offset + 4 <= plainSize)
            {
                const unsigned int length = ReadU32BE(plain + offset);
                offset += 4;
                if (length > static_cast<unsigned int>(plainSize - offset))
                    break;

                std::vector<unsigned int>& target = (key == 239) ? removedGroups : addedGroups;
                if (key == 238 || key == 239)
                {
                    for (unsigned int j = 0; j < length; ++j)
                        target.push_back(static_cast<unsigned int>(plain[offset + static_cast<int>(j)]));
                }
                offset += static_cast<int>(length);
            }
            else if (type == 0x2A) // null
            {
                continue;
            }
            else
            {
                std::printf("[PHOTON/LOAD-SYNC] ChangeGroups op248 param key=%u unexpected type=0x%02X; raw stage marker still proves client passed LevelLoading\n",
                            static_cast<unsigned int>(key), static_cast<unsigned int>(type));
                break;
            }
        }

        std::printf("[PHOTON/LOAD-SYNC] ChangeGroups op248 observed: add=");
        if (addedGroups.empty())
            std::printf("<none>");
        else
        {
            for (size_t i = 0; i < addedGroups.size(); ++i)
                std::printf("%s%u", i ? "," : "", addedGroups[i]);
        }
        std::printf(" remove=");
        if (removedGroups.empty())
            std::printf("<none>");
        else
        {
            for (size_t i = 0; i < removedGroups.size(); ++i)
                std::printf("%s%u", i ? "," : "", removedGroups[i]);
        }
        std::printf("\n");

        for (const unsigned int group : addedGroups)
        {
            if (group == 1 && !g_loadSyncSawGroup1)
            {
                g_loadSyncSawGroup1 = true;
                std::printf("[PHOTON/LOAD-SYNC] GROUP 1 ENABLED: ClientSession reached StaticSynchronizing; LevelLoadingManager/GoToSubLevel completed\n");
            }
            else if (group == 2 && !g_loadSyncSawGroup2)
            {
                g_loadSyncSawGroup2 = true;
                std::printf("[PHOTON/LOAD-SYNC] GROUP 2 ENABLED: ClientSession reached DynamicSynchronizing\n");
            }
            else if (group == 3 && !g_loadSyncSawGroup3)
            {
                g_loadSyncSawGroup3 = true;
                std::printf("[PHOTON/LOAD-SYNC] GROUP 3 ENABLED: PlayerInfoSync receive group enabled; RPC_RequestMapReady should be the next client room RPC\n");
            }
        }
        std::fflush(stdout);
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

    static void LogRecordClientDebugInfoEnvelope(const unsigned char* plain, int plainSize)
    {
        if (!plain || plainSize < 7 || plain[0] != 61)
            return;

        const unsigned short parameterCount = ReadU16BE(plain + 1);
        std::printf("[BOL/DEBUG] envelope operation=61 parameterCount=%u totalBytes=%d\n",
                    static_cast<unsigned int>(parameterCount), plainSize);

        // BOL currently sends one parameter: key 112 -> object[]. Decode the
        // stable outer shape instead of stopping at GpType.ObjectArray (0x7A).
        if (parameterCount >= 1 && plain[3] == 112 && plain[4] == 0x7A)
        {
            int offset = 5;
            if (offset + 2 <= plainSize)
            {
                const unsigned short objectCount = ReadU16BE(plain + offset);
                offset += 2;
                std::printf("[BOL/DEBUG] param112 type=ObjectArray objectCount=%u\n",
                            static_cast<unsigned int>(objectCount));

                if (objectCount >= 1 && offset + 3 <= plainSize && plain[offset] == 0x73)
                {
                    const unsigned short buildLength = ReadU16BE(plain + offset + 1);
                    offset += 3;
                    if (offset + static_cast<int>(buildLength) <= plainSize)
                    {
                        std::string build(reinterpret_cast<const char*>(plain + offset), buildLength);
                        std::printf("[BOL/DEBUG] clientBuild=%s\n", build.c_str());
                        offset += buildLength;
                    }
                }

                // Protocol16 Dictionary marker = 0x44. In this payload the
                // dictionary is Dictionary<int, Hashtable>. Surface its exact
                // declared types/count even if nested records evolve later.
                if (offset + 5 <= plainSize && plain[offset] == 0x44)
                {
                    const unsigned char keyType = plain[offset + 1];
                    const unsigned char valueType = plain[offset + 2];
                    const unsigned short recordCount = ReadU16BE(plain + offset + 3);
                    std::printf("[BOL/DEBUG] records type=Dictionary keyType=0x%02X valueType=0x%02X count=%u\n",
                                static_cast<unsigned int>(keyType),
                                static_cast<unsigned int>(valueType),
                                static_cast<unsigned int>(recordCount));
                }
            }
        }
        else
        {
            std::printf("[BOL/DEBUG] unexpected op61 outer shape: key=%u type=0x%02X\n",
                        plainSize > 3 ? static_cast<unsigned int>(plain[3]) : 0u,
                        plainSize > 4 ? static_cast<unsigned int>(plain[4]) : 0u);
        }
    }

    static void StopSessionInfoViewProbeOnDebugReport(unsigned int reportClientTime)
    {
        if (!g_autoMapProbeActive)
            return;

        if (!g_lastDebugRelevantToSessionInfoProbe)
        {
            std::printf("[PHOTON/SESSIONINFO-EXACT] debug report is unrelated to Photon Event206; exact probe remains active\n");
            std::fflush(stdout);
            return;
        }

        if (g_sessionInfoExactProbeStage == SessionInfoExactProbeStage::AwaitContractBaseline)
        {
            std::printf("[PHOTON/SESSIONINFO-EXACT] PHASE A FAIL at clientTime=%u: view1005 count=0 packet caused a Photon OnEvent/view failure\n", reportClientTime);
            std::printf("[PHOTON/SESSIONINFO-EXACT] map change was NOT requested; this points to view mapping or a dependency inside SessionInfo.OnPhotonSerializeView before SetMapAndModeName\n");
        }
        else if (g_sessionInfoExactProbeStage == SessionInfoExactProbeStage::AwaitMapTrigger)
        {
            std::printf("[PHOTON/SESSIONINFO-EXACT] PHASE B ERROR at clientTime=%u after map trigger\n", reportClientTime);
            std::printf("[PHOTON/SESSIONINFO-EXACT] debug context: instance=%s mode=%s map=%s\n",
                        g_lastDebugInstanceName.empty() ? "<empty>" : g_lastDebugInstanceName.c_str(),
                        g_lastDebugGameModeName.empty() ? "<empty>" : g_lastDebugGameModeName.c_str(),
                        g_lastDebugMapName.empty() ? "<empty>" : g_lastDebugMapName.c_str());
            if (!g_lastDebugMapName.empty())
            {
                std::printf("[PHOTON/SESSIONINFO-EXACT] Session state contains a map name, so Event206 decoding reached/advanced map state; inspect MapsDesc/LevelLoading path next\n");
            }
            else
            {
                std::printf("[PHOTON/SESSIONINFO-EXACT] Session debug map is still empty; failure happened before a usable map state was established\n");
            }
        }
        else
        {
            std::printf("[PHOTON/SESSIONINFO-EXACT] probe-relevant client debug report at clientTime=%u stage=%d\n",
                        reportClientTime, static_cast<int>(g_sessionInfoExactProbeStage));
        }

        g_autoMapProbeActive = false;
        g_autoChainMapStatePending = false;
        g_autoChainMapStateNotBefore = 0;
        g_autoMapProbeNextAt = 0;
        g_sessionInfoViewProbeNextAt = 0;
        g_sessionInfoExactProbeStage = SessionInfoExactProbeStage::Complete;
        std::fflush(stdout);
    }

    static bool LogRecordClientDebugInfoText(const unsigned char* plain, int plainSize, unsigned int reportClientTime)
    {
        g_lastDebugRelevantToSessionInfoProbe = false;
        g_lastDebugGameModeName.clear();
        g_lastDebugInstanceName.clear();
        g_lastDebugMapName.clear();

        // RecordClientDebugInfo (op 61) sends parameter 112 as a Protocol16
        // object[] containing the build string and a Dictionary<int, Hashtable>.
        // The game's debug-record Hashtable stores the managed stack/message
        // text as Int32 key 5 -> String.  The normal request logger stops at
        // object[] (0x7A), so scan the complete, already reassembled operation
        // for those typed key/string pairs and print the text verbatim.
        if (!plain || plainSize < 9 || plain[0] != 61)
            return false;

        LogRecordClientDebugInfoEnvelope(plain, plainSize);
        LogGeneratedPhotonEventCorrelation(reportClientTime);

        bool sawSessionViewCountMismatch = false;
        bool sawNullReference = false;
        bool sawNetworkingPeerOnEvent = false;
        bool sawCharacterSelect = false;

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

            if (fieldKey == 7)
                g_lastDebugGameModeName = value;
            else if (fieldKey == 8)
                g_lastDebugInstanceName = value;
            else if (fieldKey == 9)
                g_lastDebugMapName = value;

            if (value.find("Error in Instantiation! The resource's PhotonView count is not the same as in incoming data.") != std::string::npos)
                sawSessionViewCountMismatch = true;
            if (value.find("NullReferenceException") != std::string::npos)
                sawNullReference = true;
            if (value.find("NetworkingPeer.OnEvent") != std::string::npos)
                sawNetworkingPeerOnEvent = true;
            if (value.find("CharacterSelectBehaviour") != std::string::npos ||
                value.find("CharacterSelectView") != std::string::npos)
                sawCharacterSelect = true;

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

        if (sawSessionViewCountMismatch)
            std::printf("[BOL/DEBUG-CLASSIFY] PHOTON_VIEW_COUNT_MISMATCH\n");
        if (sawNullReference && sawNetworkingPeerOnEvent)
            std::printf("[BOL/DEBUG-CLASSIFY] PHOTON_ON_EVENT_NULL_REFERENCE\n");
        if (sawNullReference && sawCharacterSelect)
            std::printf("[BOL/DEBUG-CLASSIFY] CHARACTER_SELECT_NULL_REFERENCE\n");
        if (sawNullReference && !sawNetworkingPeerOnEvent && !sawCharacterSelect)
            std::printf("[BOL/DEBUG-CLASSIFY] UNCLASSIFIED_NULL_REFERENCE\n");

        g_lastDebugRelevantToSessionInfoProbe =
            sawSessionViewCountMismatch || (sawNullReference && sawNetworkingPeerOnEvent);
        if (g_lastDebugRelevantToSessionInfoProbe)
            std::printf("[BOL/DEBUG-CLASSIFY] SESSIONINFO_PROBE_RELEVANT=1\n");
        else
            std::printf("[BOL/DEBUG-CLASSIFY] SESSIONINFO_PROBE_RELEVANT=0\n");

        std::printf("[BOL/DEBUG-CONTEXT] instance=%s mode=%s map=%s\n",
                    g_lastDebugInstanceName.empty() ? "<empty>" : g_lastDebugInstanceName.c_str(),
                    g_lastDebugGameModeName.empty() ? "<empty>" : g_lastDebugGameModeName.c_str(),
                    g_lastDebugMapName.empty() ? "<empty>" : g_lastDebugMapName.c_str());

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
                const bool viewCountMismatch = LogRecordClientDebugInfoText(plain.data(), static_cast<int>(plain.size()), sentTime);
                StopSessionInfoViewProbeOnDebugReport(sentTime);
                if (viewCountMismatch)
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
            const bool viewCountMismatch = LogRecordClientDebugInfoText(payload.data() + 2, static_cast<int>(payload.size()) - 2, sentTime);
            StopSessionInfoViewProbeOnDebugReport(sentTime);
            if (viewCountMismatch)
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
            MaybeConfirmPlayerViewProbe(server, remote, sentTime);
            // Keepalive/ping commands trigger the one-shot AUTO-MAP send.  If the
            // current packet is the successful RPC_RequestMapReady operation,
            // decode it first instead of racing ahead to the next candidate.
            if (type == 5)
                MaybeSendAutoChainMapState(server, remote, sentTime);

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
                                const bool viewCountMismatch = LogRecordClientDebugInfoText(plain.data(), static_cast<int>(plain.size()), sentTime);
                                StopSessionInfoViewProbeOnDebugReport(sentTime);
                                if (viewCountMismatch)
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
                                std::printf("[PHOTON/UDP] op 74 identified from Assembly-CSharp.dll as CharacterService::SetPersonalSettings (stateful local contract)\n");
                                SendPhotonDecompiledContractResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    plain[0], plain.data(), static_cast<int>(plain.size()), true);
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
                            else if (!plain.empty() && plain[0] == 227)
                            {
                                if (session.gameServerPeer && !session.joinGameResponseSent)
                                {
                                    std::printf("[PHOTON/UDP] op 227 identified as Photon CreateGame (GameServer room create/join)\n");
                                    session.joinGameResponseSent = SendPhotonJoinGameResponse(
                                        server, remote, remoteLength, sentTime, challenge, plain[0],
                                        plain.data(), static_cast<int>(plain.size()), true);
                                    if (session.joinGameResponseSent)
                                        BeginSessionViewProbeJoin(remote, remoteLength, challenge, true);
                                }
                                else if (!session.gameServerPeer && !session.joinRandomGameResponseSent)
                                {
                                    std::printf("[PHOTON/UDP] op 227 identified as Photon CreateGame (Master -> GameServer redirect)\n");
                                    CaptureJoinRandomMatchmakingState(plain.data(), static_cast<int>(plain.size()));
                                    session.joinRandomGameResponseSent = SendPhotonJoinRandomGameResponse(
                                        server, remote, remoteLength, sentTime, challenge, plain[0],
                                        plain.data(), static_cast<int>(plain.size()), true);
                                }
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
                                CaptureJoinRandomMatchmakingState(plain.data(), static_cast<int>(plain.size()));
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
                                SendPhotonEmptySuccessResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    plain[0], true, "RAISE_EVENT");
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
                            else if (!plain.empty() && plain[0] == 248)
                            {
                                LogChangeGroupsOperation(plain.data(), static_cast<int>(plain.size()));
                                SendPhotonDecompiledContractResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    plain[0], plain.data(), static_cast<int>(plain.size()), true);
                            }
                            else if (!plain.empty() &&
                                     SendPhotonDecompiledContractResponse(
                                         server, remote, remoteLength, sentTime, challenge,
                                         plain[0], plain.data(), static_cast<int>(plain.size()), true))
                            {
                                std::printf("[PHOTON/UDP] decompiled Assembly-CSharp service contract handled op %u (%s::%s)\n",
                                            static_cast<unsigned int>(plain[0]),
                                            LookupBOLServiceName(plain[0]),
                                            LookupBOLOperationName(plain[0]));
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
                                const bool viewCountMismatch = LogRecordClientDebugInfoText(payload + 2, payloadSize - 2, sentTime);
                                StopSessionInfoViewProbeOnDebugReport(sentTime);
                                if (viewCountMismatch)
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
                                std::printf("[PHOTON/UDP] op 74 identified from Assembly-CSharp.dll as CharacterService::SetPersonalSettings (stateful local contract)\n");
                                SendPhotonDecompiledContractResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    operationCode, payload + 2, payloadSize - 2, false);
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
                            else if (operationCode == 227)
                            {
                                if (session.gameServerPeer && !session.joinGameResponseSent)
                                {
                                    std::printf("[PHOTON/UDP] op 227 identified as Photon CreateGame (GameServer room create/join)\n");
                                    session.joinGameResponseSent = SendPhotonJoinGameResponse(
                                        server, remote, remoteLength, sentTime, challenge, operationCode,
                                        payload + 2, payloadSize - 2, false);
                                    if (session.joinGameResponseSent)
                                        BeginSessionViewProbeJoin(remote, remoteLength, challenge, false);
                                }
                                else if (!session.gameServerPeer && !session.joinRandomGameResponseSent)
                                {
                                    std::printf("[PHOTON/UDP] op 227 identified as Photon CreateGame (Master -> GameServer redirect)\n");
                                    CaptureJoinRandomMatchmakingState(payload + 2, payloadSize - 2);
                                    session.joinRandomGameResponseSent = SendPhotonJoinRandomGameResponse(
                                        server, remote, remoteLength, sentTime, challenge, operationCode,
                                        payload + 2, payloadSize - 2, false);
                                }
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
                                CaptureJoinRandomMatchmakingState(payload + 2, payloadSize - 2);
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
                                SendPhotonEmptySuccessResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    operationCode, false, "RAISE_EVENT");
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
                            else if (operationCode == 248)
                            {
                                LogChangeGroupsOperation(payload + 2, payloadSize - 2);
                                SendPhotonDecompiledContractResponse(
                                    server, remote, remoteLength, sentTime, challenge,
                                    operationCode, payload + 2, payloadSize - 2, false);
                            }
                            else if (operationCode != 230 &&
                                     SendPhotonDecompiledContractResponse(
                                         server, remote, remoteLength, sentTime, challenge,
                                         operationCode, payload + 2, payloadSize - 2, false))
                            {
                                std::printf("[PHOTON/UDP] decompiled Assembly-CSharp service contract handled op %u (%s::%s)\n",
                                            static_cast<unsigned int>(operationCode),
                                            LookupBOLServiceName(operationCode),
                                            LookupBOLOperationName(operationCode));
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
                HandleAutoChainPlayerReadyAck(remote, acknowledgedSequence, sentTime);
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
        std::printf("[PHOTON/UDP] Photon 3.2 master + online + GameServer responder enabled (AUTO-PREFAB chain + SESSIONINFO Unity-resource map resolver + map asset-bundle + legacy Unity scene-table audit + v63 ChangeGroups + Mono live load-step markers + decompiled service-contract responder + local state persistence + event correlation + per-prefab count cache + BOL debug decoder)\n");
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
