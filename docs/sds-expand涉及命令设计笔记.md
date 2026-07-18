# SDS Expand 涉及命令设计笔记

## 涉及命令

| 命令 | 行为 | SDS 操作 |
|------|------|---------|
| APPEND | 追加字符串到 key 的 value 末尾 | `sdscatlen()` — 直接追加 |
| SETRANGE | 覆盖指定 offset 的字节 | offset > len 时扩容填零，再 memcpy |
| SETBIT | 设置指定 bit 位的值 | bit 位置超出字节长度时扩容，位运算写入 |

## 命令参数

### APPEND key value

| 参数 | 类型 | 说明 |
|------|------|------|
| key | RESP_STR | 目标 key |
| value | RESP_STR | 追加内容 |

**返回值**：追加后整个 value 的长度

### SETRANGE key offset value

| 参数 | 类型 | 说明 |
|------|------|------|
| key | RESP_STR | 目标 key |
| offset | RESP_INT / RESP_STR | 写入起始偏移，≥ 0 |
| value | RESP_STR | 要写入的内容 |

**返回值**：修改后整个 value 的长度

**行为**：
- key 不存在 → 创建空 SDS，offset 之前填 `\0`，再写 value
- offset ≤ sdslen → 原地覆盖
- offset > sdslen → 扩容，gap 填 `\0`，更新 len
- offset + vallen ≤ sdslen → 不扩容，len 不变

### SETBIT key offset value

| 参数 | 类型 | 说明 |
|------|------|------|
| key | RESP_STR | 目标 key |
| offset | RESP_INT / RESP_STR | bit 偏移，≥ 0 |
| value | 0 或 1 | 要设置的 bit 值 |

**返回值**：该 offset 原来的 bit 值（0 或 1）

**位运算**：MSB-first（Redis 兼容）
```
byte_idx = offset / 8
bit_pos  = 7 - (offset % 8)
bit_mask = 1 << bit_pos
```

## SDS 路径

### APPEND — sdscatlen

```
s = sdscatlen(s, data, len)
  → sdsMakeRoomFor(s, len)   // 贪婪扩容
  → memcpy(s + oldlen, data, len)
  → sdssetlen(s, oldlen + len)
  → 封 '\0'
```

### SETRANGE — 上层拼装

```
如果 offset + vallen > sdslen:
    sdsMakeRoomFor(s, end - oldlen)
    如果 offset > oldlen: memset(s + oldlen, 0, offset - oldlen)
    sdssetlen(s, end)
memcpy(s + offset, data, vallen)
封 '\0'
```

不需要新 SDS 接口。和 APPEND 的区别：offset 可能留间隙要填零，offset+vallen ≤ len 时不更新 len。

### SETBIT — 上层拼装

```
byte_idx = offset / 8
如果 byte_idx + 1 > sdslen:
    扩容到 byte_idx + 1，新增字节填 0
    sdssetlen(s, byte_idx + 1)
oldbit = (s[byte_idx] & bit_mask) ? 1 : 0
s[byte_idx] = bitval ? (s[byte_idx] | bit_mask) : (s[byte_idx] & ~bit_mask)
封 '\0'
```

也不需要新 SDS 接口。只操作单个 bit，扩容和写入都在上层完成。

## 扩容策略

三个命令都走 `sdsMakeRoomFor`（greedy=1）：
- < 1MB：翻倍扩容
- ≥ 1MB：每次 +1MB

## 实现要点

### 命令表
按字母序插入：`APPEND` → `BGSAVE` → ... → `SET` → `SETBIT` → `SETRANGE`

### goto 清理模式

```c
/* newkey 路径：key 所有权移交 kvdb，不 sdsfree */
if (newkey) {
    kvdbSet(..., key, newobj);
    return;  // 不走到末尾的 sdsfree(key)
}

/* existing 路径：只更新指针 */
obj->val.str = s;  // realloc 可能移动了指针，写回
addReplyInteger(c, ...);
sdsfree(key);
return;

oom_s:
    sdsfree(s);
oom:
    addReplyError(c, "OOM");
cleanup:
    sdsfree(key);
```

| 标签 | 触发条件 | 清理 |
|------|---------|------|
| `oom_s` | OOM 且 s 是新分配的 | `sdsfree(s)` → fall through |
| `oom` | OOM 且 s 是 dict 的或 NULL | 只报 OOM → fall through |
| `cleanup` | WRONGTYPE 等非 OOM 错误 | 只 `sdsfree(key)` |

### 关键坑位

1. `sdsMakeRoomFor` 可能 realloc 移动指针，必须把新指针写回 `obj->val.str`
2. kvdbSet 接管 key 所有权，newkey 路径不能 `sdsfree(key)`
3. DATA_INT 暂走 WRONGTYPE，后续补 int→string 转换
