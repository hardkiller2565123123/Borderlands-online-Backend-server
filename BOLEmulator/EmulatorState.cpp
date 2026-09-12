#include "EmulatorShared.h"

namespace bolemu
{
    LocalCharacterState g_localCharacter;
    LocalServiceState g_localServices;

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
        const unsigned int version = 2;
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
            std::fwrite(&g_localCharacter.timePlayed, sizeof(g_localCharacter.timePlayed), 1, file) == 1 &&
            WriteStateString(file, g_localServices.personalSettings) &&
            std::fwrite(&g_localServices.currentTown, sizeof(g_localServices.currentTown), 1, file) == 1 &&
            std::fwrite(&g_localServices.bondCurrency, sizeof(g_localServices.bondCurrency), 1, file) == 1 &&
            std::fwrite(&g_localServices.specialCurrency, sizeof(g_localServices.specialCurrency), 1, file) == 1 &&
            std::fwrite(&g_localServices.circulateCurrency, sizeof(g_localServices.circulateCurrency), 1, file) == 1;

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
        LocalServiceState serviceState; // defaults provide v1 migration values
        bool ok =
            std::fread(&magic, sizeof(magic), 1, file) == 1 && magic == 0x31434C42u &&
            std::fread(&version, sizeof(version), 1, file) == 1 && (version == 1 || version == 2) &&
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

        if (ok && version >= 2)
        {
            ok =
                ReadStateString(file, serviceState.personalSettings) &&
                std::fread(&serviceState.currentTown, sizeof(serviceState.currentTown), 1, file) == 1 &&
                std::fread(&serviceState.bondCurrency, sizeof(serviceState.bondCurrency), 1, file) == 1 &&
                std::fread(&serviceState.specialCurrency, sizeof(serviceState.specialCurrency), 1, file) == 1 &&
                std::fread(&serviceState.circulateCurrency, sizeof(serviceState.circulateCurrency), 1, file) == 1;
        }
        std::fclose(file);

        if (!ok)
            return false;

        loaded.exists = true;
        g_localCharacter = loaded;
        g_localServices = serviceState;
        return true;
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
}
