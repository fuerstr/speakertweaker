CC := gcc
CFLAGS += -I. -Wall -funroll-loops -fPIC -DPIC -O2
LD := gcc
LDFLAGS += -Wall -shared

SND_PCM_OBJECTS = pcm_speakertweaker.o
SND_PCM_LIBS = -lasound
SND_PCM_BIN = libasound_module_pcm_speakertweaker.so

LIBDIR = lib/arm-linux-gnueabihf

.PHONY: all clean install

all: Makefile $(SND_PCM_BIN) 

$(SND_PCM_BIN): $(SND_PCM_OBJECTS)
	@echo LD $@
	@$(LD) $(LDFLAGS) $(SND_PCM_OBJECTS) $(SND_PCM_LIBS) -o $(SND_PCM_BIN)

%.o: %.c
	@echo GCC $<
	@$(CC) -c $(CFLAGS) $(CPPFLAGS) $<

clean:
	@echo Cleaning...
	@rm -vf *.o *.so

install: all
	@echo Installing...
	@mkdir -p ${DESTDIR}/usr/$(LIBDIR)/alsa-lib/
	@install -m 644 $(SND_PCM_BIN) ${DESTDIR}/usr/$(LIBDIR)/alsa-lib/
