CFLAGS ?= -O2
all: reimu-flash reimu-fit reimu-abd
reimu-flash: reimu-flash.c
	$(CC) $(CFLAGS) -Wall -Wextra $(LDFLAGS) -o $@ $<
reimu-fit: reimu-fit.c
	$(CC) $(CFLAGS) -Wall $(LDFLAGS) -o $@ $<
reimu-abd: reimu-abd.c
	$(CC) $(CFLAGS) -Wall -Wextra $(LDFLAGS) -o $@ $< -lsystemd
clean:
	rm -f reimu-flash reimu-fit reimu-abd
