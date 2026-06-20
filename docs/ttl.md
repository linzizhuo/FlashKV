# TTL 功能需求

## 核心命令

| 命令 | 语义 | 必要 |
|------|------|------|
| `EXPIRE key seconds` | 设置相对过期（秒） | ✅ |
| `PEXPIRE key ms` | 设置相对过期（毫秒） | ✅ |
| `EXPIREAT key ts` | 设置绝对过期（Unix 秒） | 最终都转 PEXPIREAT，可用 EXPIRE 模拟 |
| `PEXPIREAT key ms-ts` | 设置绝对过期（毫秒） | ✅ 底层唯一接口 |
| `TTL key` | 查询剩余秒数 | ✅ |
| `PTTL key` | 查询剩余毫秒数 | ✅ |
| `PERSIST key` | 移除过期 | 顺手的事 |

## 行为约定

- **key 不存在** → 所有 EXPIRE 返回 0，TTL 返回 -2
- **key 没设 TTL** → TTL 返回 -1
- **重复 EXPIRE** → 覆盖旧 TTL
- **SET 覆盖** → 默认清除 TTL（Redis 行为）；容后再加 `SET ... KEEPTTL`

## 存储

- 独立 `expires` dict，key → 绝对毫秒时间戳
- 时间戳类型与 `void*` 同宽，值直接塞指针，零分配
- `dictType.valFree = NULL`（无堆分配）

## 删除语义

采用惰性删除即可，定期删除后续再加：

- **惰性删除**：每次 GET / EXISTS 时检查 expires dict，过期则当场删
- **定期删除**（暂缓）：后台定时扫描随机抽样

## 与主 dict 的交互点

- `dictFind(db->dict, key)` → 拿到 val 和 hash
- `dictHashFind(db->expires, key, &hash)` → 拿 TTL，hash 复用

```
SET key val      → dict 写入 / expires 不动
GET key          → expireIfNeeded(db, key) → 过期则删，否则返回 val
EXPIRE key sec   → 转毫秒 → dictAdd(db->expires, key, ts)
TTL key          → dictHashFind(expires) → 算剩余
DEL key          → 主 dict 删 + expires 删
```

## 不做的

- `SET ... EX` / `SET ... PX` — 暂不做，SET 一次搞定过期是语法糖
- 定期删除 — 先跑通惰性删除
