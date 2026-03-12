obj-m += my_pci_driver.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all: module blk_ioctl

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

blk_ioctl:
	$(CC) -Wall -Wextra -O2 -o $@ blk_ioctl.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f blk_ioctl
