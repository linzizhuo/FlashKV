#!/bin/bash
# FlashKV vs Redis 对比 benchmark
# 每个场景跑 5 轮取平均，消除偶然波动
set -e

RUNS=5
HOST=127.0.0.1
PORT=6379
SEED=42
FLASHKV_BIN=./flashkv

RED="\033[31m"
GREEN="\033[32m"
RESET="\033[0m"

run_bench() {
    # $1: label (flashkv 或 redis)
    # $2-$n: redis-benchmark args
    local label=$1; shift
    local sum=0
    echo -n "  $label: "
    for i in $(seq 1 $RUNS); do
        local result=$(redis-benchmark "$@" --csv 2>/dev/null \
            | grep -oP '"(\w+)","(\d+\.?\d*)"' \
            | awk -F',' '{print $2}' | tr -d '"')
        sum=$(echo "$sum + $result" | bc -l)
        echo -n "$result "
    done
    local avg=$(echo "scale=0; $sum / $RUNS" | bc)
    echo " → avg: $avg"
}

start_flashkv() {
    fuser -k $PORT/tcp 2>/dev/null || true
    sleep 0.3
    nohup $FLASHKV_BIN > /tmp/flashkv.log 2>&1 &
    sleep 1
    if ! redis-cli -p $PORT PING > /dev/null 2>&1; then
        echo "FlashKV 启动失败"
        cat /tmp/flashkv.log
        exit 1
    fi
}

start_redis() {
    sudo systemctl start redis-server 2>/dev/null || true
    sleep 0.5
}

stop_service() {
    fuser -k $PORT/tcp 2>/dev/null || true
    sudo systemctl stop redis-server 2>/dev/null || true
    sleep 0.5
}

echo "============================================"
echo "  FlashKV vs Redis Benchmark (每项 $RUNS 轮)"
echo "  时间: $(date)"
echo "============================================"
echo

# --- FlashKV ---
echo "--- FlashKV ---"
stop_service
start_flashkv

echo "[SET P=1]"
run_bench "flashkv" -h $HOST -p $PORT -t set -n 100000 -c 50 -q

echo "[GET P=1]"
run_bench "flashkv" -h $HOST -p $PORT -t get -n 100000 -c 50 -q

echo "[SET P=16]"
run_bench "flashkv" -h $HOST -p $PORT -t set -n 200000 -c 50 -P 16 -q

echo "[GET P=16]"
run_bench "flashkv" -h $HOST -p $PORT -t get -n 200000 -c 50 -P 16 -q

echo "[SET P=64]"
run_bench "flashkv" -h $HOST -p $PORT -t set -n 200000 -c 50 -P 64 -q

echo "[GET P=64]"
run_bench "flashkv" -h $HOST -p $PORT -t get -n 200000 -c 50 -P 64 -q

echo

# --- Redis ---
echo "--- Redis ---"
stop_service
start_redis

echo "[SET P=1]"
run_bench "redis" -h $HOST -p $PORT -t set -n 100000 -c 50 -q

echo "[GET P=1]"
run_bench "redis" -h $HOST -p $PORT -t get -n 100000 -c 50 -q

echo "[SET P=16]"
run_bench "redis" -h $HOST -p $PORT -t set -n 200000 -c 50 -P 16 -q

echo "[GET P=16]"
run_bench "redis" -h $HOST -p $PORT -t get -n 200000 -c 50 -P 16 -q

echo "[SET P=64]"
run_bench "redis" -h $HOST -p $PORT -t set -n 200000 -c 50 -P 64 -q

echo "[GET P=64]"
run_bench "redis" -h $HOST -p $PORT -t get -n 200000 -c 50 -P 64 -q

echo
echo "============================================"
echo "  Done."
echo "============================================"

stop_service
