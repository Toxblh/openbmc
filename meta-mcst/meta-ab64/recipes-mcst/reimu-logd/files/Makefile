CFLAGS ?= -O2
reimu-logd: reimu-logd.c ev-core.c ev-core.h
	$(CC) $(CFLAGS) -Wall -Wextra -Wno-unused-parameter $(LDFLAGS) -o $@ reimu-logd.c ev-core.c -lsystemd
test: tests/test_ev.c ev-core.c ev-core.h
	$(CC) -Wall -Wextra -O1 -g -fsanitize=address,undefined -I. tests/test_ev.c ev-core.c -o /tmp/test_ev && /tmp/test_ev
clean:
	rm -f reimu-logd
