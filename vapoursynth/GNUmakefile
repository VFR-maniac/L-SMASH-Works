#----------------------------------------------------------------------------------------------
#  Makefile for VapourSynth plugins
#----------------------------------------------------------------------------------------------

include config.mak

vpath %.c $(SRCDIR)
vpath %.h $(SRCDIR)

OBJ_SOURCE = $(SRC_SOURCE:%.c=%.o)

SRC_ALL = $(SRC_SOURCE)

ifneq ($(STRIP),)
LDFLAGS += -Wl,-s
endif

.PHONY: all clean distclean dep

all: vslsmashsource.dll

vslsmashsource.dll: $(OBJ_SOURCE)
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c .depend
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	$(RM) *.dll *.o .depend

distclean: clean
	$(RM) config.*

dep: .depend

ifneq ($(wildcard .depend),)
include .depend
endif

.depend: config.mak
	@$(RM) .depend
	@$(foreach SRC, $(SRC_ALL:%=$(SRCDIR)/%), $(CC) $(SRC) $(CFLAGS) -msse2 -g0 -MT $(SRC:$(SRCDIR)/%.c=%.o) -MM >> .depend;)

config.mak:
	configure