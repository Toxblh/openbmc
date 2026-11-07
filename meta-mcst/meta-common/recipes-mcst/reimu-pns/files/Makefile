all: reimu-pnsd

# rbuf.c, dbusjson.c: ../reimu-common/files in the tree (bitbake puts them here)
COMMON ?= ../../reimu-common/files
VPATH = $(COMMON)

reimu-pnsd: reimu-pnsd.c reimu-pnsflash.c reimu-view.c rbuf.c dbusjson.c reimu-pnsflash.h reimu-view.h
	$(CC) $(CFLAGS) -I$(COMMON) -Wall -o reimu-pnsd $(filter %.c,$^) -lsystemd -lm $(LDFLAGS)

clean:
	rm -f reimu-pnsd
