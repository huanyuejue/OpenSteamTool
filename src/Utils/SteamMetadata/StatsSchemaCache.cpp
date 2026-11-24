#include "StatsSchemaCache.h"

#include "dllmain.h"
#include "Utils/Logging/Log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ════════════════════════════════════════════════════════════════
//  StatsSchemaCache
//
//  解析 Steam 客户端缓存的成就 schema（二进制 KeyValues），只为一件事：
//  数出成就总数（achievement bits 节点条目数），给 Player.GetAchievementsProgress
//  响应注入提供 total 字段。
//
//  二进制 KV 方言（已对本机 141 个 schema bin 全量验证，全部精确解析到 EOF）：
//    循环 { u8 type; 若 type==8(End) 结束当前容器; name = \0 结尾 UTF-8 串; value }
//    实际只出现三种类型：0=None(容器，递归) 1=String(\0结尾) 2=Int32(定长4字节)
//    另外按方言保守支持：3/6/7=定长4字节 4/5=定长8字节。
//
//  schema 结构：
//    <appid> { stats { <stat> { type, type_int, bits { <每个成就一项> } } } }
//    成就 stat 的识别：type 字符串为 ACHIEVEMENTS / GROUPACHIEVEMENTS，
//    或 type_int 为 4/5（UserStatType 的 Achievements/GroupAchievements）。
// ════════════════════════════════════════════════════════════════

namespace
{
    constexpr size_t kMaxSchemaFileBytes = 8 * 1024 * 1024;   // schema 都很小，超限当损坏处理
    constexpr int    kMaxDepth          = 24;
    constexpr size_t kMaxNameBytes      = 4096;

    enum class KVType : uint8_t
    {
        None       = 0,
        String     = 1,
        Int32      = 2,   // 本方言里 2 是定长 4 字节整数（实测 141 个文件验证）
        WideString = 3,   // 未在本方言中出现过，按 4 字节兜底
        Int64      = 4,   // 未出现过，按 8 字节兜底
        Float32    = 5,   // 未出现过，按 8 字节兜底
        Color      = 6,
        Pointer    = 7,
        End        = 8,
    };

    struct KVNode
    {
        std::string          name;
        KVType               type = KVType::None;
        std::string          stringValue;   // type == String
        int32_t              int32Value = 0; // type == Int32 系列；String 时为 atoi 结果
        std::vector<KVNode>  children;       // type == None 时有效

        const KVNode* Find(std::string_view key) const
        {
            for (const auto& child : children) {
                if (child.name.size() == key.size() &&
                    _strnicmp(child.name.c_str(), key.data(), key.size()) == 0)
                    return &child;
            }
            return nullptr;
        }
    };

    class KVReader
    {
    public:
        explicit KVReader(const std::vector<uint8_t>& data) : m_data(data) {}

        // 逐字段读取，任何越界/畸形输入都返回 false，绝不抛异常。
        // v1.6.7 修复：容器内容直接读进 out（节点自身的 children），
        // 旧实现 ReadNode(node.children.emplace_back()) 会多出一层匿名
        // 包装节点，导致 Find("stats")/Find("bits") 全部落空。
        bool ReadNodes(std::vector<KVNode>& out, int depth)
        {
            if (depth > kMaxDepth) return false;

            while (true) {
                const uint8_t raw = ReadU8();
                if (!m_ok) return false;
                if (raw == static_cast<uint8_t>(KVType::End)) return true;
                if (raw > static_cast<uint8_t>(KVType::End)) return false;

                KVNode node;
                node.type = static_cast<KVType>(raw);
                if (!ReadString(node.name)) return false;

                switch (node.type) {
                case KVType::None:
                    if (!ReadNodes(node.children, depth + 1))
                        return false;
                    break;
                case KVType::String:
                    if (!ReadString(node.stringValue)) return false;
                    node.int32Value = atoi(node.stringValue.c_str());
                    break;
                case KVType::Int32:
                case KVType::WideString:
                case KVType::Color:
                case KVType::Pointer: {
                    const uint32_t v = ReadU32();
                    if (!m_ok) return false;
                    node.int32Value = static_cast<int32_t>(v);
                    break;
                }
                case KVType::Int64:
                case KVType::Float32:
                    if (!Skip(8)) return false;
                    break;
                default:
                    return false;
                }

                out.push_back(std::move(node));
            }
        }

