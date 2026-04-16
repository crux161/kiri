# Makefile — builds libkiri (static + shared) and the kiri CLI.
#
# Targets:
#   make            -> library (static + shared) + CLI
#   make lib        -> library only
#   make cli        -> CLI only
#   make install    -> install to $(PREFIX) (default /usr/local)
#   make clean      -> remove build artifacts
#   make re         -> clean, build, run against bunny test video

CC        ?= clang
AR        ?= ar
PREFIX    ?= /usr/local

VERSION_MAJOR := 2
VERSION_MINOR := 0
VERSION_PATCH := 0

# Platform detection
UNAME_S   := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    SHLIB_EXT := dylib
    SHLIB_FLAGS := -dynamiclib -install_name @rpath/libkiri.dylib
else
    SHLIB_EXT := so
    SHLIB_FLAGS := -shared -Wl,-soname,libkiri.so.$(VERSION_MAJOR)
endif

TEST_FILE := ./big_buck_bunny_1080p_h264.mov

# --- FFmpeg discovery -----------------------------------------------------
# Set OSKI_PREFIX to an Oski macOS shared output (the directory containing
# bin/, lib/, include/) to build against Oski instead of the system FFmpeg.
#
#   make OSKI_PREFIX=../oski/dist/macos/oski-8.0-macos-shared
#
OSKI_PREFIX ?=

ifdef OSKI_PREFIX
    _OSKI_ABS   := $(abspath $(OSKI_PREFIX))
    _PKG_CONFIG_PATH := $(_OSKI_ABS)/lib/pkgconfig
    FFMPEG_CFLAGS := $(shell PKG_CONFIG_PATH="$(_PKG_CONFIG_PATH)" pkg-config --cflags libavformat libavcodec libavutil)
    FFMPEG_LIBS   := $(shell PKG_CONFIG_PATH="$(_PKG_CONFIG_PATH)" pkg-config --libs   libavformat libavcodec libavutil)
    OSKI_RPATH    := -Wl,-rpath,$(_OSKI_ABS)/lib
else
    FFMPEG_CFLAGS := $(shell pkg-config --cflags libavformat libavcodec libavutil)
    FFMPEG_LIBS   := $(shell pkg-config --libs   libavformat libavcodec libavutil)
    OSKI_RPATH    :=
endif

CFLAGS     ?= -O2 -g
CFLAGS     += -Wall -Wextra -std=c17 -fvisibility=hidden $(FFMPEG_CFLAGS)
CFLAGS_PIC := $(CFLAGS) -fPIC -DKIRI_BUILDING_SHARED

LIB_SRC := kiri.c
LIB_HDR := kiri.h
CLI_SRC := kiri_cli.c

LIB_OBJ_PIC := kiri.pic.o
LIB_OBJ_A   := kiri.o
CLI_OBJ     := kiri_cli.o

STATIC_LIB := libkiri.a
SHARED_LIB := libkiri.$(SHLIB_EXT)
CLI_BIN    := kiri

.PHONY: all lib cli clean install re

all: lib cli

lib: $(STATIC_LIB) $(SHARED_LIB)

cli: $(CLI_BIN)

# --- Object files -----------------------------------------------------
$(LIB_OBJ_A): $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -c $(LIB_SRC) -o $@

$(LIB_OBJ_PIC): $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS_PIC) -c $(LIB_SRC) -o $@

$(CLI_OBJ): $(CLI_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -c $(CLI_SRC) -o $@

# --- Libraries --------------------------------------------------------
$(STATIC_LIB): $(LIB_OBJ_A)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(LIB_OBJ_PIC)
	$(CC) $(SHLIB_FLAGS) $(OSKI_RPATH) -o $@ $^ $(FFMPEG_LIBS)

# --- CLI binary (dynamically linked against libkiri) ------------------
# Use -rpath so the binary finds libkiri next to itself
# (macOS: @loader_path; Linux: $ORIGIN).
ifeq ($(UNAME_S),Darwin)
RPATH_FLAGS := -Wl,-rpath,@loader_path
else
RPATH_FLAGS := -Wl,-rpath,'$$ORIGIN'
endif

$(CLI_BIN): $(CLI_OBJ) $(SHARED_LIB)
	$(CC) $(CLI_OBJ) -L. -lkiri $(RPATH_FLAGS) $(OSKI_RPATH) -o $@

# --- Install ----------------------------------------------------------
install: all
	install -d $(PREFIX)/lib $(PREFIX)/include $(PREFIX)/bin
	install -m 0644 $(STATIC_LIB) $(PREFIX)/lib/
	install -m 0755 $(SHARED_LIB) $(PREFIX)/lib/
	install -m 0644 $(LIB_HDR)    $(PREFIX)/include/
	install -m 0755 $(CLI_BIN)    $(PREFIX)/bin/

# --- Cleanup ----------------------------------------------------------
clean:
	rm -f $(LIB_OBJ_A) $(LIB_OBJ_PIC) $(CLI_OBJ) \
	      $(STATIC_LIB) $(SHARED_LIB) $(CLI_BIN)

re: clean all
	@if [ -x ./get_test_data.sh ]; then ./get_test_data.sh; fi
	@if [ -f $(TEST_FILE) ]; then ./kiri $(TEST_FILE); fi
