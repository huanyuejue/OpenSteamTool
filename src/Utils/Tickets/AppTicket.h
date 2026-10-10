#pragma once

#include "Steam/Types.h"

#include <cstdint>
#include <vector>

namespace AppTicket {
    inline constexpr uint32 kAppTicketSteamIdOffset = 8;
    inline constexpr uint32 kAppTicketAppIdOffset = 16;
    inline constexpr uint32 kAppTicketSignatureSize = 128;

    enum class AppTicketSource {
        CredentialStoreOnly,
        ForgeOnly,
        CredentialStoreThenForge,
    };

    struct AppOwnershipTicket {
        std::vector<uint8_t> data;
        uint32 totalSize = 0;
        uint32 appIdOffset = kAppTicketAppIdOffset;
        uint32 steamIdOffset = kAppTicketSteamIdOffset;
        uint32 signatureOffset = 0;
        uint32 signatureSize = kAppTicketSignatureSize;
    };

    // Reads the app ownership ticket cached by Steam's local credential store.
    // Returns an empty vector when no ticket is available.
    std::vector<uint8_t> GetAppOwnershipTicketFromCredentialStore(AppId_t appId);

    bool GetAppOwnershipTicket(AppId_t appId, AppOwnershipTicket& ticket, AppTicketSource source);

    // Reads the encrypted app ticket cached by Steam's local credential store.
    // Returns an empty vector when no ticket is available.
    std::vector<uint8_t> GetEncryptedTicketFromCredentialStore(AppId_t appId);

    //Get spoof steamID From the cached AppOwnershipTicket for the given AppId.
    uint64_t GetSpoofSteamID(AppId_t appId);

    // Forge 出票并解析票里 SteamID（donor 的身份）。给非 Denuvo 假入库游戏做
    // GetSteamID 回退 spoof 用：forge 走了但 credential 无身份时，票里是谁人就是谁，
    // 否则 SteamStub 票人不一致报 54。无 donor 时返回 0。
    uint64_t GetForgeSteamID(AppId_t appId);

    // Write AppTicket binary data to Steam's local credential store.
    bool WriteAppOwnershipTicket(AppId_t appId, const std::vector<uint8_t>& data);

    // Write ETicket binary data to Steam's local credential store.
    bool WriteEncryptedTicket(AppId_t appId, const std::vector<uint8_t>& data);

    // Write authorized SteamID to Steam's local credential store.
    bool WriteSteamID(AppId_t appId, uint64_t steamId);
}
