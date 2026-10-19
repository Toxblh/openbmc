all: reimu-pnsd

reimu-pnsd: reimu-pnsd.c reimu-sensors.c reimu-sensors.h
	$(CC) $(CFLAGS) -Wall -o reimu-pnsd reimu-pnsd.c reimu-sensors.c -lsystemd -lm $(LDFLAGS)

clean:
	rm -f reimu-pnsd
