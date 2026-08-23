CC = gcc
CFLAGS = -Wall -Wextra -O2 -D_GNU_SOURCE
TARGET = okpm
SRC = okpm.c

PREFIX ?= /usr/local

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

static: $(SRC)
	$(CC) $(CFLAGS) -static -o $(TARGET) $(SRC)

install: $(TARGET)
	cp $(TARGET) $(PREFIX)/bin/

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)

test: $(TARGET)
	./$(TARGET) help
	./$(TARGET) stats

.PHONY: install uninstall clean static test