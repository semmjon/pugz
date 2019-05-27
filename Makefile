#
# Use 'make help' to list available targets.
#
# Define V=1 to enable "verbose" mode, showing all executed commands.
#
# Define DECOMPRESSION_ONLY to omit all compression code, building a
# decompression-only library.  If doing this, you must also build a specific
# library target such as 'libdeflate.a', as the programs will no longer compile.
#
# Define DISABLE_GZIP to disable support for the gzip wrapper format.
#
##############################################################################

#### Common compiler flags.
#### Flags given here are not intended to be overridden, but you can add more
#### by defining CFLAGS in the environment or on the 'make' command line.

cc-option = $(shell if $(CXX) $(1) -c -x c /dev/null -o /dev/null \
	      1>&2 2>/dev/null; then echo $(1); else echo $(2); fi)

WARNS := -Wall -Wextra -Weffc++ -Wpedantic -Wundef -Wuseless-cast -Wconversion -Wshadow
WARNS += -Wdisabled-optimization -Wparentheses -Wpointer-arith
WARNS += $(cc-option -Wnull-dereference)
WARNS += $(cc-option -Wno-ignored-attributes)

override CFLAGS += -std=c++11 -mssse3 -I. -Icommon $(WARNS)

##############################################################################

STATIC_LIB_SUFFIX := .a
SHARED_LIB_SUFFIX := .so
SHARED_LIB_CFLAGS := -fPIC
PROG_SUFFIX       :=
PROG_CFLAGS       :=


#debugging options for decompression (Rayan)

# Enable assertion by default (for now)
ifeq ($(debug),1)
    asserts=1
    override CFLAGS += -O0 -g
else
    override CFLAGS += -O3 $(call cc-option,-flto=jobserver,-flto) -march=native -mtune=native -g
endif

ifndef asserts
    asserts=1
endif

ifneq ($(asserts),1)
     override CFLAGS+= -DNDEBUG
endif

ifeq ($(print_debug),1)
     override CFLAGS+= -DPRINT_DEBUG=1
endif

ifeq ($(print_debug_decoding),1)
     override CFLAGS+= -DPRINT_DEBUG_DECODING=1
endif

# Compiling for Windows with MinGW?
ifneq ($(findstring -mingw,$(shell $(CXX) -dumpmachine 2>/dev/null)),)
    STATIC_LIB_SUFFIX := .lib
    SHARED_LIB_SUFFIX := .dll
    SHARED_LIB_CFLAGS :=
    PROG_SUFFIX       := .exe
    PROG_CFLAGS       := -static -municode
    HARD_LINKS        :=
    override CFLAGS   := $(CFLAGS) $(call cc-option,-Wno-pedantic-ms-format)

    # If AR was not already overridden, then derive it from $(CXX).
    # Note that CC may take different forms, e.g. "cc", "gcc",
    # "x86_64-w64-mingw32-gcc", or "x86_64-w64-mingw32-gcc-6.3.1".
    # On Windows it may also have a .exe extension.
    ifeq ($(AR),ar)
        AR := $(shell echo $(CXX) | \
                sed -E 's/g?cc(-?[0-9]+(\.[0-9]+)*)?(\.exe)?$$/ar\3/')
    endif
endif

##############################################################################

#### Quiet make is enabled by default.  Define V=1 to disable.

ifneq ($(findstring s,$(MAKEFLAGS)),s)
ifneq ($(V),1)
        QUIET_CC       = @echo '  CXX     ' $@;
        QUIET_CCLD     = @echo '  CCLD    ' $@;
        QUIET_AR       = @echo '  AR      ' $@;
        QUIET_LN       = @echo '  LN      ' $@;
        QUIET_CP       = @echo '  CP      ' $@;
        QUIET_GEN      = @echo '  GEN     ' $@;
endif
endif

##############################################################################

