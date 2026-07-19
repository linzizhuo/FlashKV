#!/bin/bash
# expire 模块功能测试
set -e

REDIS="redis-cli -p 6379"
PASS=0
FAIL=0

check() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$actual" = "$expected" ]; then
        echo "  ✓ $desc"
        PASS=$((PASS + 1))
    else
        echo "  ✗ $desc (expected: '$expected', got: '$actual')"
        FAIL=$((FAIL + 1))
    fi
}

cleanup() {
    kill $SERVER_PID 2>/dev/null
    wait $SERVER_PID 2>/dev/null
    rm -f dump.rdb ../dump.rdb
}
trap cleanup EXIT

# 启动 server，日志扔到 /dev/null
cd /home/baidu/FlashKV
./flashkv > /dev/null 2>&1 &
SERVER_PID=$!
sleep 1

echo "=== 1. 惰性删除 (expireIfNeeded) ==="

$REDIS SET k1 v1 > /dev/null
check "SET" "OK" "$($REDIS SET k1 v1)"

$REDIS EXPIRE k1 1 > /dev/null
check "EXPIRE 1s" "1" "$($REDIS EXPIRE k1 1)"

check "GET (未过期)" "v1" "$($REDIS GET k1)"

sleep 2
check "GET (已过期)" "" "$($REDIS GET k1)"
check "EXISTS (已过期)" "0" "$($REDIS EXISTS k1)"

echo ""
echo "=== 2. 无 TTL key 不受影响 ==="
$REDIS SET pk keep > /dev/null
sleep 2
check "GET (无 TTL)" "keep" "$($REDIS GET pk)"
check "EXISTS (无 TTL)" "1" "$($REDIS EXISTS pk)"
$REDIS DEL pk > /dev/null

echo ""
echo "=== 3. PERSIST ==="
$REDIS SET pp pv > /dev/null
$REDIS EXPIRE pp 10 > /dev/null
check "PERSIST" "1" "$($REDIS PERSIST pp)"
check "TTL (已持久化)" "-1" "$($REDIS TTL pp)"
$REDIS DEL pp > /dev/null

echo ""
echo "=== 4. EXPIREAT 绝对时间(过去) ==="
$REDIS SET ak av > /dev/null
past=$(date -d '1 hour ago' +%s)
check "EXPIREAT" "1" "$($REDIS EXPIREAT ak $past)"
check "GET (立即过期)" "" "$($REDIS GET ak)"

echo ""
echo "=== 5. 主动过期 + 惰性协同 ==="
# 写入 200 个 2s 过期 key
for i in $(seq 0 199); do
    $REDIS SET "ek$i" "x" > /dev/null
    $REDIS EXPIRE "ek$i" 2 > /dev/null
done
echo "  写入 200 个 2s 过期 key"

# 随机采样 10 个确认存在
sample_alive=0
for i in 5 25 50 75 100 125 150 160 180 195; do
    if [ "$($REDIS EXISTS "ek$i")" = "1" ]; then
        sample_alive=$((sample_alive + 1))
    fi
done
check "采样 10 个(未过期)全部存在" "10" "$sample_alive"

# 等 cron 主动清理
sleep 3
echo "  等待 3s (cron 每 100ms 跑一次)..."

# 再采样 10 个 — 应该有一部分被主动清理了
sample_alive=0
for i in 5 25 50 75 100 125 150 160 180 195; do
    if [ "$($REDIS EXISTS "ek$i")" = "1" ]; then
        sample_alive=$((sample_alive + 1))
    fi
done
echo "  采样 10 个存活: $sample_alive (预期 < 10，部分被 cron 清理)"

if [ "$sample_alive" -lt 10 ]; then
    echo "  ✓ activeExpireCycle 主动清理了过期 key"
    PASS=$((PASS + 1))
else
    echo "  ✗ activeExpireCycle 似乎没起作用"
    FAIL=$((FAIL + 1))
fi

# 惰性兜底：逐个 GET，应全部 nil
lazy_alive=0
for i in $(seq 0 49); do
    if [ "$($REDIS GET "ek$i")" != "" ]; then
        lazy_alive=$((lazy_alive + 1))
    fi
done
check "惰性兜底: 前 50 个已过期 key 全部不可访问" "0" "$lazy_alive"

echo ""
echo "=== 6. PEXPIRE / PEXPIREAT ==="
$REDIS SET mk mv > /dev/null
check "PEXPIRE 1000ms" "1" "$($REDIS PEXPIRE mk 1000)"
sleep 2
check "GET (已过期)" "" "$($REDIS GET mk)"

$REDIS SET nk nv > /dev/null
past_ms=$(($(date +%s) * 1000 - 1000))
check "PEXPIREAT (过去)" "1" "$($REDIS PEXPIREAT nk $past_ms)"
check "GET (立即过期)" "" "$($REDIS GET nk)"

echo ""
echo "========== 结果: $PASS 通过, $FAIL 失败 =========="
[ "$FAIL" -eq 0 ] || exit 1
