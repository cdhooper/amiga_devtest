#
# Makefile to build devtest for AmigaOS/68k using Bebbo's GCC cross-compiler.
#

VER     ?= 1.6c

PROG    := devtest
OBJDIR  := objs
SRCS    := devtest.c
OBJS    := $(SRCS:%.c=$(OBJDIR)/%.o)
CC      := m68k-amigaos-gcc
CFLAGS  := -Wall -Wextra -Wno-pointer-sign -fomit-frame-pointer
CFLAGS  += -Wno-strict-aliasing
CFLAGS  += -Wpedantic -Wno-overflow

# clib2 crashes on exit under Kickstart 2.x
LDFLAGS := -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -fomit-frame-pointer -lgcc -lc -lamiga -mcrt=clib2

# -noixemul generates significantly larger binaries, but KS 2.x is supported
#LDFLAGS := -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -fomit-frame-pointer -lgcc -lc -lamiga -noixemul

#CFLAGS += -flto
#LDFLAGS += -flto

#VER := $(PROG)_$(shell awk '/char[[:space:]]*\*version =/{print $$7}' devtest.c)
PROGVER := $(PROG)_$(VER)

CFLAGS  += -Os -DVER=\"$(VER)\"
#CFLAGS  += -g

# If verbose is specified with no other targets, then build everything
ifeq ($(MAKECMDGOALS),verbose)
verbose: all
endif
ifeq (,$(filter verbose timed, $(MAKECMDGOALS)))
QUIET   := @
else
QUIET   :=
endif

ifeq (, $(shell which $(CC) 2>/dev/null ))
$(error "No $(CC) in PATH: maybe do PATH=$$PATH:/opt/amiga/bin")
endif

all: $(PROG)

define DEPEND_SRC
# The following line creates a rule for an object file to depend on a
# given source file.
$(patsubst %,$(OBJDIR)/%,$(filter-out $(OBJDIR)/%,$(basename $(1)).o)) $(filter $(OBJDIR)/%,$(basename $(1)).o): $(1)
endef
$(foreach SRCFILE,$(SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE))))

$(OBJS): Makefile | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS) -c $(filter %.c,$^) -o $@

$(PROG): $(OBJS)
	@echo Building $@
	$(QUIET)$(CC) $(OBJS) $(LDFLAGS) -o $@

$(OBJDIR):
	mkdir -p $@

zip:
	@echo Building $(PROGVER).zip
	$(QUIET)rm -rf $(PROG).zip $(PROGVER)
	$(QUIET)mkdir $(PROGVER)
	$(QUIET)cp -p $(PROG) README.md $(PROGVER)/
	$(QUIET)zip -rq $(PROGVER).zip $(PROGVER)
	$(QUIET)rm -rf $(PROGVER)

lha:
	@echo Building $(PROGVER).lha
	$(QUIET)rm -rf $(PROG).zip $(PROGVER)
	$(QUIET)mkdir $(PROGVER)
	$(QUIET)cp -p $(PROG) README.md $(PROGVER)/
	$(QUIET)lha -aq2 $(PROGVER).lha $(PROGVER)
	$(QUIET)rm -rf $(PROGVER)

clean:
	rm -f $(OBJS) $(OBJDIR)/*.map $(OBJDIR)/*.lst

FLINT_FILE=flexelint.lnt
flint:
	flexelint -v -w3 -I/opt/amiga/m68k-amigaos/ndk-include -I/opt/amiga/m68k-amigaos/sys-include -I/opt/amiga/m68k-amigaos/clib2/include flexelint.lnt $(SRCS)
