# Host unit tests: plain gcc, no dependencies.   Run:  make -C tests
FW      := ../steeringwheel_firmware/Core
CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O1 -g -I. -I$(FW)/Inc -I$(FW)/Inc/ffb \
           -fsanitize=address,undefined -fno-omit-frame-pointer
LDLIBS  := -lm

# Pure firmware modules (must not include any HAL header).
FW_SRCS := $(FW)/Src/hid_pack.c \
           $(FW)/Src/odrive_ascii.c \
           $(FW)/Src/steering_map.c \
           $(FW)/Src/odrive_link.c \
           $(FW)/Src/debounce.c \
           $(FW)/Src/mcp23017_core.c \
           $(FW)/Src/centering_law.c \
           $(FW)/Src/settings.c \
           $(FW)/Src/axis_shape.c \
           $(FW)/Src/board_health.c
CFLAGS  += -DBOARD_HEALTH_HOST
# The link tests use fixed numbers computed for a 540 deg lock-to-lock. Pin it here so the tests do not
# change when the profile (config/hardware_profile.toml, now 900 deg) is edited.
CFLAGS  += -DWHEEL_LOCK_TO_LOCK_DEG=540
# Same for the direction: the profile now says invert = true (measured on the bench); the link tests expect +X for +turns.
CFLAGS  += -DODRIVE_INVERT_DIRECTION=0

# Pure force-feedback modules (Core/Src/ffb). ffb_board.c and usbd_ffb_hid.c
# are hardware glue and are only compiled into the firmware.
FFB_SRCS := $(filter-out $(FW)/Src/ffb/ffb_board.c $(FW)/Src/ffb/usbd_ffb_hid.c, \
              $(wildcard $(FW)/Src/ffb/*.c))
FW_SRCS  += $(FFB_SRCS)

TEST_SRCS := $(wildcard test_*.c) $(wildcard mock_*.c)

OUT := build/run_tests

.PHONY: all test clean
all test: $(OUT)
	./$(OUT)

$(OUT): $(TEST_SRCS) $(FW_SRCS) $(wildcard *.h) $(wildcard $(FW)/Inc/*.h) $(wildcard $(FW)/Inc/ffb/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) $(TEST_SRCS) $(FW_SRCS) -o $@ $(LDLIBS)

clean:
	rm -rf build
