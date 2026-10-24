all: reimu-fand

SRC = reimu-fand.c fand.c fand-hw.c

reimu-fand: $(SRC) fand.h
	$(CC) $(CFLAGS) -Wall -o reimu-fand $(SRC) -lsystemd -lm $(LDFLAGS)

# native, on the build host: the control logic against fake chips
test: tests/test_fand
	./tests/test_fand

tests/test_fand: tests/test_fand.c fand.c fand-hw.c fand.h
	$(HOSTCC) -Wall -Wextra -Wno-unused-parameter -g -O1 -fsanitize=address,undefined -I. -o $@ tests/test_fand.c fand.c fand-hw.c -lm

HOSTCC ?= gcc

clean:
	rm -f reimu-fand tests/test_fand
