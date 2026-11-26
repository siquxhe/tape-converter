CC ?= gcc
CFLAGS ?= -O2 -I../../simulator/src_c

TARGET = tape2char

.PHONY: all
all: main.c tape.h
	$(CC) $(CFLAGS) main.c -o $(TARGET)

.PHONY: clean
clean:
	rm -f *.o $(TARGET)
