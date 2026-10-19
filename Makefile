# Makefile - CH552 USB-SPI bridge firmware
#
# Targets: all (default), flash, clean
#
# Requires SDCC 4.x.  The toolchain can be pointed at a local build with
#     make SDCC=/path/to/sdcc-4.2.0/bin/sdcc
#
# Flashing uses the WCH ISP protocol:
#   - the stock WCHISPTool (Windows), or
#   - the open-source tools on Linux/macOS: librech551, ch552tool.

PROJECT := ch552_spi_bridge

# System clock. 24MHz gives the highest SPI rate the peripheral can
# sustain and leaves plenty of headroom for the USB full-speed device
# requirement (Fsys >= 6MHz).
FREQ_SYS ?= 24000000

# 14KB of user flash (16KB ROM minus the 2KB bootloader at 0x3800).
CODE_SIZE ?= 0x3800

# 1KB of on-chip xRAM.  The USB stack owns 0x0000-0x01FF (endpoint
# buffers plus the reply staging area), so the compiler starts at 0x0200.
XRAM_SIZE ?= 0x0200
XRAM_LOC ?= 0x0200

SDCC ?= sdcc
PACKIHX ?= packihx
OBJCOPY ?= objcopy
# Open-source ISP tool for the WCH bootloader (pip install ch55xtool).
# Put the board into ISP mode first (hold BOOT while plugging in USB).
FLASHER ?= ch55xtool

SRC_DIR := firmware/src
INC_DIR := firmware/include
BUILD_DIR := build

# Firmware sources
C_FILES := \
	$(SRC_DIR)/main.c \
	$(SRC_DIR)/usb.c \
	$(SRC_DIR)/protocol.c \
	$(SRC_DIR)/board.c \
	$(SRC_DIR)/spi.c \
	$(SRC_DIR)/debug.c

INCLUDES := -I$(INC_DIR)

CFLAGS := \
	-mmcs51 --model-small \
	--xram-size $(XRAM_SIZE) --xram-loc $(XRAM_LOC) \
	--code-size $(CODE_SIZE) \
	-DFREQ_SYS=$(FREQ_SYS) \
	$(INCLUDES) \
	--std-sdcc11 --opt-code-speed

# Extra headroom for the bootloader entry symbol; keep it explicit so the
# jump target is visible in the map file.
CFLAGS += -DBOOTLOADER_ENTRY=0x3800

# 32MHz is out of spec for the CH552 (marked "Reserved, disabled" since
# datasheet V1.1) and does not improve throughput.  Kept as an explicit
# opt-in for experiments only.
ifdef ALLOW_OVERCLOCK
CFLAGS += -DALLOW_OVERCLOCK=1
endif

RELS := $(C_FILES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.rel)
HDRS := $(wildcard $(INC_DIR)/*.h)

.PHONY: all flash clean

all: $(BUILD_DIR)/$(PROJECT).bin $(BUILD_DIR)/$(PROJECT).hex

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.rel: $(SRC_DIR)/%.c $(HDRS) | $(BUILD_DIR)
	$(SDCC) -c $(CFLAGS) -o $@ $<

$(BUILD_DIR)/$(PROJECT).ihx: $(RELS)
	cd $(BUILD_DIR) && $(SDCC) $(notdir $(RELS)) $(CFLAGS) -o $(PROJECT).ihx

$(BUILD_DIR)/$(PROJECT).hex: $(BUILD_DIR)/$(PROJECT).ihx
	$(PACKIHX) $< > $@

$(BUILD_DIR)/$(PROJECT).bin: $(BUILD_DIR)/$(PROJECT).ihx
	$(OBJCOPY) -I ihex -O binary $< $@

flash: $(BUILD_DIR)/$(PROJECT).bin
	$(FLASHER) -f $< -r

clean:
	rm -rf $(BUILD_DIR)
