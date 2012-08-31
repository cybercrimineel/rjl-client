TARGET := libretro.so
SOURCES := $(wildcard *.c)
OBJECTS := $(SOURCES:.c=.o)

LIBS := $(shell pkg-config libusb-1.0 --libs) -pthread -shared -Wl,--version-script=link.T -fPIC
CFLAGS += $(shell pkg-config libusb-1.0 --cflags) -O2 -Wall -std=gnu99 -g -pthread -fPIC

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: clean

