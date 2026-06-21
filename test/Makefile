CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -I../module
TARGET  = hyperion-test

all: $(TARGET)

$(TARGET): hyperion-test.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
