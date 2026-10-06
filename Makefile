# Command-line tools (portable libusb C) and the macOS camera.
#
#   make            build tools/
#   make macos      build the macOS camera extension, feeder and menu app (macos/build.sh)
#   make research   build the exploratory probe tools in research/tools/
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -Wall
USB_INC := $(shell pkg-config --variable=includedir libusb-1.0)
USB_LIB := $(shell pkg-config --libs libusb-1.0)
CFLAGS  += -I$(USB_INC)

TOOLS    := $(addprefix tools/,gl310i2c gl310init gl310start gl310log gl310recover)
RESEARCH := $(addprefix research/tools/,gl310probe gl310armtest gl310aperture gl310ddr \
                                        gl310life gl310mbox desc)

all: $(TOOLS)

tools/% research/tools/%: | check-libusb
tools/%: tools/%.c
	$(CC) $(CFLAGS) -o $@ $< $(USB_LIB)
research/tools/%: research/tools/%.c
	$(CC) $(CFLAGS) -o $@ $< $(USB_LIB)

research: $(RESEARCH)

macos:
	macos/build.sh

check-libusb:
	@test -n "$(USB_INC)" || { echo "libusb-1.0 not found (macOS: brew install libusb pkg-config;" \
	  "Debian/Ubuntu: apt install libusb-1.0-0-dev pkg-config)"; exit 1; }

clean:
	rm -f $(TOOLS) $(RESEARCH)
	rm -rf macos/build

.PHONY: all research macos clean check-libusb
