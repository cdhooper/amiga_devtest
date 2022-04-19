PROG := devtest
OBJDIR  := objs
SRCS    := devtest.c
OBJS    := $(SRCS:%.c=$(OBJDIR)/%.o)
CC      := m68k-amigaos-gcc
CFLAGS  := -Wall -Wno-pointer-sign -fomit-frame-pointer
CFLAGS  += -Wno-strict-aliasing
LDFLAGS = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -fomit-frame-pointer -mcrt=clib2 -lgcc -lc -lamiga

CFLAGS  += -Os
QUIET   := @
QUIET   :=

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

clean:
	rm -f $(OBJS) $(OBJDIR)/*.map $(OBJDIR)/*.lst
