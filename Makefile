# Makefile - CH552 USB-SPI bridge firmware
#
# Targets: all (default), flash, clean
#
# Requires SDCC 4.x.  The toolchain can be pointed at a local build with
#     make SDCC=/path/to/sdcc-4.2.0/bin/sdcc
#
# Flashing uses the WCH ISP protocol:
#   - the stock WCHISPTool (Windows), or
#   - the open-source ch55xtool on Linux/macOS (make flash).

PROJECT := ch552_spi_bridge

# The config stamp rule is defined before `all`; keep the default goal explicit.
.DEFAULT_GOAL := all

# Supply voltage.  The CH552 datasheet (table 8.2.2) requires VCC > 4.4 V
# for 24 MHz; a 3.3 V board must run at 12 MHz (16 MHz is marginal).
# An explicit FREQ_SYS=... on the command line still wins.
VCC ?= 5V
ifeq ($(VCC),3V3)
FREQ_SYS ?= 12000000
ifneq ($(filter-out 6000000 12000000,$(FREQ_SYS)),)
$(error VCC=3V3 supports only FREQ_SYS=6/12 MHz, got $(FREQ_SYS))
endif
else
FREQ_SYS ?= 24000000
endif

$(info Build: VCC=$(VCC), Fsys=$(FREQ_SYS) Hz, SPI max $(shell expr $(FREQ_SYS) / 2000000) MHz)

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

# Free-form extra flags, e.g. make EXTRA_FLAGS=-DDE_PRINTF=1 for UART debug.
CFLAGS += $(EXTRA_FLAGS)

RELS := $(C_FILES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.rel)
HDRS := $(wildcard $(INC_DIR)/*.h)

# make does not track CFLAGS, so switching FREQ_SYS/VCC used to reuse stale
# objects and silently produce a binary for the old clock.  Keep the flags in
# a stamp file and make every object depend on it.
CFG_STAMP := $(BUILD_DIR)/.config

.PHONY: all flash clean FORCE

FORCE:

$(CFG_STAMP): FORCE
	@mkdir -p $(BUILD_DIR)
	@echo '$(CC) $(CFLAGS)' | cmp -s - $@ || echo '$(CC) $(CFLAGS)' > $@

all: $(BUILD_DIR)/$(PROJECT).bin $(BUILD_DIR)/$(PROJECT).hex

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.rel: $(SRC_DIR)/%.c $(HDRS) $(CFG_STAMP) | $(BUILD_DIR)
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
