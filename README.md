# FlashKV

轻量级内存 KV 存储，参考 Redis 设计。

## 当前进度

### 核心引擎

- **dict** — 哈希表核心：`dictnew` / `dictAdd` / `dictReplace` / `dictfind` / `dictDelete` / `dictfree`
- **dictType** — 虚函数表，支持 `hash`、`keyCompare`、`keyFree`、`valFree`
- **dictTypeSds** — 基于 SDS 字符串的键值类型
- **ValObj** — 值统一包装，支持 STRING / LIST / ZSET / SET / HASH / INT 类型，通过 switch 分发释放

### 基础工具

- **SDS** — 动态字符串，含 MurmurHash2
- **log** — 日志模块，级别控制 + stdout/stderr，使用 `LOG_INFO(...)` 宏即可

## 测试

所有核心模块已通过单元测试：

- `test_dict` — 增 / 查 / 改 / 删 / 多 key 批量 / 值类型混合 / NULL 防御
- `test_sds` — 创建 / 长度 / 二进制安全

```bash
make test_dict && ./test_dict
# ======== 🎉 全部测试通过 ========
```

## 使用

```bash
# 构建并运行测试
make test_dict

# 运行服务（开发阶段）
./flashkv > flash.log 2>&1
```
