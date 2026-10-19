# BuzzLink 协议层宿主单测：直接编译 ../main/proto 与 ../main/audio 源码（路径引用，不复制），零三方依赖。
# 本机默认 clang（macOS）；CI 用 `make CC=gcc`（ubuntu）做双编译器零警告门禁。
CC     := clang
CFLAGS := -std=c11 -Wall -Wextra -Werror -g
PROTO  := ../main/proto
AUDIO  := ../main/audio
BUILD  := build

# 会话向量回放（TESTING.md §3）：JSON 或转换脚本变更时 make 自动重新生成 C 表
VECTOR_JSON := ../../apps/mac/Packages/BuzzLink/Tests/BuzzLinkCoreTests/Fixtures/basic-session.json
VECTOR_TOOL := ../../tools/vector2c.py
GENERATED   := generated
VECTOR_SRC  := $(GENERATED)/vectors_basic_session.c
VECTOR_HDR  := $(GENERATED)/vectors_basic_session.h

# Ogg 端到端对拍：测试落盘 .ogg 后由独立 Python 校验器复核（坏页文件必须被拒）
OGG_CHECK := ../../tools/ogg_check.py

SRCS := $(PROTO)/crc16.c $(PROTO)/buzz_proto.c $(PROTO)/buzz_reasm.c $(AUDIO)/ogg_writer.c $(AUDIO)/wav_writer.c $(VECTOR_SRC)
HDRS := $(PROTO)/crc16.h $(PROTO)/buzz_proto.h $(PROTO)/buzz_reasm.h $(AUDIO)/ogg_writer.h $(AUDIO)/wav_writer.h $(VECTOR_HDR) test.h
TESTS := test_crc16 test_frame test_reasm test_vector_replay test_ogg_writer test_wav_writer

.PHONY: test clean
test: $(TESTS)
	@for t in $(TESTS); do ./$(BUILD)/$$t || exit 1; done
	@echo "--- ogg_check 对拍 ---"
	python3 $(OGG_CHECK) $(BUILD)/test_e2e.ogg
	@if python3 $(OGG_CHECK) $(BUILD)/test_bad_page.ogg > /dev/null 2>&1; then \
		echo "反向验证失败：坏页通过校验"; exit 1; \
	else \
		echo "反向验证通过：坏页被正确拒绝"; \
	fi

$(TESTS): %: %.c $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(PROTO) -I$(AUDIO) -I$(GENERATED) -o $(BUILD)/$@ $< $(SRCS)

# 生成物一次产出 .c/.h 两个文件：.h 仅作同批声明，避免 macOS make 3.81 双跑生成
$(VECTOR_HDR): $(VECTOR_SRC)
$(VECTOR_SRC): $(VECTOR_JSON) $(VECTOR_TOOL)
	@mkdir -p $(GENERATED)
	python3 $(VECTOR_TOOL) $(VECTOR_JSON) $(GENERATED)

clean:
	rm -rf $(BUILD) $(GENERATED)
