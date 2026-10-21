all: reimu-pnsd

reimu-pnsd: reimu-pnsd.c reimu-sensors.c reimu-journal.c reimu-sensors.h
	$(CC) $(CFLAGS) -Wall -o reimu-pnsd reimu-pnsd.c reimu-sensors.c reimu-journal.c -lsystemd -lm $(LDFLAGS)

clean:
	rm -f reimu-pnsd
