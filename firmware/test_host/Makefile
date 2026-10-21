# BuzzLink 协议层宿主单测：直接编译 ../main/proto 与 ../main/audio 源码（路径引用，不复制），零三方依赖。
# 本机默认 clang（macOS）；CI 用 `make CC=gcc`（ubuntu）做双编译器零警告门禁。
CC     := clang
CFLAGS := -std=c11 -Wall -Wextra -Werror -g
PROTO  := ../main/proto
AUDIO  := ../main/audio
BLE    := ../main/ble
STORAGE := ../main/storage
BUILD  := build

# 会话向量回放（TESTING.md §3）：JSON 或转换脚本变更时 make 自动重新生成 C 表
VECTOR_JSON := ../../apps/mac/Packages/BuzzLink/Tests/BuzzLinkCoreTests/Fixtures/basic-session.json
VECTOR_TOOL := ../../tools/vector2c.py
GENERATED   := generated
VECTOR_SRC  := $(GENERATED)/vectors_basic_session.c
VECTOR_HDR  := $(GENERATED)/vectors_basic_session.h

# Ogg 端到端对拍：测试落盘 .ogg 后由独立 Python 校验器复核（坏页文件必须被拒）
OGG_CHECK := ../../tools/ogg_check.py

# libopus roundtrip（M3 rec_pipe）：pkg-config 检测宿主 libopus，有才编入 test_rec_pipe；
# 无库则跳过并打印醒目提示（CI 已装 libopus-dev，不允许静默跳过）
OPUS_CFLAGS := $(shell pkg-config --cflags opus 2>/dev/null)
OPUS_LIBS   := $(shell pkg-config --libs opus 2>/dev/null)
HAVE_OPUS   := $(shell pkg-config --exists opus > /dev/null 2>&1 && echo 1)
PIPE_SRCS   := $(AUDIO)/opus_enc.c $(AUDIO)/rec_pipe.c
PIPE_HDRS   := $(AUDIO)/opus_enc.h $(AUDIO)/rec_pipe.h

SRCS := $(PROTO)/crc16.c $(PROTO)/buzz_proto.c $(PROTO)/buzz_reasm.c $(AUDIO)/ogg_writer.c $(AUDIO)/wav_writer.c $(BLE)/buzz_server.c $(STORAGE)/file_index.c $(VECTOR_SRC)
HDRS := $(PROTO)/crc16.h $(PROTO)/buzz_proto.h $(PROTO)/buzz_reasm.h $(AUDIO)/ogg_writer.h $(AUDIO)/wav_writer.h $(BLE)/buzz_server.h $(STORAGE)/file_index.h $(VECTOR_HDR) test.h
TESTS := test_crc16 test_frame test_reasm test_vector_replay test_ogg_writer test_wav_writer test_server test_file_index test_keyd test_beep test_rec_meter test_rec_journal

ifeq ($(HAVE_OPUS),1)
TESTS += test_rec_pipe
endif

FONT_CHECK := ../tools/check_font_charset.py

.PHONY: test clean fontcheck
fontcheck:
	python3 $(FONT_CHECK)

test: $(TESTS) fontcheck
	@for t in $(TESTS); do ./$(BUILD)/$$t || exit 1; done
	@echo "--- ogg_check 对拍 ---"
	python3 $(OGG_CHECK) $(BUILD)/test_e2e.ogg
ifeq ($(HAVE_OPUS),1)
	python3 $(OGG_CHECK) $(BUILD)/test_pipe.ogg
else
	@echo "!!! pkg-config 未找到 libopus：test_rec_pipe 已跳过（CI 已装 libopus-dev，不会走到这里）!!!"
endif
	@if python3 $(OGG_CHECK) $(BUILD)/test_bad_page.ogg > /dev/null 2>&1; then \
		echo "反向验证失败：坏页通过校验"; exit 1; \
	else \
		echo "反向验证通过：坏页被正确拒绝"; \
	fi

# keyd/beep（M5.2）：纯逻辑单测，仅链各自 sys 源码，独立显式规则
SYS := ../main/sys
test_keyd: test_keyd.c $(SYS)/keyd.c $(SYS)/keyd.h test.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(SYS) -o $(BUILD)/$@ $< $(SYS)/keyd.c

test_beep: test_beep.c $(SYS)/beep.c $(SYS)/beep.h test.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(SYS) -o $(BUILD)/$@ $< $(SYS)/beep.c

# rec_meter（M5.3）：电平算法，仅链自身（-lm 数学库）
test_rec_meter: test_rec_meter.c $(AUDIO)/rec_meter.c $(AUDIO)/rec_meter.h test.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(AUDIO) -o $(BUILD)/$@ $< $(AUDIO)/rec_meter.c -lm

# rec_journal（M5.3）：序列化/恢复决策纯逻辑，依赖 proto/crc16
test_rec_journal: test_rec_journal.c $(AUDIO)/rec_journal.c $(AUDIO)/rec_journal.h $(PROTO)/crc16.c test.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(AUDIO) -I$(PROTO) -o $(BUILD)/$@ $< $(AUDIO)/rec_journal.c $(PROTO)/crc16.c

# test_rec_pipe 需额外 libopus 头/库与 -lm（数学库），独立显式规则，不走下方模式规则
ifeq ($(HAVE_OPUS),1)
test_rec_pipe: test_rec_pipe.c $(SRCS) $(PIPE_SRCS) $(HDRS) $(PIPE_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(OPUS_CFLAGS) -I$(PROTO) -I$(AUDIO) -I$(GENERATED) \
	    -o $(BUILD)/test_rec_pipe $< $(SRCS) $(PIPE_SRCS) $(OPUS_LIBS) -lm
endif

# test_rec_pipe/keyd/beep 从模式规则中剔除，避免与上方显式规则重复给配方（macOS make 3.81 下同）
$(filter-out test_rec_pipe test_keyd test_beep test_rec_meter test_rec_journal,$(TESTS)): %: %.c $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(PROTO) -I$(AUDIO) -I$(BLE) -I$(STORAGE) -I$(GENERATED) -o $(BUILD)/$@ $< $(SRCS)

# 生成物一次产出 .c/.h 两个文件：.h 仅作同批声明，避免 macOS make 3.81 双跑生成
$(VECTOR_HDR): $(VECTOR_SRC)
$(VECTOR_SRC): $(VECTOR_JSON) $(VECTOR_TOOL)
	@mkdir -p $(GENERATED)
	python3 $(VECTOR_TOOL) $(VECTOR_JSON) $(GENERATED)

clean:
	rm -rf $(BUILD) $(GENERATED)
