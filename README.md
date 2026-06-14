# FlashKV

轻量级内存 KV 存储，参考 Redis 设计。

## 当前进度

- **dict** — 哈希表核心：`dictnew` / `dictAdd` / `dictReplace` / `dictfind` / `dictfree`
- **dictType** — 虚函数表，支持 `hash`、`keyCompare`、`keyFree`、`valFree`
- **dictTypeSds** — 基于 SDS 字符串的键值类型
- **ValObj** — 值统一包装，支持 STRING / LIST / ZSET / SET / HASH / INT 类型，通过 switch 分发释放
