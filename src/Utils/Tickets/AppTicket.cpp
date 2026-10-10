#include "AppTicket.h"
#include "Hook/Hooks_Decryption.h"
#include "OSTPlatform/include/SteamCredentialStore.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

#include <windows.h>

#include <cstring>
#include <iterator>

namespace AppTicket {
    constexpr AppId_t kLocalAppTicketSourceAppId = 7;
    // Steamworks 公共运行库：免费自动装、覆盖极广。第二顺位 donor，
    // 已实测内存里有有效票（178 字节命中），有票即用，日志标明来源。
    constexpr AppId_t kSysVerifyAppId = 228980;
    constexpr size_t kDonorMaxSize = 65536;
    constexpr size_t kSteamIdTicketMinimumSize = 16;
    constexpr const wchar_t* kRegAppsPath = L"Software\\Valve\\Steam\\Apps";

    // donor 只借结构 + 真签名，不借身份：够大、非占位、首字段非零即用，
    // 真假由 Denuvo 定。16B 占位符和垃圾大值直接扔。
    static bool IsUsableDonor(const std::vector<uint8_t>& t) {
        if (t.size() <= kAppTicketSignatureSize || t.size() > kDonorMaxSize) return false;
        uint32_t head = 0;
        memcpy(&head, t.data(), sizeof(head));
        return head != 0;
    }

    // pin：记住 donor 来源，下次先验 pin（单 IPC 线程，无锁，沿用存量假设）。
    enum class DonorKind { None, Memory, Registry };
    static DonorKind g_donorKind = DonorKind::None;
    static AppId_t g_donorAppId = 0;

    static bool ReadDonor(DonorKind kind, AppId_t donorAppId, std::vector<uint8_t>& out) {
        out.clear();
        if (donorAppId == 0) return false;
        if (kind == DonorKind::Memory) {
            out = Hooks_Decryption::GetCacheAppOwnershipTicket(donorAppId);
        } else if (kind == DonorKind::Registry) {
            std::vector<uint8_t> t;
            if (OSTPlatform::SteamCredentialStore::GetAppTicket(donorAppId, t) ==
                OSTPlatform::SteamCredentialStore::Status::Ok) {
                out = std::move(t);
            }
        }
        return IsUsableDonor(out);
    }

