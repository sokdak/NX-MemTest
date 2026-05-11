ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to devkitpro>")
endif

include $(DEVKITPRO)/libnx/switch_rules
export LD := $(CC)

TARGET := NX-MemTest
BUILD := build
SOURCES := source/core source/nx
INCLUDES := include
ROMFS := romfs
OUT_SHADERS := shaders
LIBDIRS := $(PORTLIBS) $(LIBNX)
APP_TITLE := NX-MemTest
APP_AUTHOR := Codex
APP_VERSION := 0.1.0

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -g -Wall -Wextra -O3 -ffunction-sections -fdata-sections -ftree-vectorize
CFLAGS += $(ARCH) $(DEFINES) $(INCLUDE)
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS := -g $(ARCH)
LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) $(LIBPATHS) -Wl,-Map,$(notdir $*.map)
LIBS := -ldeko3d -lnx

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
GLSLFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.glsl)))
export OFILES := $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) $(foreach dir,$(LIBDIRS),-I$(dir)/include)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifneq ($(strip $(ROMFS)),)
ROMFS_SHADERS := $(ROMFS)/$(OUT_SHADERS)
ROMFS_TARGETS := $(patsubst %.glsl, $(ROMFS_SHADERS)/%.dksh, $(GLSLFILES))
NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
export NROFLAGS
endif

.PHONY: all clean $(BUILD) shaders

all: $(BUILD)

shaders: $(ROMFS_TARGETS)

$(ROMFS_SHADERS):
	@[ -d $@ ] || mkdir -p $@

$(ROMFS_SHADERS)/%.dksh: $(CURDIR)/source/nx/%.glsl | $(ROMFS_SHADERS)
	@echo "  uam comp $(notdir $<)"
	@uam -s comp -o $@ $<

$(BUILD): shaders
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -rf $(BUILD) $(ROMFS_SHADERS) $(TARGET).elf $(TARGET).nro $(TARGET).nacp $(TARGET).map

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).nro: $(OUTPUT).elf

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
