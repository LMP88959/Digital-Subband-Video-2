CC=gcc
CFLAGS=-O3 -march=native
LANCE_CFLAGS=-O3 --max-inline 256
SOURCES=$(wildcard src/*.c)

build:
	$(CC) $(SOURCES) -o ./dsv2 $(CFLAGS)

build_lance:
	lance $(SOURCES) -o ./dsv2 $(LANCE_CFLAGS)
