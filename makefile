BIN=mfcmd
CC=gcc
CFLAGS=-Wall -Wextra -std=gnu99
CFLAGS_DEBUG=-O0 -g -fsanitize=address
CFLAGS_RELEASE=-O3
LDFLAGS=-lpthread

CFLAGS+=$(CFLAGS_RELEASE)

all:
	$(CC) $(CFLAGS) *.c $(LDFLAGS) -o $(BIN)
