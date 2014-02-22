LIBUSB_FLAGS += $(shell pkg-config --cflags --libs libusb-1.0)
CFLAGS = -g -O3 #-Wall

all: bmd-streamer bmd-extractfw

bmd-streamer: CFLAGS+=$(LIBUSB_FLAGS) -lpthread -lm

%: %.c
	gcc $(CFLAGS) $< -o $@  $(LDFLAGS)
