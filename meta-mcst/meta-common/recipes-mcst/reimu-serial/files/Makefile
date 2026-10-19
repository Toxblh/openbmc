BINARYNAME=reimu-serial

all: $(BINARYNAME)

$(BINARYNAME): $(BINARYNAME).c
	$(CC) $(CFLAGS) -Wall -o $(BINARYNAME) $(BINARYNAME).c $(LDFLAGS)

clean:
	rm -f *.o $(BINARYNAME)
