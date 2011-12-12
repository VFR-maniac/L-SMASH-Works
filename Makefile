#----------------------------------------------------------------------------------------------
#  Makefile for Libav-SMASH File Reader using mingw + gcc.
#----------------------------------------------------------------------------------------------

include config.mak

OBJS = $(SRCS:.c=.o)

all: $(AUINAME)

$(AUINAME): $(OBJS)
	$(LD) $(LDFLAGS) $(AUI_LDFLAGS) -o $(AUINAME) $(OBJS) $(DEF) $(LIBS)
	@if [ -n "$(STRIP)" ]; then \
		echo $(STRIP) $(AUINAME); \
		$(STRIP) $(AUINAME); \
	fi
    
.c.o:
	$(CC) $(CFLAGS) -c $<  -o $@

clean:
	rm *.aui *.o
    
distclean: clean
	rm config.*