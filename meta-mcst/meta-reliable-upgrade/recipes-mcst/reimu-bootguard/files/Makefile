all: reimu-wdt2 reimu-flashcopy reimu-firmwared

reimu-wdt2: reimu-wdt2.c
	$(CC) $(CFLAGS) -Wall -o reimu-wdt2 reimu-wdt2.c $(LDFLAGS)

reimu-flashcopy: reimu-flashcopy.c
	$(CC) $(CFLAGS) -Wall -o reimu-flashcopy reimu-flashcopy.c $(LDFLAGS)

reimu-firmwared: reimu-firmwared.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -Wall -o reimu-firmwared reimu-firmwared.c $(LDFLAGS) -lsystemd

clean:
	rm -f reimu-wdt2 reimu-flashcopy reimu-firmwared
