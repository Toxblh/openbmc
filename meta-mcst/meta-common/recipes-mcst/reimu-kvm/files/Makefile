all: reimu-kvm

reimu-kvm: reimu-kvm.c
	$(CC) $(CFLAGS) -Wall -o reimu-kvm reimu-kvm.c -lssl -lcrypto -lsystemd $(LDFLAGS)

clean:
	rm -f reimu-kvm
