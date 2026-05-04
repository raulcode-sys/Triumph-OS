CC      = gcc
CFLAGS  = -O2 -static -w
LDFLAGS = -lm

all: triumph init

triumph: triumph.c editor.c snake.c tetris.c tools.c
	$(CC) $(CFLAGS) -o triumph triumph.c $(LDFLAGS)

init: init.c splash.c
	$(CC) $(CFLAGS) -o init init.c

clean:
	rm -f triumph init
