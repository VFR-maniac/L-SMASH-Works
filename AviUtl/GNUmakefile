#----------------------------------------------------------------------------------------------
#  Makefile for Libav-SMASH File Reader using mingw + gcc.
#----------------------------------------------------------------------------------------------

include config.mak

vpath %.c $(SRCDIR)

OBJS = $(SRCS:.c=.o)

all: $(AUINAME)

$(AUINAME): $(OBJS)
	$(LD) $(LDFLAGS) $(AUI_LDFLAGS) -o $(AUINAME) $(OBJS) $(DEF) $(LIBS)
ifneq ($(STRIP),)
	$(STRIP) $(AUINAME)
endif

%.o: %.c .depend
	$(CC) $(CFLAGS) -c $<  -o $@

clean:
	-rm *.aui *.o .depend

distclean: clean
	-rm config.*

ifneq ($(wildcard .depend),)
include .depend
endif

.depend: config.mak
	@$(RM) .depend
	@$(foreach SRC, $(SRCS:%=$(SRCDIR)/%), $(CC) $(SRC) $(CFLAGS) -msse2 -g0 -MT $(SRC:$(SRCDIR)/%.c=%.o) -MM >> .depend;)

config.mak:
	configure