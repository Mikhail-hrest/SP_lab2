obj-m += my_pci_driver.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all: module blk_ioctl emulator

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

blk_ioctl: blk_ioctl.c
	$(CC) -Wall -Wextra -O2 -o $@ $<

emulator: emulator.c
	$(CC) -Wall -Wextra -O2 -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f blk_ioctl emulator
