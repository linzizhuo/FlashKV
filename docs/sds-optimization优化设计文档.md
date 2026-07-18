# FlashKV SDS 多类型 Header 优化设计

## 状态：已完成 ✅

测试通过，零 warning 编译。

---

## 一、背景

FlashKV SDS 原版采用单一 `sdshdr64` header（17 字节），小字符串头部开销远超数据本身。参照 Redis，进行五级分段 header 重构。

---

## 二、Header 类型体系

| 类型 | 容量 | Header 大小 | `"hello"`(5B) 总开销 |
|------|------|-------------|---------------------|
| sdshdr5 | 0–31B | 1B (flags) | 7B (原 22B) |
| sdshdr8 | 32–255B | 3B | 9B |
| sdshdr16 | 256–64KB | 5B | 10B |
| sdshdr32 | 64KB–4GB | 9B | 14B |
| sdshdr64 | 4GB+ | 17B | 22B |

TYPE_5 无 `alloc` 字段，不可原地扩容，适用于 key、反序列化只读 value、短字面量。

---

## 三、核心实现：`_sdsnewlen`

整个 SDS 构造路径收敛为一个内部函数，所有公共 API 为薄壳转调：

```c
// 内部实现 — 类型选择 + 分配 + 初始化 + 升级
sds _sdsnewlen(const void *init, size_t initlen, int trymalloc);

// 公共 API
sds sdsnewlen(const void *init, size_t initlen)    →  _sdsnewlen(init, initlen, 0)
sds sdstrynewlen(const void *init, size_t initlen) →  _sdsnewlen(init, initlen, 1)
sds sdsnew(const char *init)                       →  sdsnewlen(init, strlen(init))
```

### 3.1 类型选择：按 `alloc` 而非 `len`

```c
char sdsReqType(size_t string_size);
```

旧逻辑按 `len` 选类型，扩容后 `alloc` 可能超出类型上限导致截断。新逻辑按 `string_size`（= `alloc`）选择，带 header + `\0` 的容量校验，保证选出来的类型所有字段都能装下。

### 3.2 malloc_usable_size：利用 glibc 隐式多分配

glibc `malloc_usable_size()` 可获取实际分配的可写大小（通常比请求多 8~16 字节）。`_sdsnewlen` 将此空间直接记入 `alloc`，后续小追加操作无需 realloc：

```c
size_t usable = malloc_usable_size(sh);
while (adjustTypeIfNeeded(&type, &hdrlen, usable));
sdssetalloc(s, usable - sdsHdrSize(type) - 1);
```

### 3.3 adjustTypeIfNeeded：自动类型升级

若 `malloc_usable_size` 返回的空间超出现有类型容量上限，while 循环逐级升级类型，直至装下：

```c
static inline int adjustTypeIfNeeded(char *type, int *hdrlen, size_t bufsize) {
    size_t usable = bufsize - *hdrlen - 1;
    if (*type != SDS_TYPE_5 && usable > sdsTypeMaxSize(*type)) {
        *type = sdsReqType(usable);
        *hdrlen = sdsHdrSize(*type);
        return 1;   // 继续尝试升级
    }
    return 0;       // 到位，退出循环
}
```

调用方：`while (adjustTypeIfNeeded(&type, &hdrlen, usable));`

### 3.4 三条初始化路径收敛（init 参数）

| `init` 值 | 行为 | `len` |
|-----------|------|-------|
| 有效指针 | `memcpy` 拷贝数据 | `initlen` |
| `NULL` | `memset` 清零 | `initlen` |
| `SDS_NOINIT` | 跳过初始化，buf 为垃圾字节 | `initlen` |

三个分支收敛在 4 行内：

```c
if (init != SDS_NOINIT)
    init ? memcpy(s, init, initlen) : memset(s, 0, initlen);
s[initlen] = '\0';
sdssetlen(s, initlen);
```

### 3.5 空串优化

TYPE_5 无 `alloc` 不适合追加。空串（`initlen == 0`）强制使用 TYPE_8，避免首次 append 立即触发升级：

```c
if (type == SDS_TYPE_5 && initlen == 0)
    type = SDS_TYPE_8;
```

### 3.6 双出口 — trymalloc 参数

```c
// trymalloc=0：溢出 assert 炸，OOM abort 炸 → 核心路径无需判空
// trymalloc=1：溢出 return NULL，OOM return NULL → 调用方健壮处理
if (trymalloc) {
    if (initlen + hdrlen + 1 <= initlen) return NULL;  // size_t overflow
} else {
    assert(initlen + hdrlen + 1 > initlen);
}
```

