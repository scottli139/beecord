# BuzzLink 协议层宿主单测：直接编译 ../main/proto 源码（路径引用，不复制），零三方依赖。
# 本机默认 clang（macOS）；CI 用 `make CC=gcc`（ubuntu）做双编译器零警告门禁。
CC     := clang
CFLAGS := -std=c11 -Wall -Wextra -Werror -g
PROTO  := ../main/proto
BUILD  := build

# 会话向量回放（TESTING.md §3）：JSON 或转换脚本变更时 make 自动重新生成 C 表
VECTOR_JSON := ../../apps/mac/Packages/BuzzLink/Tests/BuzzLinkCoreTests/Fixtures/basic-session.json
VECTOR_TOOL := ../../tools/vector2c.py
GENERATED   := generated
VECTOR_SRC  := $(GENERATED)/vectors_basic_session.c
VECTOR_HDR  := $(GENERATED)/vectors_basic_session.h

SRCS := $(PROTO)/crc16.c $(PROTO)/buzz_proto.c $(PROTO)/buzz_reasm.c $(VECTOR_SRC)
HDRS := $(PROTO)/crc16.h $(PROTO)/buzz_proto.h $(PROTO)/buzz_reasm.h $(VECTOR_HDR) test.h
TESTS := test_crc16 test_frame test_reasm test_vector_replay

.PHONY: test clean
test: $(TESTS)
	@for t in $(TESTS); do ./$(BUILD)/$$t || exit 1; done

$(TESTS): %: %.c $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(PROTO) -I$(GENERATED) -o $(BUILD)/$@ $< $(SRCS)

# 生成物一次产出 .c/.h 两个文件：.h 仅作同批声明，避免 macOS make 3.81 跑两遍生成
$(VECTOR_HDR): $(VECTOR_SRC)
$(VECTOR_SRC): $(VECTOR_JSON) $(VECTOR_TOOL)
	@mkdir -p $(GENERATED)
	python3 $(VECTOR_TOOL) $(VECTOR_JSON) $(GENERATED)

clean:
	rm -rf $(BUILD) $(GENERATED)
