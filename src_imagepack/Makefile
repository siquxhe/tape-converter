CC ?= gcc
CFLAGS ?= -O2 -I.

TARGET = image-pack

.PHONY: all
all: $(TARGET)

$(TARGET): main.c image_format.h
	$(CC) $(CFLAGS) main.c -o $(TARGET)

.PHONY: clean
clean:
	rm -f *.o $(TARGET) $(TARGET).exe