COMMON_HEADERS := $(wildcard common/*.h)
DEFAULT_TARGETS :=

#### Library

STATIC_LIB := libdeflate$(STATIC_LIB_SUFFIX)
SHARED_LIB := libdeflate$(SHARED_LIB_SUFFIX)

LIB_CFLAGS += $(CFLAGS) -fvisibility=hidden -D_ANSI_SOURCE

LIB_HEADERS := $(wildcard lib/*.h) $(wildcard lib/*.hpp)

LIB_SRC :=
LIB_SRC_CXX :=
ifndef DISABLE_GZIP
    LIB_SRC_CXX +=
endif

STATIC_LIB_OBJ := $(LIB_SRC:.c=.o)
STATIC_LIB_OBJ_CXX := $(LIB_SRC_CXX:.cpp=.o)
SHARED_LIB_OBJ := $(LIB_SRC:.c=.shlib.o)

# Compile static library object files
$(STATIC_LIB_OBJ): %.o: %.c $(LIB_HEADERS) $(COMMON_HEADERS) .lib-cflags
	+$(QUIET_CC) $(CXX) -o $@ -c $(LIB_CFLAGS) $<
$(STATIC_LIB_OBJ_CXX): %.o: %.cpp $(LIB_HEADERS) $(COMMON_HEADERS) .lib-cflags
	+$(QUIET_CC) $(CXX) -o $@ -c $(LIB_CFLAGS) $<

# Compile shared library object files
$(SHARED_LIB_OBJ): %.shlib.o: %.c $(LIB_HEADERS) $(COMMON_HEADERS) .lib-cflags
	+$(QUIET_CC) $(CXX) -o $@ -c $(LIB_CFLAGS) $(SHARED_LIB_CFLAGS) -DLIBDEFLATE_DLL $<

# Create static library
$(STATIC_LIB):$(STATIC_LIB_OBJ) $(STATIC_LIB_OBJ_CXX)
	+$(QUIET_AR) $(AR) cr $@ $+

DEFAULT_TARGETS += $(STATIC_LIB)

# Create shared library
$(SHARED_LIB):$(SHARED_LIB_OBJ)
	+$(QUIET_CCLD) $(CXX) -o $@ $(LDFLAGS) $(LIB_CFLAGS) -shared $+

#DEFAULT_TARGETS += $(SHARED_LIB)

# Rebuild if CC or LIB_CFLAGS changed
.lib-cflags: FORCE
	@flags='$(CXX):$(LIB_CFLAGS)'; \
	if [ "$$flags" != "`cat $@ 2>/dev/null`" ]; then \
		[ -e $@ ] && echo "Rebuilding library due to new compiler flags"; \
		echo "$$flags" > $@; \
	fi

##############################################################################

#### Programs

PROG_CFLAGS += $(CFLAGS)		 \
	       -D_POSIX_C_SOURCE=200809L \
	       -D_FILE_OFFSET_BITS=64

PROG_COMMON_HEADERS := programs/prog_util.h  $(LIB_HEADERS)
PROG_COMMON_SRC     := programs/prog_util.cpp programs/tgetopt.cpp
NONTEST_PROGRAM_SRC := programs/gunzip.cpp
TEST_PROGRAM_SRC    := programs/benchmark.cpp programs/test_checksums.cpp \
			programs/checksum.cpp

NONTEST_PROGRAMS := $(NONTEST_PROGRAM_SRC:programs/%.cpp=%$(PROG_SUFFIX))
DEFAULT_TARGETS  += $(NONTEST_PROGRAMS)
TEST_PROGRAMS    := $(TEST_PROGRAM_SRC:programs/%.cpp=%$(PROG_SUFFIX))

PROG_COMMON_OBJ     := $(PROG_COMMON_SRC:%.cpp=%.o)
NONTEST_PROGRAM_OBJ := $(NONTEST_PROGRAM_SRC:%.cpp=%.o)
TEST_PROGRAM_OBJ    := $(TEST_PROGRAM_SRC:%.cpp=%.o)
PROG_OBJ := $(PROG_COMMON_OBJ) $(NONTEST_PROGRAM_OBJ) $(TEST_PROGRAM_OBJ)


# Compile program object files
$(PROG_OBJ): %.o: %.cpp $(PROG_COMMON_HEADERS) $(COMMON_HEADERS) .prog-cflags
	+$(QUIET_CC) $(CXX) -o $@ -c $(PROG_CFLAGS) $<

# Link the programs.
#
# Note: the test programs are not compiled by default.  One reason is that the
# test programs must be linked with zlib for doing comparisons.

$(NONTEST_PROGRAMS): %$(PROG_SUFFIX): programs/%.o $(PROG_COMMON_OBJ) $(STATIC_LIB)
	+$(QUIET_CCLD) $(CXX) -o $@ $(LDFLAGS) $(PROG_CFLAGS) $+ -lpthread -lrt


DEFAULT_TARGETS += gunzip$(PROG_SUFFIX)

# Rebuild if CC or PROG_CFLAGS changed
.prog-cflags: FORCE
	@flags='$(CXX):$(PROG_CFLAGS)'; \
	if [ "$$flags" != "`cat $@ 2>/dev/null`" ]; then \
		[ -e $@ ] && echo "Rebuilding programs due to new compiler flags"; \
		echo "$$flags" > $@; \
	fi

##############################################################################

all:$(DEFAULT_TARGETS)

test_programs:$(TEST_PROGRAMS)

help:
	@echo "Available targets:"
	@echo "------------------"
	@for target in $(DEFAULT_TARGETS) $(TEST_PROGRAMS); do \
		echo -e "$$target";		\
	done

clean:
	rm -f *.a *.dll *.exe *.exp *.so \
		lib/*.o lib/*.obj lib/*.dllobj \
		programs/*.o programs/*.obj \
		$(DEFAULT_TARGETS) $(TEST_PROGRAMS) \
		libdeflate.lib libdeflatestatic.lib \
		.lib-cflags .prog-cflags

realclean: clean
	rm -f tags cscope* run_tests.log

FORCE:

.PHONY: all test_programs help clean realclean

.DEFAULT_GOAL = all
