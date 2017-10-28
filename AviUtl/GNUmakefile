#----------------------------------------------------------------------------------------------
#  Makefile for AviUtl plugins
#----------------------------------------------------------------------------------------------

include config.mak

vpath %.c $(SRCDIR)
vpath %.h $(SRCDIR)
vpath %.rc $(SRCDIR)

OBJ_INPUT = $(SRC_INPUT:%.c=%.o) lwinput.res
OBJ_MUXER = $(SRC_MUXER:%.c=%.o) lwmuxer.res
OBJ_DUMPER = $(SRC_DUMPER:%.c=%.o)
OBJ_COLOR = $(SRC_COLOR:%.c=%.o)

SRC_ALL = $(SRC_INPUT) $(SRC_MUXER) $(SRC_DUMPER) $(SRC_COLOR)

RCFLAGS = -J rc -O coff

ifneq ($(STRIP),)
LDFLAGS += -Wl,-s
endif

.PHONY: all input muxer dumper color clean distclean dep

all: input muxer dumper color

input: lwinput.aui

muxer: lwmuxer.auf

dumper: lwdumper.auf

color: lwcolor.auc

lwinput.aui: $(OBJ_INPUT)
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

lwmuxer.auf: $(OBJ_MUXER)
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

lwdumper.auf: $(OBJ_DUMPER)
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

lwcolor.auc: $(OBJ_COLOR)
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c .depend
	$(CC) $(CFLAGS) -c $< -o $@

%.res: %.rc resource.h
	$(RC) $(RCFLAGS) $< -o $@

clean:
	$(RM) *.auf *.aui *.auc *.o *.res .depend

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
