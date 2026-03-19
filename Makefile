CC      = gcc
CFLAGS  = -O2 -static -w
LDFLAGS = -lm

all: triumph init

triumph: triumph.c editor.c
	$(CC) $(CFLAGS) -o triumph triumph.c $(LDFLAGS)

init: init.c
	$(CC) $(CFLAGS) -o init init.c

clean:
	rm -f triumph init
