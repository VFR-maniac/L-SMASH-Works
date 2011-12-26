#----------------------------------------------------------------------------------------------
#  Makefile for AviUtl plugins
#----------------------------------------------------------------------------------------------

include config.mak

vpath %.c $(SRCDIR)
vpath %.h $(SRCDIR)

OBJ_INPUT = $(SRC_INPUT:%.c=%.o)
OBJ_MUXER = $(SRC_MUXER:%.c=%.o)

SRC_ALL = $(SRC_INPUT) $(SRC_MUXER)

all: input muxer

input: lsmashinput.aui

lsmashinput.aui: $(OBJ_INPUT) lsmashinput.def
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)
ifneq ($(STRIP),)
	$(STRIP) $@
endif

muxer: lsmashmuxer.auf

lsmashmuxer.auf: $(OBJ_MUXER) lsmashmuxer.def
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)
ifneq ($(STRIP),)
	$(STRIP) $@
endif

%.o: %.c .depend
	$(CC) $(CFLAGS) -c $<  -o $@

clean:
	-rm *.auf *.aui *.o .depend

distclean: clean
	-rm config.*

ifneq ($(wildcard .depend),)
include .depend
endif

.depend: config.mak
	@$(RM) .depend
	@$(foreach SRC, $(SRC_ALL:%=$(SRCDIR)/%), $(CC) $(SRC) $(CFLAGS) -msse2 -g0 -MT $(SRC:$(SRCDIR)/%.c=%.o) -MM >> .depend;)

config.mak:
	configure