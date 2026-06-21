#!/usr/bin/env bash
# ================================================================
#  FlashKV 稀疏表 vs 紧凑表 性能对比压测
#
#  测试场景：
#    A: 紧凑表 — 1M keys, ~1M slots, 负载 ~0.95
#    B: 稀疏表 — 1M keys, ~2M slots, 负载 ~0.48 (无缩容惩罚)
#
#  用途：量化"无缩容"对 GET/SET 性能的影响
# ================================================================

set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_DIR="$(dirname "$SCRIPTS_DIR")"

BENCH="${PROJ_DIR}/bench_server"
SERVER="${PROJ_DIR}/flashkv"
PORT=6379
HOST="127.0.0.1"
SEED=42

# 进度间隔：100K 报告一次
PROG=100000

cleanup() {
    if [ -n "${SERVER_PID:-}" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

# ---- 检查二进制 ----
if [ ! -x "$SERVER" ]; then
    echo ">>> Building flashkv..."
    make -C "$PROJ_DIR" flashkv
fi
if [ ! -x "$BENCH" ]; then
    echo ">>> Building bench_server..."
    make -C "$PROJ_DIR" bench_server
fi

# ---- 启动服务端 ----
echo ""
echo ">>> Starting FlashKV server on port $PORT..."
echo "    (server stdout → /dev/null, stderr visible for errors)"
"$SERVER" "$PORT" >/dev/null &
SERVER_PID=$!
sleep 1

# 验证服务端存活
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "ERROR: Server failed to start"
    exit 1
fi
echo ">>> Server PID=$SERVER_PID"

# ================================================================
#  场景 A：紧凑表 — 1M keys, 1M slots (负载 ~0.95)
# ================================================================
echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  Scenario A: Compact Table                                  ║"
echo "║  1M keys → table size 1,048,576 → load factor ~0.95         ║"
echo "╚══════════════════════════════════════════════════════════════╝"

echo ""
echo "── Phase A1: Populating 1M keys ──"
"$BENCH" --host "$HOST" --port "$PORT" \
    --populate 1000000 --key-start 1 \
    --label "A-populate" --seed "$SEED" \
    --prog-interval "$PROG" --warmup 0

echo ""
echo "── Phase A2: GET benchmark (compact table) ──"
"$BENCH" --host "$HOST" --port "$PORT" \
    --get 1000000 --keyspace 1000000 --key-start 1 \
    --label "A-GET-compact" --seed "$SEED" \
    --prog-interval "$PROG" --warmup 200000

echo ""
echo "── Phase A3: SET overwrite benchmark (compact table) ──"
"$BENCH" --host "$HOST" --port "$PORT" \
    --set 1000000 --keyspace 1000000 --key-start 1 \
    --label "A-SET-compact" --seed "$SEED" \
    --prog-interval "$PROG" --warmup 0

# ================================================================
#  场景 B：稀疏表 — 1M keys, 2M slots (负载 ~0.48)
#
#  步骤：
#    1. 再插入 1M keys (key 1M+1..2M) → 总计 2M keys，
#       期间触发 rehash 到 2^21 = 2,097,152 slots
#    2. 删除前半 1M keys (key 1..1M) → 剩下 1M keys，table 仍为 2M slots
#    3. 对剩余 keys (1M+1..2M) 做 GET/SET 压测
# ================================================================
echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  Scenario B: Sparse Table (No-Shrink Penalty)               ║"
echo "║  1M keys → table size 2,097,152 → load factor ~0.48         ║"
echo "╚══════════════════════════════════════════════════════════════╝"

echo ""
echo "── Phase B1: Populating 2nd 1M keys (key 1000001..2000000) ──"
echo "    (will trigger rehash from 1M → 2M slots)"
"$BENCH" --host "$HOST" --port "$PORT" \
    --populate 1000000 --key-start 1000001 \
    --label "B-populate" --seed "$SEED" \
    --prog-interval "$PROG" --warmup 0

echo ""
echo "── Phase B2: Deleting first 1M keys (key 1..1000000) ──"
echo "    (table stays at 2M slots — no shrink!)"
"$BENCH" --host "$HOST" --port "$PORT" \
    --delete 1000000 --key-start 1 \
    --label "B-delete" --seed "$SEED" \
    --prog-interval "$PROG" --warmup 0

echo ""
echo "── Phase B3: GET benchmark (sparse table, keys 1M+1..2M) ──"
"$BENCH" --host "$HOST" --port "$PORT" \
    --get 1000000 --keyspace 1000000 --key-start 1000001 \
    --label "B-GET-sparse" --seed "$SEED" \
    --prog-interval "$PROG" --warmup 200000

echo ""
echo "── Phase B4: SET overwrite benchmark (sparse table) ──"
"$BENCH" --host "$HOST" --port "$PORT" \
    --set 1000000 --keyspace 1000000 --key-start 1000001 \
    --label "B-SET-sparse" --seed "$SEED" \
    --prog-interval "$PROG" --warmup 0

# ================================================================
#  对比总结
# ================================================================
echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║                    COMPARISON SUMMARY                       ║"
echo "╠══════════════════════════════════════════════════════════════╣"
echo "║                                                             ║"
echo "║  Compare these pairs:                                       ║"
echo "║    A-GET-compact  vs  B-GET-sparse                          ║"
echo "║    A-SET-compact  vs  B-SET-sparse                          ║"
echo "║                                                             ║"
echo "║  Hypothesis:                                                ║"
echo "║    • Sparse GET ~5-15% slower (2× bucket array → TLB miss)  ║"
echo "║    • Sparse P95/P99 wider (cache-miss jitter)                ║"
echo "║    • SET overwrite similar delta (dictReplace = find+write)  ║"
echo "║                                                             ║"
echo "║  Key metric: throughput delta = TLB/cache penalty of        ║"
echo "║  carrying a 2× oversized bucket array with no shrink.       ║"
echo "║                                                             ║"
echo "╚══════════════════════════════════════════════════════════════╝"

# cleanup via trap
