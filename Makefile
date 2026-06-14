CC       = gcc
CFLAGS   = -Wall -Wextra -g -O2
CXX      = g++
CXXFLAGS = -Wall -Wextra -g -O2 -std=c++17

# ---------- SDS（C） ----------

SDS_SRC = src/sds.c

test_sds: tests/test_sds.c $(SDS_SRC)
	$(CC) $(CFLAGS) -I src -o $@ $^

# ---------- Dict（C 实现） ----------

DICT_SRC = src/dict.c
DICT_DEPS = src/dict.h src/sds.h

test_dict: tests/test_dict.c $(DICT_SRC) $(SDS_SRC) $(DICT_DEPS)
	$(CC) $(CFLAGS) -I src -o $@ $^

# ---------- 全部 ----------

.PHONY: all
all: test_sds test_dict
	@echo "======= 全部构建完成 ======="

clean:
	rm -f test_sds test_dict

.PHONY: clean