    // 扫描顺序：pin → 内存 7 → 内存 228980（验证位）→ 注册表全枚举。
    // 目标游戏自己跳过（它有票就不会进 forge）。donor 字节不落盘，
    // 落到目标名下会污染该游戏的 spoof 身份。
    static bool FindDonorAppTicket(AppId_t excludeAppId, std::vector<uint8_t>& out, AppId_t& donorAppId) {
        out.clear();
        donorAppId = 0;
        if (g_donorKind != DonorKind::None && g_donorAppId != excludeAppId) {
            if (ReadDonor(g_donorKind, g_donorAppId, out)) {
                donorAppId = g_donorAppId;
                return true;
            }
            g_donorKind = DonorKind::None;
            g_donorAppId = 0;
        }
        const AppId_t memCandidates[] = { kLocalAppTicketSourceAppId, kSysVerifyAppId };
        for (AppId_t cand : memCandidates) {
            if (cand == excludeAppId) continue;
            if (ReadDonor(DonorKind::Memory, cand, out)) {
                g_donorKind = DonorKind::Memory;
                g_donorAppId = cand;
                donorAppId = cand;
                if (cand == kSysVerifyAppId) {
                    LOG_INFO("FindDonorAppTicket: 228980 sysverify hit ({} bytes), usable donor", out.size());
                }
                return true;
            }
        }
        HKEY hApps = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegAppsPath, 0, KEY_ENUMERATE_SUB_KEYS,
                          &hApps) != ERROR_SUCCESS) {
            return false;
        }
        wchar_t nameBuf[32];
        for (DWORD index = 0; ; ++index) {
            DWORD nameLen = static_cast<DWORD>(std::size(nameBuf));
            if (RegEnumKeyExW(hApps, index, nameBuf, &nameLen, nullptr, nullptr,
                              nullptr, nullptr) != ERROR_SUCCESS) {
                break;
            }
            const unsigned long id = wcstoul(nameBuf, nullptr, 10);
            if (id == 0 || id > UINT32_MAX || static_cast<AppId_t>(id) == excludeAppId) continue;
            if (ReadDonor(DonorKind::Registry, static_cast<AppId_t>(id), out)) {
                RegCloseKey(hApps);
                g_donorKind = DonorKind::Registry;
                g_donorAppId = static_cast<AppId_t>(id);
                donorAppId = static_cast<AppId_t>(id);
                LOG_INFO("ForgeLocalAppOwnershipTicket: forge donor ticket found: appid={} ({} bytes)",
                         donorAppId, out.size());
                return true;
            }
        }
        RegCloseKey(hApps);
        return false;
    }

    static uint64_t GetSteamIDFromCredentialStore(AppId_t appId) {
        uint64_t steamId = 0;
        const auto status = OSTPlatform::SteamCredentialStore::GetSteamId(appId, steamId);
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_TRACE("GetSpoofSteamID for AppId {}: SteamID unavailable in credential store ({})",
                      appId, OSTPlatform::SteamCredentialStore::ToString(status));
            return 0;
        }

        LOG_DEBUG("GetSpoofSteamID for AppId {}: SteamID credential -> 0x{:X}({})", appId, steamId, steamId);
        return steamId;
    }

    std::vector<uint8_t> GetAppOwnershipTicketFromCredentialStore(AppId_t appId) {
        // exclude those appids that are not in addappid
        if (!LuaConfig::HasDepot(appId)) {
            LOG_DEBUG("GetAppOwnershipTicketFromCredentialStore for AppId {}: not in addappid, skip", appId);
            return {};
        }
        std::vector<uint8_t> ticket;
        const auto status = OSTPlatform::SteamCredentialStore::GetAppTicket(appId, ticket);
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_TRACE("Read App Ownership Ticket for AppId {}: cached credential unavailable ({})",
                      appId, OSTPlatform::SteamCredentialStore::ToString(status));
            return {};
        }

        LOG_INFO("Successfully retrieved App Ownership Ticket from credential store, AppId: {}, Ticket Size: {}", appId, ticket.size());
        return ticket;
    }

    // Exploit steamdrmp's off-by-four ticket parsing vulnerability:
    static std::vector<uint8_t> ForgeLocalAppOwnershipTicket(AppId_t appId) {
        std::vector<uint8_t> source;
        AppId_t donorAppId = 0;
        if (!FindDonorAppTicket(appId, source, donorAppId)) {
            LOG_DEBUG("ForgeLocalAppOwnershipTicket for AppId {}: no source appticket (no usable donor)", appId);
            return {};
        }

        const size_t signedSize = source.size() - kAppTicketSignatureSize;
        std::vector<uint8_t> ticket;
        ticket.reserve(source.size() + sizeof(AppId_t));
        ticket.insert(ticket.end(), source.begin(), source.begin() + signedSize);

        const uint8_t* appIdBytes = reinterpret_cast<const uint8_t*>(&appId);
        ticket.insert(ticket.end(), appIdBytes, appIdBytes + sizeof(AppId_t));
        ticket.insert(ticket.end(), source.begin() + signedSize, source.end());

        LOG_INFO("Forged App Ownership Ticket, AppId: {}, SourceAppId: {}, Physical Size: {}, Total Size: {}",
                 appId, donorAppId, ticket.size(), source.size());
        return ticket;
    }

    bool GetAppOwnershipTicket(AppId_t appId, AppOwnershipTicket& ticket, AppTicketSource source) {
        ticket = {};
        
        if (source == AppTicketSource::CredentialStoreOnly || source == AppTicketSource::CredentialStoreThenForge) {
            ticket.data = GetAppOwnershipTicketFromCredentialStore(appId);
            if (!ticket.data.empty() && ticket.data.size() >= sizeof(uint32)) {
                ticket.totalSize = static_cast<uint32>(ticket.data.size());
                ticket.appIdOffset = kAppTicketAppIdOffset;
                ticket.steamIdOffset = kAppTicketSteamIdOffset;
                ticket.signatureOffset = *reinterpret_cast<const uint32*>(ticket.data.data());
                ticket.signatureSize = kAppTicketSignatureSize;
                return true;
            }
        }

        if (source == AppTicketSource::CredentialStoreOnly) return false;

        ticket.data = ForgeLocalAppOwnershipTicket(appId);
        if (ticket.data.empty()) return false;

        ticket.totalSize = static_cast<uint32>(ticket.data.size() - sizeof(AppId_t));
        ticket.appIdOffset = ticket.totalSize - kAppTicketSignatureSize;
        ticket.steamIdOffset = kAppTicketSteamIdOffset;
        ticket.signatureOffset = ticket.appIdOffset + sizeof(AppId_t);
        ticket.signatureSize = kAppTicketSignatureSize;
        return true;
    }

    std::vector<uint8_t> GetEncryptedTicketFromCredentialStore(AppId_t appId) {
        LOG_DEBUG("appid={}", appId);    
        // exclude those appids that are not in addappid
        if (!LuaConfig::HasDepot(appId)) {
            LOG_DEBUG("GetEncryptedTicketFromCredentialStore for AppId {}: not in addappid, skip", appId);
            return {};
        }
        std::vector<uint8_t> ticket;
        const auto status = OSTPlatform::SteamCredentialStore::GetETicket(appId, ticket);
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_TRACE("Read Encrypted App Ticket for AppId {}: cached credential unavailable ({})",
                      appId, OSTPlatform::SteamCredentialStore::ToString(status));
            return {};
        }

        LOG_INFO("Successfully retrieved Encrypted App Ticket from credential store, AppId: {}, Ticket Size: {}", appId, ticket.size());
        return ticket;
    }

    bool WriteAppOwnershipTicket(AppId_t appId, const std::vector<uint8_t>& data) {
        // we can't execlude appids here 
        const auto status = OSTPlatform::SteamCredentialStore::WriteAppTicket(appId, data);
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_ERROR("Failed to write AppTicket for AppId {} to credential store: {}",
                      appId, OSTPlatform::SteamCredentialStore::ToString(status));
            return false;
        }

        LOG_INFO("Wrote AppTicket for AppId {} ({} bytes)", appId, data.size());
        return true;
    }

    bool WriteEncryptedTicket(AppId_t appId, const std::vector<uint8_t>& data) {
        // we can't execlude appids here 
        const auto status = OSTPlatform::SteamCredentialStore::WriteETicket(appId, data);
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_ERROR("Failed to write ETicket for AppId {} to credential store: {}",
                      appId, OSTPlatform::SteamCredentialStore::ToString(status));
            return false;
        }

        LOG_INFO("Wrote ETicket for AppId {} ({} bytes)", appId, data.size());
        return true;
    }

    bool WriteSteamID(AppId_t appId, uint64_t steamId) {
        const auto status = OSTPlatform::SteamCredentialStore::WriteSteamId(appId, steamId);
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_ERROR("Failed to write SteamID for AppId {} to credential store: {}",
                      appId, OSTPlatform::SteamCredentialStore::ToString(status));
            return false;
        }

        LOG_INFO("Wrote SteamID for AppId {} ({})", appId, steamId);
        return true;
    }

    uint64_t GetSpoofSteamID(AppId_t appId) {
        // exclude those appids that are not in addappid
        if (!LuaConfig::HasDepot(appId)) {
            LOG_DEBUG("GetSpoofSteamID for AppId {}: not in addappid, skip spoofing", appId);
            return 0;
        }
        const uint64_t credentialSteamID = GetSteamIDFromCredentialStore(appId);
        if (credentialSteamID != 0) {
            return credentialSteamID;
        }

        // The SteamID baked into the cached AppOwnershipTicket is the same
        // one Steam itself uses for this app — pull it straight out of the
        // ticket so spoofed responses match what the DRM layer expects.
        // Layout: ticket bytes start with [uint32 Size][uint32 Version][uint64 SteamID][...].
        std::vector<uint8_t> ticket = GetAppOwnershipTicketFromCredentialStore(appId);
        if (ticket.size() >= kSteamIdTicketMinimumSize) {
            const uint64_t steamID = reinterpret_cast<const uint64_t*>(ticket.data())[1];
            LOG_DEBUG("GetSpoofSteamID for AppId {}: -> 0x{:X}({})", appId, steamID, steamID);
            return steamID;
        }
        return 0;
    }

    uint64_t GetForgeSteamID(AppId_t appId) {
        // 非名单游戏不 spoof，跟 GetSpoofSteamID 同门控。
        if (!LuaConfig::HasDepot(appId)) {
            return 0;
        }
        // forge 是纯函数，可重复调：取票里 SteamID（donor 的身份），票里是谁人就是谁。
        // Layout 与 GetSpoofSteamID 同：[uint32 Size][uint32 Version][uint64 SteamID][...]。
        std::vector<uint8_t> ticket = ForgeLocalAppOwnershipTicket(appId);
        if (ticket.size() < kSteamIdTicketMinimumSize) {
            return 0;
        }
        const uint64_t steamID = reinterpret_cast<const uint64_t*>(ticket.data())[1];
        LOG_DEBUG("GetForgeSteamID for AppId {}: -> 0x{:X}({})", appId, steamID, steamID);
        return steamID;
    }
}
