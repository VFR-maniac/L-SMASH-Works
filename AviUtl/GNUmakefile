#----------------------------------------------------------------------------------------------
#  Makefile for AviUtl plugins
#----------------------------------------------------------------------------------------------

include config.mak

vpath %.c $(SRCDIR)
vpath %.h $(SRCDIR)
vpath %.rc $(SRCDIR)

OBJ_INPUT = $(SRC_INPUT:%.c=%.o) lsmashinput.res
OBJ_MUXER = $(SRC_MUXER:%.c=%.o) lwmuxer.res
OBJ_DUMPER = $(SRC_DUMPER:%.c=%.o)

SRC_ALL = $(SRC_INPUT) $(SRC_MUXER) $(SRC_DUMPER)

RCFLAGS = -J rc -O coff

ifneq ($(STRIP),)
LDFLAGS += -Wl,-s
endif

.PHONY: all input muxer dumper clean distclean dep

all: input muxer dumper

input: lsmashinput.aui

muxer: lwmuxer.auf

dumper: lwdumper.auf

lsmashinput.aui: $(OBJ_INPUT)
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

lwmuxer.auf: $(OBJ_MUXER)
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

lwdumper.auf: $(OBJ_DUMPER)
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c .depend
	$(CC) $(CFLAGS) -c $< -o $@

%.res: %.rc resource.h
	$(RC) $(RCFLAGS) $< -o $@

clean:
	$(RM) *.auf *.aui *.o *.res .depend

distclean: clean
	$(RM) config.*

dep: .depend

ifneq ($(wildcard .depend),)
include .depend
endif

.depend: config.mak
	@$(RM) .depend
	@$(foreach SRC, $(SRC_ALL:%=$(SRCDIR)/%), $(CC) $(SRC) $(CFLAGS) -msse4.1 -g0 -MT $(SRC:$(SRCDIR)/%.c=%.o) -MM >> .depend;)

config.mak:
	configure