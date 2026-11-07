CFLAGS ?= -O2
# rbuf.c: meta-common/recipes-mcst/reimu-common/files in the tree (bitbake puts it here)
COMMON ?= ../../../../meta-common/recipes-mcst/reimu-common/files
VPATH = $(COMMON)
reimu-logd: reimu-logd.c ev-core.c ev-core.h reimu-journal.c reimu-journal.h rbuf.c
	$(CC) $(CFLAGS) -I$(COMMON) -Wall -Wextra -Wno-unused-parameter $(LDFLAGS) -o $@ $(filter %.c,$^) -lsystemd
test: tests/test_ev.c ev-core.c ev-core.h
	$(CC) -Wall -Wextra -O1 -g -fsanitize=address,undefined -I. tests/test_ev.c ev-core.c -o /tmp/test_ev && /tmp/test_ev
clean:
	rm -f reimu-logd
