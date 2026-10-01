# Angry Birds 1.0 engine for the iPod nano 7G: builds build/nano/libab_nano.a, which the
# NanoApps app (nano/app, copied to <NanoApps>/apps/angrybirds) links.
#
#   make -C nano NANOAPPS=~/nano-ab/NanoApps        (on the mini PC: needs arm-none-eabi + newlib)
#
# Flags as the Mario Kart 64 port: ARM state with the hardware FPU, and every absolute
# address as a 32-bit word (the NanoApps loader only rebases R_ARM_ABS32).
NANOAPPS ?= $(HOME)/NanoApps
ROOT     := ..
BUILD    := $(ROOT)/build/nano
CC       := arm-none-eabi-gcc
CXX      := arm-none-eabi-g++
AR       := arm-none-eabi-ar

ARCH     := -marm -mcpu=cortex-a5 -mfloat-abi=softfp -mfpu=vfpv3-d16 -mword-relocations
COMMON   := $(ARCH) -O3 -ffunction-sections -fdata-sections -fno-common -fno-strict-aliasing \
            -fno-builtin -MMD -MP \
            -DNDEBUG -DAB_NANO \
            -I$(NANOAPPS)/sdk -I$(ROOT)/src -I$(ROOT)/lua -I$(ROOT)
CFLAGS   := $(COMMON) -std=gnu99 -Wall
CXXFLAGS := $(COMMON) -std=gnu++11 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit \
            -Wall -Wno-unused-value

LUA_SRCS := lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring \
            ltable ltm lundump lvm lzio lauxlib lbaselib ltablib lstrlib
ENGINE_C := gfx assets render resources lua_libs numfmt
ENGINE_CXX := game game_render particles
NANO_C   := nano_platform nano_libc nano_math nano_perf nano_alloc nano_memory
B2_SRCS  := $(shell cd $(ROOT) && find Box2D -name '*.cpp')

OBJS := $(LUA_SRCS:%=$(BUILD)/lua/%.o) $(ENGINE_C:%=$(BUILD)/src/%.o) $(ENGINE_CXX:%=$(BUILD)/src/%.o) \
        $(B2_SRCS:%.cpp=$(BUILD)/%.o) $(NANO_C:%=$(BUILD)/nano/%.o) $(BUILD)/nano/nano_cxx.o

all: $(BUILD)/libab_nano.a

$(BUILD)/libab_nano.a: $(OBJS)
	@rm -f $@
	$(AR) crs $@ $(OBJS)
	@# static constructors would need .init_array entries, which the app loader does not relocate
	@if arm-none-eabi-objdump -h $(OBJS) | grep -q 'init_array'; then \
		echo "error: an object has static constructors (.init_array):"; \
		for o in $(OBJS); do arm-none-eabi-objdump -h $$o | grep -q init_array && echo "  $$o"; done; \
		rm -f $@; exit 1; fi
	@arm-none-eabi-size -t $@ | tail -1

$(BUILD)/lua/%.o: $(ROOT)/lua/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c $< -o $@

$(BUILD)/src/%.o: $(ROOT)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/src/%.o: $(ROOT)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/Box2D/%.o: $(ROOT)/Box2D/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -w -c $< -o $@

$(BUILD)/nano/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/nano/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean

-include $(OBJS:.o=.d)
