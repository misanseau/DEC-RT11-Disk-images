# Makefile for rt11dv - RT-11 / TSX+ virtual disk manipulator
#
# Builds a single executable `rt11dv` from the C sources in this
# directory.  Portable between Linux (gcc/clang) and Windows (MinGW).
# The MSVC build continues to use rt11dv.vcxproj / the .sln solution.
#
# Usage:
#   make              # optimised build ("release"), no debug symbols
#   make debug        # -O0 -g with assertions, extra warnings
#   make BUILD=debug  # same as `make debug`
#   make clean        # remove objects + executable
#   make run          # build and run the REPL
#   make test-dates   # build and run the date-format self-test
#
# Override compiler or flags on the command line, e.g.:
#   make CC=clang
#   make CFLAGS='-O3 -march=native'

# --- Configuration -----------------------------------------------------

BUILD   ?= release

CC      ?= gcc
RM      ?= rm -f
MKDIR_P ?= mkdir -p

TARGET  := rt11dv
OBJDIR  := build/$(BUILD)

# Every .c in the current directory is a source file.
SRCS := \
    main.c      \
    commands.c  \
    cmd_dir.c   \
    cmd_ods1_dir.c \
    mount.c     \
    mt.c        \
    ods1.c      \
    rad50.c     \
    rsts.c      \
    rt11.c      \
    tar.c       \
    util.c

OBJS := $(addprefix $(OBJDIR)/,$(SRCS:.c=.o))
DEPS := $(OBJS:.o=.d)

# Warnings shared by every build.  These were the flags the project was
# kept clean under during development.
WARN := -Wall -Wextra -Wpedantic -Wshadow

# -std=gnu99 gives us C99 plus POSIX defaults (fseeko/ftello need
# _POSIX_C_SOURCE under strict -std=c99, which is messy to set portably).
# -MMD -MP emits a .d file next to each .o capturing the exact set of
# headers that .c depends on, so editing a header triggers a rebuild of
# only the .c files that actually include it.
COMMON_CFLAGS := -std=gnu99 $(WARN) -MMD -MP

ifeq ($(BUILD),debug)
    CFLAGS  ?= -O0 -g -DDEBUG
else ifeq ($(BUILD),release)
    CFLAGS  ?= -O2 -DNDEBUG
else
    $(error Unknown BUILD='$(BUILD)' (expected 'release' or 'debug'))
endif

ALL_CFLAGS := $(COMMON_CFLAGS) $(CFLAGS)

# Windows hosts need no special link libraries for what rt11dv uses.
# On Linux nothing extra is required either; keep LDLIBS empty so a
# user can extend it without fighting defaults.
LDFLAGS ?=
LDLIBS  ?=

# --- Top-level rules ---------------------------------------------------

.PHONY: all debug release clean run help test-dates test-ods1

all: $(TARGET)

debug release:
	@$(MAKE) --no-print-directory BUILD=$@ all

$(TARGET): $(OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Pattern rule: compile one C source into an object in $(OBJDIR).
$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(OBJDIR):
	$(MKDIR_P) $(OBJDIR)

# Pull in the auto-generated header dependencies.  The `-` silences the
# initial "no such file" warning on a clean tree.
-include $(DEPS)

# --- Convenience targets ----------------------------------------------

clean:
	$(RM) -r build
	$(RM) $(TARGET) $(TARGET).exe

run: $(TARGET)
	./$(TARGET)

# Ad-hoc self-test for the V3/V5/V55 date encoders/decoders.  The test
# source lives next to the Makefile so it ships with the project.
test-dates: test_dates.c util.c rad50.c util.h rad50.h
	$(CC) $(ALL_CFLAGS) -I. test_dates.c util.c rad50.c -o $@ $(LDFLAGS) $(LDLIBS)
	./$@

# Self-test for the Files-11 ODS-1 home-block decoder.  Builds a synthetic
# home block in memory and checks ods1_parse_home / ods1_test against it.
test-ods1: test_ods1.c ods1.c rad50.c util.c ods1.h util.h rad50.h
	$(CC) $(ALL_CFLAGS) -I. test_ods1.c ods1.c rad50.c util.c -o $@ $(LDFLAGS) $(LDLIBS)
	./$@

help:
	@echo "rt11dv Makefile targets:"
	@echo "  make              - build release (default)"
	@echo "  make debug        - build with -O0 -g"
	@echo "  make release      - build with -O2"
	@echo "  make run          - build + launch the REPL"
	@echo "  make test-dates   - build + run the date-format self-test"
	@echo "  make test-ods1    - build + run the ODS-1 home-block self-test"
	@echo "  make clean        - remove build artifacts"
	@echo ""
	@echo "Override variables on the command line, e.g.:"
	@echo "  make CC=clang"
	@echo "  make CFLAGS='-O3 -march=native -flto'"
