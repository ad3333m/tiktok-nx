#---------------------------------------------------------------------------------
# TikTok for Switch - devkitA64 build. Target devkitpro/devkita64 CI container.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := TikTok
BUILD       := build
SOURCES     := source source/util source/net source/ui source/tiktok source/app source/platform/switch third_party
DATA        := data
INCLUDES    := source third_party
ROMFS       := romfs
APP_TITLE   := TikTok
APP_AUTHOR  := Adam
APP_VERSION := 1.0.0
ifneq ($(wildcard $(TOPDIR)/icon.jpg),)
ICON        := icon.jpg
endif

PKGCONF := $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config
PKGS    := sdl2 freetype2 libcurl libwebp libavformat libavcodec libswresample libswscale libavutil

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS   := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES) $(INCLUDE) -D__SWITCH__ \
            $(shell $(PKGCONF) --cflags $(PKGS))
CXXFLAGS := $(CFLAGS) -std=gnu++20 -fexceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS     := -lwebpdemux $(shell $(PKGCONF) --static --libs $(PKGS)) -lz -lnx
LIBDIRS  := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD := $(CXX)
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_SRC)
export INCLUDE    := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS   := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
ifneq ($(ICON),)
export APP_ICON   := $(TOPDIR)/$(ICON)
else
export APP_ICON   := $(LIBNX)/default_icon.jpg
endif
export NROFLAGS   += --icon=$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp --romfsdir=$(CURDIR)/$(ROMFS)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
