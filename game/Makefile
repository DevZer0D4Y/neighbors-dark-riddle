.DEFAULT_GOAL := all
DEVKITPRO ?= /opt/devkitpro
CXX       := aarch64-none-elf-g++
CC        := aarch64-none-elf-gcc
TARGET    := darkriddle
BUILD     := build
SRC       := src
CPPFILES  := $(wildcard $(SRC)/*.cpp)
OFILES    := $(patsubst $(SRC)/%.cpp,$(BUILD)/%.o,$(CPPFILES))
ARCH      := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS    := -g -O2 -ffunction-sections -fdata-sections -MMD -MP -D__SWITCH__
CXXFLAGS  := $(CFLAGS) $(ARCH) -std=gnu++17 -fno-exceptions -fno-rtti
LDFLAGS   := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(TARGET).map -Wl,--gc-sections
LIBS      := -lSDL2 -lGLESv2 -lEGL -lglapi -ldrm_nouveau -lnx
INCLUDES  := -I$(SRC) -I$(DEVKITPRO)/libnx/include -I$(DEVKITPRO)/portlibs/switch/include
LIBDIRS   := -L$(DEVKITPRO)/libnx/lib -L$(DEVKITPRO)/portlibs/switch/lib

$(BUILD)/%.o: $(SRC)/%.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET).elf: $(OFILES)
	$(CXX) $(LDFLAGS) $(OFILES) $(LIBDIRS) $(LIBS) -o $@

$(TARGET).nacp: Makefile
	nacptool --create "Dark Riddle" "Switch port" "1.0.0" $@

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp $(wildcard romfs/*)
	elf2nro $< $@ --nacp=$(TARGET).nacp --romfsdir=$(CURDIR)/romfs

all: $(TARGET).nro

clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).nro $(TARGET).nacp $(TARGET).map

-include $(OFILES:.o=.d)