    private:
        uint8_t ReadU8()
        {
            if (m_pos >= m_data.size()) { m_ok = false; return 0; }
            return m_data[m_pos++];
        }

        uint32_t ReadU32()
        {
            if (m_pos + 4 > m_data.size()) { m_ok = false; return 0; }
            uint32_t v;
            memcpy(&v, m_data.data() + m_pos, 4);
            m_pos += 4;
            return v;
        }

        bool ReadString(std::string& out)
        {
            out.clear();
            while (true) {
                if (m_pos >= m_data.size()) { m_ok = false; return false; }
                const char c = static_cast<char>(m_data[m_pos++]);
                if (c == '\0') return true;
                out.push_back(c);
                if (out.size() > kMaxNameBytes) { m_ok = false; return false; }
            }
        }

        bool Skip(size_t n)
        {
            if (m_pos + n > m_data.size()) { m_ok = false; return false; }
            m_pos += n;
            return true;
        }

        const std::vector<uint8_t>& m_data;
        size_t m_pos = 0;
        bool   m_ok  = true;
    };

    bool IsAchievementStat(const KVNode& stat)
    {
        // type 字符串：ACHIEVEMENTS / GROUPACHIEVEMENTS
        const KVNode* typeStr = stat.Find("type");
        if (typeStr && !typeStr->stringValue.empty()) {
            if (_stricmp(typeStr->stringValue.c_str(), "ACHIEVEMENTS") == 0 ||
                _stricmp(typeStr->stringValue.c_str(), "GROUPACHIEVEMENTS") == 0)
                return true;
        }
        // type_int：4 = Achievements、5 = GroupAchievements
        const KVNode* typeInt = stat.Find("type_int");
        if (typeInt && (typeInt->int32Value == 4 || typeInt->int32Value == 5))
            return true;
        return false;
    }

    // 数一个 stat 节点下的成就数（bits 子节点的条目数）
    uint32_t CountStatAchievements(const KVNode& stat)
    {
        if (!IsAchievementStat(stat)) return 0;
        const KVNode* bits = stat.Find("bits");
        if (!bits) return 0;
        return static_cast<uint32_t>(bits->children.size());
    }

    // ── 带缓存 的 schema/存档 加载器 ────────────────────────────
    // 按 mtime 缓存整个 KV 根节点：文件没变就直接复用上次解析结果。
    // 游戏内解锁会改写 UserGameStats 存档（mtime 变化），所以解锁状态
    // 总能在下一次查询时反映出来。
    struct CachedFile
    {
        std::filesystem::file_time_type mtime;
        KVNode root;
        bool   valid = false;
    };

    std::mutex g_cacheMutex;
    std::unordered_map<uint32_t, uint32_t> g_cache;            // appid -> 成就总数
    std::unordered_map<std::string, CachedFile> g_fileCache;   // 路径 -> 解析结果

