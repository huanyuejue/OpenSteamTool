#include "AppTicket.h"
#include "Hook/Hooks_Decryption.h"
#include "OSTPlatform/include/SteamCredentialStore.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

#include "dllmain.h"

#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace AppTicket {
    constexpr AppId_t kLocalAppTicketSourceAppId = 7;
    // Steamworks 公共运行库：免费自动装、覆盖极广。第二顺位 donor，
    // 已实测内存里有有效票（178 字节命中），有票即用，日志标明来源。
    constexpr AppId_t kSysVerifyAppId = 228980;
    constexpr size_t kDonorMaxSize = 65536;
    constexpr size_t kSteamIdTicketMinimumSize = 16;
    constexpr const wchar_t* kRegAppsPath = L"Software\\Valve\\Steam\\Apps";

    // donor 借用的是结构 + 真签名：够大、非占位、首字段非零即可用。
    // 16B 占位符和垃圾大值直接扔，真假交给 Denuvo 定。
    static bool IsUsableDonor(const std::vector<uint8_t>& t) {
        if (t.size() <= kAppTicketSignatureSize || t.size() > kDonorMaxSize) return false;
        uint32_t head = 0;
        memcpy(&head, t.data(), sizeof(head));
        return head != 0;
    }

    // 票里 SteamID 的低 32 位就是账号 ID，用它认出这张票是谁的。
    // 布局与 GetSpoofSteamID 一致：[Size][Version][SteamID]。
    static uint32_t DonorAccountId(const std::vector<uint8_t>& t) {
        if (t.size() < kSteamIdTicketMinimumSize) return 0;
        uint64_t steamId = 0;
        memcpy(&steamId, t.data() + 8, sizeof(steamId));
        return static_cast<uint32_t>(steamId & 0xFFFFFFFFull);
    }

    // 当前登录账号。拿不到时返回 0，调用方据此退回纯结构校验。
    // 切号必然重启 Steam，进程内不会有第二个值，不需要缓存失效逻辑。
    static uint32_t ActiveAccountId() {
        uint32_t accountId = 0;
        std::wstring universe;
        if (OSTPlatform::SteamCredentialStore::GetActiveUser(accountId, universe) !=
            OSTPlatform::SteamCredentialStore::Status::Ok) {
            return 0;
        }
        return accountId;
    }

    // pin：记住 donor 来源，下次先验 pin（单 IPC 线程，无锁，沿用存量假设）。
    enum class DonorKind { None, Memory, Registry, LocalConfig };
    static DonorKind g_donorKind = DonorKind::None;
    static AppId_t g_donorAppId = 0;

    // localconfig.vdf 里的 apptickets 段是 Steam 自己维护的真票仓库，OST 从不写它。
    // 注册表那边基本只剩 OST 自己的激活记录，数量级差得很远，所以这里才是活票来源。
    // 票是 Valve 签的，伪造只需要借尾部的签名，源 appid 与目标无关，任取一张即可。
    //
    // 文件是文本 VDF，结构固定为 "apptickets" { "<appid>" "<hex>" ... }。
    // 找段用括号配对而不是直接找右括号，避免段里将来出现嵌套时截断。
    static bool ParseLocalConfigAppTickets(const std::string& data, uint32_t wantAccount,
                                           AppId_t excludeAppId,
                                           std::vector<uint8_t>& out, AppId_t& appId) {
        const size_t marker = data.find("\"apptickets\"");
        if (marker == std::string::npos) return false;

        const size_t open = data.find('{', marker);
        if (open == std::string::npos) return false;

        size_t close = std::string::npos;
        int depth = 0;
        for (size_t i = open; i < data.size(); ++i) {
            if (data[i] == '{') {
                ++depth;
            } else if (data[i] == '}') {
                if (--depth == 0) { close = i; break; }
            }
        }
        if (close == std::string::npos) return false;

        // 逐对取 "<appid>" "<hex>"，读到第一张合格的票就停。
        size_t pos = open;
        auto NextQuoted = [&](std::string& value) -> bool {
            const size_t a = data.find('"', pos);
            if (a == std::string::npos || a >= close) return false;
            const size_t b = data.find('"', a + 1);
            if (b == std::string::npos || b >= close) return false;
            value.assign(data, a + 1, b - a - 1);
            pos = b + 1;
            return true;
        };
        auto HexNibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return c - 'A' + 10;
        };

        std::string key;
        std::string hex;
        while (NextQuoted(key) && NextQuoted(hex)) {
            if (key.empty() || key.find_first_not_of("0123456789") != std::string::npos) continue;
            if (hex.size() < 2 || (hex.size() % 2) != 0) continue;

            bool allHex = true;
            for (char c : hex) {
                if (!std::isxdigit(static_cast<unsigned char>(c))) { allHex = false; break; }
            }
            if (!allHex) continue;
            // 至少要能容纳尾部签名，否则拼接时会把整张票吃掉。
            if (hex.size() / 2 <= kAppTicketSignatureSize) continue;

            std::vector<uint8_t> bytes;
            bytes.reserve(hex.size() / 2);
            for (size_t i = 0; i + 1 < hex.size(); i += 2) {
                bytes.push_back(static_cast<uint8_t>((HexNibble(hex[i]) << 4) | HexNibble(hex[i + 1])));
            }
            if (!IsUsableDonor(bytes)) continue;

            // 家庭共享来的票带的是出借者的 SteamID，拿去拼接必然报 54。
            // 调用方按 wantAccount 过滤，wantAccount 为 0 表示不限。
            const AppId_t parsedApp = static_cast<AppId_t>(strtoul(key.c_str(), nullptr, 10));
            // 目标游戏自己跳过：它要是有票就不会走到 forge，拿它当基底只会自杀。
            if (parsedApp == excludeAppId) continue;
            if (wantAccount != 0) {
                const uint32_t ticketAccount = DonorAccountId(bytes);
                if (ticketAccount != 0 && ticketAccount != wantAccount) {
                    LOG_DEBUG("LocalConfigDonor: skip appid {} base (account {} != {})",
                              parsedApp, ticketAccount, wantAccount);
                    continue;
                }
            }

            out = std::move(bytes);
            appId = parsedApp;
            return true;
        }
        return false;
    }

    // accountId 为 0 时读当前登录账号；非 0 时读指定账号的目录。
    static bool ReadLocalConfigDonor(uint32_t accountId, AppId_t excludeAppId,
                                     std::vector<uint8_t>& out, AppId_t& appId) {
        out.clear();
        appId = 0;
        if (accountId == 0 || SteamInstallPath[0] == '\0') return false;

        const std::string path = std::string(SteamInstallPath) + "\\userdata\\" +
                                 std::to_string(accountId) + "\\config\\localconfig.vdf";
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            LOG_DEBUG("LocalConfigDonor: cannot open {}", path);
            return false;
        }

        const std::string data((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
        if (data.empty()) return false;

        if (!ParseLocalConfigAppTickets(data, accountId, excludeAppId, out, appId)) {
            LOG_DEBUG("LocalConfigDonor: no usable ticket in {} ({} bytes)", path, data.size());
            return false;
        }
        LOG_DEBUG("LocalConfigDonor: picked appid {} ({} bytes) from account {}",
                  appId, out.size(), accountId);
        return true;
    }

    // excludeAppId 只对 LocalConfig 有意义：那边按文件内容重新解析，需要知道
    // 目标游戏是谁才能跳过它。Memory/Registry 按 appid 直取，天然不会取到目标。
    static bool ReadDonor(DonorKind kind, AppId_t donorAppId, AppId_t excludeAppId,
                          std::vector<uint8_t>& out) {
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
        } else if (kind == DonorKind::LocalConfig) {
            // 只认这一张：pin 里记的 appid 换了就作废，调用方会重新走全链扫描。
            std::vector<uint8_t> t;
            AppId_t appId = 0;
            if (!ReadLocalConfigDonor(ActiveAccountId(), excludeAppId, t, appId)) return false;
            if (appId != donorAppId) return false;
            out = std::move(t);
        }
        return IsUsableDonor(out);
    }

    // 扫描顺序：pin → 内存 7 → 内存 228980（验证位）→ localconfig → 注册表全枚举。
    // 目标游戏自己跳过（它有票就不会进 forge）。donor 字节不落盘，
    // 落到目标名下会污染该游戏的 spoof 身份。
    //
    // localconfig.vdf 是本账号真票的主要来源，票里带的就是本账号 SteamID，
    // 用它做基底不需要再对齐身份。注册表留作最后兜底：那边多半只剩 OST
    // 自己的激活记录，池子账号连真票都没有时只能靠它。
    //
    // 分两轮：第一轮只收当前登录账号名下的票，第二轮才放宽到任意可用票。
    // 宽松轮里 localconfig 仍只读本账号目录，跨账号的票身份对不上，
    // 拿来做基底只会把 54 报得更早。
    static bool FindDonorAppTicket(AppId_t excludeAppId, std::vector<uint8_t>& out, AppId_t& donorAppId) {
        const uint32_t activeAccount = ActiveAccountId();

        auto TryAccept = [&](DonorKind kind, AppId_t cand) -> bool {
            std::vector<uint8_t> ticket;
            if (!ReadDonor(kind, cand, excludeAppId, ticket)) return false;
            if (activeAccount != 0) {
                const uint32_t donorAccount = DonorAccountId(ticket);
                if (donorAccount != 0 && donorAccount != activeAccount) return false;
            }
            out = std::move(ticket);
            donorAppId = cand;
            return true;
        };

        auto TryMemCandidates = [&](bool requireOwn) -> bool {
            const AppId_t memCandidates[] = { kLocalAppTicketSourceAppId, kSysVerifyAppId };
            for (AppId_t cand : memCandidates) {
                if (cand == excludeAppId) continue;
                if (!requireOwn) {
                    if (TryAccept(DonorKind::Memory, cand)) {
                        g_donorKind = DonorKind::Memory;
                        g_donorAppId = cand;
                        LOG_INFO("ForgeLocalAppOwnershipTicket: forge donor from memory appid={} ({} bytes, own_ticket={})",
                                 cand, out.size(), activeAccount != 0 && DonorAccountId(out) == activeAccount);
                        return true;
                    }
                    continue;
                }
                std::vector<uint8_t> ticket;
                if (!ReadDonor(DonorKind::Memory, cand, excludeAppId, ticket)) continue;
                if (activeAccount == 0 || DonorAccountId(ticket) != activeAccount) continue;
                out = std::move(ticket);
                donorAppId = cand;
                g_donorKind = DonorKind::Memory;
                g_donorAppId = cand;
                LOG_INFO("ForgeLocalAppOwnershipTicket: forge donor from memory appid={} ({} bytes, own_ticket=true)",
                         cand, out.size());
                return true;
            }
            return false;
        };

        // localconfig 只能读本账号目录：目录本身就按账号分隔，跨账号的票
        // 带的是别人的 SteamID，做基底只会把身份冲突报得更早。因此它不进宽松轮，
        // 第一轮没命中就不必再试，省一次整文件读取。
        auto TryLocalConfig = [&]() -> bool {
            if (activeAccount == 0) return false;
            std::vector<uint8_t> ticket;
            AppId_t cand = 0;
            if (!ReadLocalConfigDonor(activeAccount, excludeAppId, ticket, cand)) return false;
            out = std::move(ticket);
            donorAppId = cand;
            g_donorKind = DonorKind::LocalConfig;
            g_donorAppId = cand;
            LOG_INFO("ForgeLocalAppOwnershipTicket: forge donor from localconfig appid={} ({} bytes, account={})",
                     cand, out.size(), activeAccount);
            return true;
        };

        auto TryRegistry = [&](bool requireOwn) -> bool {
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
                const AppId_t cand = static_cast<AppId_t>(id);
                std::vector<uint8_t> ticket;
                if (!ReadDonor(DonorKind::Registry, cand, excludeAppId, ticket)) continue;
                if (requireOwn && (activeAccount == 0 || DonorAccountId(ticket) != activeAccount)) continue;
                out = std::move(ticket);
                donorAppId = cand;
                RegCloseKey(hApps);
                g_donorKind = DonorKind::Registry;
                g_donorAppId = cand;
                LOG_INFO("ForgeLocalAppOwnershipTicket: forge donor ticket found: appid={} ({} bytes, own_ticket={})",
                         cand, out.size(), activeAccount != 0 && DonorAccountId(out) == activeAccount);
                return true;
            }
            RegCloseKey(hApps);
            return false;
        };

        out.clear();
        donorAppId = 0;

        // pin 先验：上次选中的来源优先，省掉重复全扫。
        if (g_donorKind != DonorKind::None && g_donorAppId != excludeAppId) {
            if (TryAccept(g_donorKind, g_donorAppId)) {
                return true;
            }
            g_donorKind = DonorKind::None;
            g_donorAppId = 0;
        }

        if (activeAccount != 0) {
            if (TryMemCandidates(true)) return true;
            if (TryLocalConfig()) return true;
            if (TryRegistry(true)) return true;
        }
        if (TryMemCandidates(false)) return true;
        if (TryRegistry(false)) return true;

        out.clear();
        donorAppId = 0;
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
}
