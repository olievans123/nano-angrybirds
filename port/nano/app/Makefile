# Angry Birds (1.0, 2009). The game is built by the port tree (make -C <port>/nano) into
# libab_nano.a (ARM state, VFPv3-D16 softfp); this app links it with the SDK.
APP_NAME          := angrybirds
SRCS              := ab_entry.c ../../sdk/hb_surface_input.c
RELOC             := 1
EXTRA_CFLAGS      += -DHB_GL_SURFACE_APP -DHB_LV_RELOC
CHECK_NO_THUMB_FP := 1
AB_DIR            ?= $(HOME)/nano-ab/port
include ../../sdk/hb_app.mk

# newlib's ARM-state softfp multilib (no Thumb VFP code; see tools/check_no_thumb_fp.py).
NEWLIB_FLAGS  := -marm -march=armv5te+fp -mfloat-abi=softfp
LDLIBS        := $(AB_DIR)/build/nano/libab_nano.a \
                 $(shell $(CC) $(NEWLIB_FLAGS) -print-file-name=libc.a) $(LIBGCC)
$(BUILD)/$(APP_NAME).reloc.elf: $(AB_DIR)/build/nano/libab_nano.a
