CC       = gcc
CFLAGS   = -Wall -Wextra -g -O2
CXX      = g++
CXXFLAGS = -Wall -Wextra -g -O2 -std=c++17

# ---------- SDS（C） ----------

SDS_SRC = src/sds.c

test_sds: tests/test_sds.c $(SDS_SRC)
	$(CC) $(CFLAGS) -I src -o $@ $^

# ---------- Dict（C 实现） ----------

DICT_SRC = src/dict.c src/dict_type.c
DICT_DEPS = src/dict.h src/dict_type.h src/sds.h src/val_obj.h

test_dict: tests/test_dict.c $(DICT_SRC) $(SDS_SRC) $(DICT_DEPS)
	$(CC) $(CFLAGS) -I src -o $@ $^

bench_dict: tests/bench_dict.c $(DICT_SRC) $(SDS_SRC)
	$(CC) $(CFLAGS) -O2 -I src -o $@ $^ -lm
	./$@ $(N)

# ---------- 服务端 ----------

SERVER_SRC = src/main.c src/server.c src/log.c src/service.c \
             src/dict.c src/dict_type.c src/resp.c src/sds.c
SERVER_DEPS = src/server.h src/log.h src/service.h \
              src/dict.h src/dict_type.h src/resp.h src/sds.h src/val_obj.h

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
	rm -f test_resp test_sds test_dict flashkv bench_dict

.PHONY: clean
