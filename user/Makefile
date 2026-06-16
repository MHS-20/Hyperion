CC      = gcc
CFLAGS  = -Wall -I../module
TARGET  = hyperion-user

all: $(TARGET)

$(TARGET): hyperion-user.c
	$(CC) $(CFLAGS) -o $(TARGET) hyperion-user.c

clean:
	rm -f $(TARGET)
