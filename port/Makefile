# Angry Birds 1.0 engine re-implementation: host (Mac) test build.
# The nano build is in nano/Makefile (run on the mini PC, which has newlib).
BUILD   := build/host
CC      := cc
CXX     := c++
CFLAGS  := -O2 -g -Wall -Wno-deprecated-declarations -Ihost -Isrc -Ilua -I.
CXXFLAGS := $(CFLAGS) -std=c++11 -fno-exceptions -fno-rtti -Wno-unused-private-field -Wno-unused-value

LUA_SRCS := lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring \
            ltable ltm lundump lvm lzio lauxlib lbaselib ltablib lstrlib
ENGINE_C := gfx assets render resources lua_libs numfmt
ENGINE_CXX := game game_render particles
B2_SRCS := $(shell find Box2D -name '*.cpp')

OBJS := $(LUA_SRCS:%=$(BUILD)/lua/%.o) $(ENGINE_C:%=$(BUILD)/src/%.o) $(ENGINE_CXX:%=$(BUILD)/src/%.o) \
        $(B2_SRCS:%.cpp=$(BUILD)/%.o) $(BUILD)/host/host_main.o

all: build/abhost

build/abhost: $(OBJS)
	$(CXX) -o $@ $(OBJS) -framework OpenGL -framework GLUT -lz

$(BUILD)/lua/%.o: lua/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/src/%.o: src/*.h
$(BUILD)/host/%.o: src/*.h

clean:
	rm -rf $(BUILD) build/abhost

.PHONY: all clean
