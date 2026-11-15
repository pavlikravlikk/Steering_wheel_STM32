# Host unit tests: plain gcc, no dependencies.   Run:  make -C tests
FW      := ../steeringwheel_firmware/Core
CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O1 -g -I. -I$(FW)/Inc \
           -fsanitize=address,undefined -fno-omit-frame-pointer
LDLIBS  := -lm

# Pure firmware modules (must not include any HAL header).
FW_SRCS := $(FW)/Src/hid_pack.c

TEST_SRCS := $(wildcard test_*.c)

OUT := build/run_tests

.PHONY: all test clean
all test: $(OUT)
	./$(OUT)

$(OUT): $(TEST_SRCS) $(FW_SRCS) test_runner.h $(wildcard $(FW)/Inc/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) $(TEST_SRCS) $(FW_SRCS) -o $@ $(LDLIBS)

clean:
	rm -rf build
