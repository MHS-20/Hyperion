CC      = gcc
CFLAGS  = -Wall -I../module
TARGET  = hyperion-user
TARGET2 = test-hyperion

all: $(TARGET) $(TARGET2)

$(TARGET): hyperion-user.c
	$(CC) $(CFLAGS) -o $(TARGET) hyperion-user.c

$(TARGET2): test-hyperion.c
	$(CC) $(CFLAGS) -o $(TARGET2) test-hyperion.c

clean:
	rm -f $(TARGET) $(TARGET2)
