all: reimu-hostlink

SRC = reimu-hostlink.c hl-proto.c hl-json.c hl-net.c hl-dbus.c

reimu-hostlink: $(SRC) hostlink.h
	$(CC) $(CFLAGS) -Wall -DHL_DBUS -o reimu-hostlink $(SRC) -lsystemd -lssl -lcrypto -lm $(LDFLAGS)

# native, on the build host: the unit tests, and the daemon without D-Bus for
# the protocol tests (tests/test_protocol.py: TLS, the agent end to end).
# NATIVE_SSL: an OpenSSL prefix when the build host has no libssl-dev, e.g.
# the Yocto build's tmp/sysroots-components/x86_64/openssl-native/usr
HOSTCC ?= gcc
NATIVE_SSL ?=
NCFLAGS = -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -g -O1 -fsanitize=address,undefined -I. \
	$(if $(NATIVE_SSL),-I$(NATIVE_SSL)/include)
comma := ,
NLIBS = $(if $(NATIVE_SSL),-L$(NATIVE_SSL)/lib -Wl$(comma)-rpath$(comma)$(NATIVE_SSL)/lib) -lssl -lcrypto -lm

test: tests/test_hostlink tests/reimu-hostlink-native
	./tests/test_hostlink
	python3 tests/test_protocol.py ./tests/reimu-hostlink-native
	python3 tests/test_disk.py ./tests/reimu-hostlink-native

tests/test_hostlink: tests/test_hostlink.c hl-proto.c hl-json.c hostlink.h
	$(HOSTCC) $(NCFLAGS) -o $@ tests/test_hostlink.c hl-proto.c hl-json.c $(NLIBS)

tests/reimu-hostlink-native: reimu-hostlink.c hl-proto.c hl-json.c hl-net.c hostlink.h
	$(HOSTCC) $(NCFLAGS) -o $@ reimu-hostlink.c hl-proto.c hl-json.c hl-net.c $(NLIBS)

clean:
	rm -f reimu-hostlink tests/test_hostlink tests/reimu-hostlink-native
