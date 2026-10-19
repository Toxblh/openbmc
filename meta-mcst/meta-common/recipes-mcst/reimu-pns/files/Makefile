all: reimu-pnsd

reimu-pnsd: reimu-pnsd.c
	$(CC) $(CFLAGS) -Wall -o reimu-pnsd reimu-pnsd.c -lsystemd $(LDFLAGS)

clean:
	rm -f reimu-pnsd
