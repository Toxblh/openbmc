all: reimu-pnsd

reimu-pnsd: reimu-pnsd.c reimu-sensors.c reimu-journal.c reimu-pnsflash.c reimu-overheat.c reimu-sensors.h reimu-pnsflash.h reimu-overheat.h
	$(CC) $(CFLAGS) -Wall -o reimu-pnsd reimu-pnsd.c reimu-sensors.c reimu-journal.c reimu-pnsflash.c reimu-overheat.c -lsystemd -lm $(LDFLAGS)

clean:
	rm -f reimu-pnsd