    bool LoadKVFile(const std::string& path, KVNode& root)
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) return false;
        if (std::filesystem::file_size(path, ec) > kMaxSchemaFileBytes || ec) return false;
        const auto mtime = std::filesystem::last_write_time(path, ec);
        if (ec) return false;

        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            const auto it = g_fileCache.find(path);
            if (it != g_fileCache.end() && it->second.valid && it->second.mtime == mtime) {
                root = it->second.root;
                return true;
            }
        }

        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
        if (data.empty()) return false;

        KVReader reader(data);
        KVNode parsed;
        if (!reader.ReadNodes(parsed.children, 0)) return false;

        LOG_ACHIEVEMENT_DEBUG("LoadKVFile: parsed OK path='{}' bytes={} rootChildren={}",
                              path, data.size(), parsed.children.size());

        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            auto& entry = g_fileCache[path];
            entry.mtime = mtime;
            entry.root  = parsed;
            entry.valid = true;
        }
        root = std::move(parsed);
        return true;
    }

    // 成就定义：schema 里每个成就 stat → { stat 节点名, bits[{bit 索引, internal_name}] }
    struct SchemaAchBit
    {
        std::string statName;      // stat 节点名，对应存档里的同名节点
        uint32_t    bitIndex = 0;  // bits 子节点名
        std::string internalName;  // bits 子节点的 name 字段
    };

    void CollectSchemaAchievements(const KVNode& root, uint32_t appId,
                                   std::vector<SchemaAchBit>& out, uint32_t& total)
    {
        char appidName[16];
        snprintf(appidName, sizeof(appidName), "%u", static_cast<unsigned>(appId));

        LOG_ACHIEVEMENT_DEBUG("CollectSchemaAchievements: appId={} appidName='{}' rootChildren={}",
                              appId, appidName, root.children.size());
        for (size_t i = 0; i < root.children.size() && i < 8; ++i)
            LOG_ACHIEVEMENT_DEBUG("CollectSchemaAchievements:   root[{}] name='{}' type={}",
                                  i, root.children[i].name, static_cast<int>(root.children[i].type));

        const KVNode* appNode = root.Find(appidName);
        if (!appNode) {
            LOG_ACHIEVEMENT_WARN("CollectSchemaAchievements: appId={} app node '{}' NOT FOUND",
                                 appId, appidName);
            return;
        }
        const KVNode* stats = appNode->Find("stats");
        if (!stats) {
            LOG_ACHIEVEMENT_WARN("CollectSchemaAchievements: appId={} 'stats' NOT FOUND under '{}'",
                                 appId, appidName);
            return;
        }
        LOG_ACHIEVEMENT_DEBUG("CollectSchemaAchievements: appId={} stats children={}",
                              appId, stats->children.size());

        for (const auto& stat : stats->children) {
            const KVNode* typeStr = stat.Find("type");
            const KVNode* bits = stat.Find("bits");
            LOG_ACHIEVEMENT_DEBUG("CollectSchemaAchievements: appId={} stat='{}' type={} "
                                  "typeStr='{}' bits={} isAch={}",
                                  appId, stat.name, static_cast<int>(stat.type),
                                  typeStr ? typeStr->stringValue : std::string("(none)"),
                                  bits ? bits->children.size() : 0u,
                                  IsAchievementStat(stat) ? "yes" : "no");
            if (!IsAchievementStat(stat)) continue;
            if (!bits) continue;
            for (const auto& bit : bits->children) {
                SchemaAchBit item;
                item.statName = stat.name;
                item.bitIndex = static_cast<uint32_t>(atoi(bit.name.c_str()));
                if (const KVNode* nm = bit.Find("name"))
                    item.internalName = nm->stringValue;
                out.push_back(std::move(item));
            }
            total += static_cast<uint32_t>(bits->children.size());
        }
    }

} // namespace

namespace StatsSchemaCache
{

bool GetAchievementTotal(AppId_t appId, uint32_t& total)
{
    if (appId == 0) return false;

    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        const auto it = g_cache.find(appId);
        if (it != g_cache.end()) {
            total = it->second;
            return true;
        }
    }

    if (SteamInstallPath[0] == '\0') return false;

    char path[512];
    snprintf(path, sizeof(path), "%s\\appcache\\stats\\UserGameStatsSchema_%u.bin",
             SteamInstallPath, static_cast<unsigned>(appId));

    KVNode root;
    if (!LoadKVFile(path, root)) return false;

    std::vector<SchemaAchBit> bits;
    uint32_t count = 0;
    CollectSchemaAchievements(root, appId, bits, count);

    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_cache[appId] = count;
    }
    LOG_ACHIEVEMENT_DEBUG("StatsSchemaCache: appId={} achievement total = {}", appId, count);

    total = count;
    return true;
}

