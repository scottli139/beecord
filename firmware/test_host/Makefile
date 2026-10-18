# BuzzLink 协议层宿主单测：直接编译 ../main/proto 源码（路径引用，不复制），零三方依赖。
# 本机默认 clang（macOS）；CI 用 `make CC=gcc`（ubuntu）做双编译器零警告门禁。
CC     := clang
CFLAGS := -std=c11 -Wall -Wextra -Werror -g
PROTO  := ../main/proto
BUILD  := build

SRCS := $(PROTO)/crc16.c $(PROTO)/buzz_proto.c $(PROTO)/buzz_reasm.c
HDRS := $(PROTO)/crc16.h $(PROTO)/buzz_proto.h $(PROTO)/buzz_reasm.h test.h
TESTS := test_crc16 test_frame test_reasm

.PHONY: test clean
test: $(TESTS)
	@for t in $(TESTS); do ./$(BUILD)/$$t || exit 1; done

$(TESTS): %: %.c $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(PROTO) -o $(BUILD)/$@ $< $(SRCS)

clean:
	rm -rf $(BUILD)
