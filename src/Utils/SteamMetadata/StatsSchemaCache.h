#ifndef STATSSCHEMACACHE_H
#define STATSSCHEMACACHE_H

#include "Steam/Types.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace StatsSchemaCache
{
    // 统计 app 的成就总数（schema 里的 achievement bits 数）。
    // 数据源：<Steam>/appcache/stats/UserGameStatsSchema_<appid>.bin，
    // 这是 Steam 客户端自己缓存的成就 schema，游戏被加载过后就会存在。
    //
    // 返回 true 表示拿到了确定的成就总数（含 0：该游戏没有成就）；
    // 返回 false 表示本地没有 schema 或解析失败，调用方应跳过注入。
    //
    // 只缓存解析成功的结果；文件不存在不缓存——schema 可能在游戏
    // 首次运行后才落盘，下次查询要重试文件。
    bool GetAchievementTotal(AppId_t appId, uint32_t& total);

    // ── 真实成就解锁状态 ────────────────────────────────────────
    //
    // 数据源（都用 schema internal_name 做连接键）：
    //   1. UserGameStatsSchema_<appid>.bin     成就定义：stat 节点名 = 本地
    //      存档里的 stat 索引；bits 子节点名 = bit 索引；bits 子节点的
    //      name 字段 = 成就 internal_name。
    //   2. UserGameStats_<steamid3>_<appid>.bin 游戏内解锁的真实记录：
    //      cache → <stat 索引> → { data/pendingbits(解锁位图, bit N=第 N 个
    //      成就), AchievementTimes{ <bit 索引> = 解锁时间戳 } }。
    //
    // 两个文件都按 mtime 缓存，游戏解锁新成就会立刻反映。
    struct RealUnlock
    {
        bool     unlocked = false;
        uint32_t unlock_time = 0;   // unix 秒；未解锁为 0
    };

    // steamId3 = 64 位 steamid 的低 32 位；传 0 时自动扫描该 appid 的
    // 全部 UserGameStats_*_<appid>.bin 并取 mtime 最新的（当前活跃账号
    // 的存档总会随解锁刷新）。
    //
    // 返回 true 表示拿到了确定的解锁状态映射（可能为空 = 全部未解锁）；
    // 返回 false 表示本地无 schema/存档或解析失败，调用方应退回旧行为。
    bool GetRealUnlockState(AppId_t appId, uint32_t steamId3,
                            std::unordered_map<std::string, RealUnlock>& out);
}

#endif // STATSSCHEMACACHE_H