bool GetRealUnlockState(AppId_t appId, uint32_t steamId3,
                        std::unordered_map<std::string, RealUnlock>& out)
{
    out.clear();
    if (appId == 0 || SteamInstallPath[0] == '\0') return false;

    // ── 1. schema：成就定义（stat 名 + bit 索引 + internal_name）─────
    char schemaPath[512];
    snprintf(schemaPath, sizeof(schemaPath),
             "%s\\appcache\\stats\\UserGameStatsSchema_%u.bin",
             SteamInstallPath, static_cast<unsigned>(appId));
    KVNode schemaRoot;
    if (!LoadKVFile(schemaPath, schemaRoot)) {
        LOG_ACHIEVEMENT_WARN("GetRealUnlockState: schema load failed: {}", schemaPath);
        return false;
    }
    LOG_ACHIEVEMENT_DEBUG("GetRealUnlockState: schemaPath='{}' rootChildren={}",
                          schemaPath, schemaRoot.children.size());

    std::vector<SchemaAchBit> bits;
    uint32_t total = 0;
    CollectSchemaAchievements(schemaRoot, appId, bits, total);
    if (bits.empty()) {
        LOG_ACHIEVEMENT_WARN("GetRealUnlockState: no schema achievements appId={}", appId);
        return false;
    }

    // ── 2. 本地解锁存档：优先指定账号，否则取 mtime 最新的 ──────────
    char statsDir[512];
    snprintf(statsDir, sizeof(statsDir), "%s\\appcache\\stats", SteamInstallPath);

    std::string bestPath;
    if (steamId3 != 0) {
        char p[560];
        snprintf(p, sizeof(p), "%s\\UserGameStats_%u_%u.bin", statsDir,
                 static_cast<unsigned>(steamId3), static_cast<unsigned>(appId));
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) bestPath = p;
    }
    if (bestPath.empty()) {
        // 扫描 UserGameStats_*_<appid>.bin，取 mtime 最新的（当前活跃账号
        // 的存档总会随解锁刷新，所以最新的一定是本账号的）
        std::error_code ec;
        std::filesystem::file_time_type bestTime{};
        for (const auto& entry : std::filesystem::directory_iterator(statsDir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            const std::string name = entry.path().filename().string();
            // 前缀 UserGameStats_ 后缀 _<appid>.bin
            const std::string prefix = "UserGameStats_";
            const std::string suffix  = "_" + std::to_string(static_cast<unsigned>(appId)) + ".bin";
            if (name.size() <= prefix.size() + suffix.size()) continue;
            if (name.compare(0, prefix.size(), prefix) != 0) continue;
            if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
            if (name.compare(prefix.size(), name.size() - prefix.size() - suffix.size(),
                             "Schema") == 0)
                continue;                                     // 排除 schema 文件本身
            const auto t = entry.last_write_time(ec);
            if (ec) continue;
            if (bestPath.empty() || t > bestTime) {
                bestPath = entry.path().string();
                bestTime = t;
            }
        }
    }
    if (bestPath.empty())
    {
        LOG_ACHIEVEMENT_WARN("GetRealUnlockState: archive not found appId={} steamId3={}",
                             appId, steamId3);
        return false;                                         // 连存档都没有 → 无法确定
    }

    KVNode statsRoot;
    if (!LoadKVFile(bestPath, statsRoot)) {
        LOG_ACHIEVEMENT_WARN("GetRealUnlockState: archive load failed: {}", bestPath);
        return false;
    }

    // 存档结构：cache → <stat 索引> → { data/pendingbits, AchievementTimes{...} }
    // DFS 找每个 stat 索引对应的解锁位图与时间表
    std::unordered_map<std::string, const KVNode*> statNodes;
    std::function<void(const KVNode&)> walk = [&](const KVNode& node) {
        for (const auto& child : node.children) {
            if (child.type != KVType::None) continue;
            if (child.Find("data") || child.Find("pendingbits") || child.Find("AchievementTimes"))
                statNodes[child.name] = &child;
            walk(child);
        }
    };
    walk(statsRoot);
    if (statNodes.empty())
        LOG_ACHIEVEMENT_WARN("GetRealUnlockState: no stat nodes found in archive: {}", bestPath);

    // ── 3. 合成 internal_name → 解锁状态 ────────────────────────────
    for (const auto& bit : bits) {
        RealUnlock u;
        const auto statIt = statNodes.find(bit.statName);
        if (statIt != statNodes.end()) {
            const KVNode* statNode = statIt->second;
            uint32_t mask = 0;
            bool hasMask = false;
            if (const KVNode* pb = statNode->Find("pendingbits")) {
                mask = static_cast<uint32_t>(pb->int32Value);
                hasMask = true;
            }
            if (const KVNode* d = statNode->Find("data")) {
                mask = static_cast<uint32_t>(d->int32Value);   // data 是已提交状态
                hasMask = true;
            }
            if (hasMask)
                u.unlocked = ((mask >> (bit.bitIndex & 31)) & 1u) != 0;

            if (const KVNode* times = statNode->Find("AchievementTimes")) {
                char idxName[16];
                snprintf(idxName, sizeof(idxName), "%u", bit.bitIndex);
                if (const KVNode* t = times->Find(idxName))
                    u.unlock_time = static_cast<uint32_t>(t->int32Value);
            }
        }
        if (!bit.internalName.empty())
            out[bit.internalName] = u;
    }

    LOG_ACHIEVEMENT_DEBUG("StatsSchemaCache: appId={} real unlock state: entries={} archive={}",
                          appId, out.size(), bestPath);
    return true;
}

} // namespace StatsSchemaCache
