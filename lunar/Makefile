CC = gcc
CFLAGS = -Wall -Wextra -O2 -D_GNU_SOURCE
TARGET = okpm
OBJS = util.o store.o pkg.o cmd.o globals.o

PREFIX ?= /usr/local

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c okpm.h
	$(CC) $(CFLAGS) -c -o $@ $<

static: $(OBJS)
	$(CC) $(CFLAGS) -static -o $(TARGET) $(OBJS)

install: $(TARGET)
	cp $(TARGET) $(PREFIX)/bin/

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)

test: $(TARGET)
	./$(TARGET) help
	./$(TARGET) stats

.PHONY: install uninstall clean static test