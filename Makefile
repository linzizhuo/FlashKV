CC       = gcc
CFLAGS   = -Wall -Wextra -g -O2
CXX      = g++
CXXFLAGS = -Wall -Wextra -g -O2 -std=c++17

# ---------- SDS（C） ----------

SDS_SRC = src/sds.c

test_sds: tests/test_sds.c $(SDS_SRC)
	$(CC) $(CFLAGS) -I src -o $@ $^

# ---------- Dict + KVDB（C 实现） ----------

DICT_SRC  = src/dict.c src/dict_type.c
KVDB_SRC  = src/kvdb.c
DICT_DEPS = src/dict.h src/dict_type.h src/sds.h src/val_obj.h src/kvdb.h src/ttl.h

test_dict: tests/test_dict.c $(DICT_SRC) $(SDS_SRC) src/zskiplist.c src/zset.c $(DICT_DEPS)
	$(CC) $(CFLAGS) -I src -o $@ tests/test_dict.c $(DICT_SRC) $(SDS_SRC) src/zskiplist.c src/zset.c

bench_dict: tests/bench_dict.c $(DICT_SRC) $(SDS_SRC)
	$(CC) $(CFLAGS) -O2 -I src -o $@ $^ -lm
	./$@ $(N)

# ---------- 服务端 Benchmark ----------

bench_server: tests/bench_server.c
	$(CC) $(CFLAGS) -O2 -o $@ $^ -lm

# ---------- 服务端 ----------

SERVER_SRC = src/main.c src/server.c src/log.c src/service.c \
             src/kvdb.c src/dict.c src/dict_type.c src/resp.c src/sds.c src/zskiplist.c src/zset.c
SERVER_DEPS = src/server.h src/log.h src/service.h src/kvdb.h \
              src/dict.h src/dict_type.h src/resp.h src/sds.h src/val_obj.h src/ttl.h src/zskiplist.h src/zset.h

flashkv: $(SERVER_SRC) $(SERVER_DEPS)
	$(CC) $(CFLAGS) -I src -o $@ $(SERVER_SRC)

# ---------- RESP ----------

RESP_SRC = src/resp.c

test_resp: tests/test_resp.c $(RESP_SRC)
	$(CC) $(CFLAGS) -I src -o $@ $^

# ---------- 全部 ----------

.PHONY: all
all: test_resp test_sds test_dict flashkv
	@echo "======= 全部构建完成 ======="

clean:
	rm -f test_resp test_sds test_dict flashkv bench_dict bench_server

.PHONY: clean