---

## 四、多类型 header 释放：`sdsfree`

旧：`SDS_HDR(64, s)` 硬编码 → 用错偏移，free 野指针崩。

新：`s[-1]` 读 flags 取类型 → `sdsHdrSize(type)` 算出真实 header 大小：

```c
void sdsfree(void *s) {
    if (s == NULL) return;
    free(s - sdsHdrSize(sdsType(s)));
}
```

---

## 五、改动清单

| 函数 | 状态 | 说明 |
|------|------|------|
| `_sdsnewlen` | 🆕 新增 | 内部实现，所有构造路径的收敛点 |
| `sdsReqType` | 🆕 新增 | 按 `alloc` 选类型，对抗 malloc 多分配 |
| `adjustTypeIfNeeded` | 🆕 新增 | malloc 多给空间后升级类型 |
| `sdsTypeMaxSize` | 🆕 新增 | 返回类型容量上限 |
| `sdsnewlen` | ♻️ 重构 | → 薄壳调用 `_sdsnewlen(init, initlen, 0)` |
| `sdsnew` | ♻️ 重构 | → 薄壳调用 `sdsnewlen` |
| `sdstrynewlen` | 🆕 新增 | try 版，分配失败返回 NULL |
| `sdsfree` | ♻️ 重构 | `sdsHdrSize(sdsType(s))` 动态偏移 |
| `sdsHdrSize` | 🆕 新增 | `static inline`，类型 → header 字节数 |
| `sdslen/sdsavail/sdssetalloc/sdssetlen/...` | ✅ 无需改 | 已支持多类型分发 |

---

## 六、改动链路

```
sdsnew/sdsnewlen/sdstrynewlen → _sdsnewlen → sdsReqType → malloc
                                           ↘ malloc_usable_size → adjustTypeIfNeeded
                                           ↘ init 三条路径 → \0 → 返回
sdsdup ────────────────────────────────→ sdsnewlen ┘
sdsDeserialize/sdsRead ────────────────→ sdsnewlen ┘
sdsfree → sdsType(s[-1]) → sdsHdrSize → 正确 offset → free
```

---

## 七、设计哲学

1. **内部实现收敛** — 多个公共 API 合并为一个 `_impl`，通过参数区分行为
2. **双出口 API** — abort 版 + try 版，调用方按场景选择失败策略
3. **隐式资源利用** — `malloc_usable_size` 白嫖 glibc 多分配空间，零性能代价
4. **防御性类型选择** — 按 `alloc` 而非 `len` 选类型，杜绝字段溢出
5. **`_` 前缀约定** — 内部函数暴露链接但不声明于 `.h`，高级调用方自担风险

---

## 八、未完成项（下一阶段）

- `sdsMakeRoomFor` — 扩容逻辑（TYPE_5 → TYPE_8 升级、预分配策略）
- `sdscatlen` / `sdscatsds` — 追加操作，依赖 `sdsMakeRoomFor`
- `zmalloc` 平台收敛 — `malloc_usable_size`(glibc) / `malloc_size`(macOS) 统一封装

---

## 九、验证

```
$ make test_sds && ./test_sds
======== SDS 测试 ========
===== 测试 sdsnew / sdslen =====
s1 = "hello", len = 5       ✅
s2 = "", len = 0            ✅
===== 测试二进制安全 =====
s (binary), len = 6         ✅
===== 测试 sdsfree 后释放 =====
释放完成 (无 crash 即通过)   ✅
======== 全部测试通过 ========
```

零 warning 编译，0 crash。


补充：最终，sds不仅仅是一个字符串，而是一个可靠的，效率极高的动态数组。这点值得补充。

补充：我去解析了redis中_sdsnewlen的逻辑，得到了以下的原逻辑：

type = sds_type_5   initlen == 0 ---> 强转sds_type_8    一种贪婪策略
initlen + hdrlen + 1 <= initlen  看似恒等式，实则是整数溢出检测，initlen是size_t，按照隐式转换规则，最终表达式会转化为size_t，最大2^64-1.几乎不可能触发溢出，所以这本质是一种边界检测，用溢出的方式检测边界，这种方式直击核心逻辑，值得学习。
trymalloc和检测溢出与充分利用内存的逻辑这里不再补充。
现在用我们拆解的逻辑策略去复查我们的_sdsnewlen
最后还真发现了错误awa，s[-1] = type & SDS_TYPE_MASK
首先这是错误的逻辑，其次type在sds5里是复用的，如果这样粗暴处理会导致sds5长度是0
改掉，然后像redis一样抽一个分段处理的函数出来。