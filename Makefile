CFLAGS=-std=gnu99 -Wall -O2 -pthread

all : player.c master.c
	gcc $(CFLAGS) -o player player.c err.c
	gcc $(CFLAGS) -o master master.c err.c

player : player.c
	gcc $(CFLAGS) -o player player.c err.c

master : master.c
	gcc $(CFLAGS) -o master master.c err.c
