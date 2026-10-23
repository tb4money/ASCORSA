PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS := -O2 -Wall

ac_modloader.elf: main.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f ac_modloader.elf
