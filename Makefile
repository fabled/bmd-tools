LIBUSB_CFLAGS += $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LDFLAGS += $(shell pkg-config --libs libusb-1.0)
CFLAGS = -g -O3 #-Wall

TOOLS = bmd-streamer bmd-extractfw

all: $(TOOLS)

bmd-streamer: CFLAGS+=$(LIBUSB_CFLAGS)
bmd-streamer: LDFLAGS+=$(LIBUSB_LDFLAGS) -lpthread -lm

%: %.c
	gcc $(CFLAGS) $< -o $@  $(LDFLAGS)

clean:
	rm -f $(TOOLS)
