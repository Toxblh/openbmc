all: reimu-pnsd

reimu-pnsd: reimu-pnsd.c reimu-sensors.c reimu-journal.c reimu-pnsflash.c reimu-overheat.c reimu-view.c reimu-sensors.h reimu-pnsflash.h reimu-overheat.h reimu-view.h
	$(CC) $(CFLAGS) -Wall -o reimu-pnsd reimu-pnsd.c reimu-sensors.c reimu-journal.c reimu-pnsflash.c reimu-overheat.c reimu-view.c -lsystemd -lm $(LDFLAGS)

clean:
	rm -f reimu-pnsd
