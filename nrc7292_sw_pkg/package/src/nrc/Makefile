# NRC7292 Kernel Driver Makefile

obj-m := nrc.o

nrc-objs := nrc-mac80211.o \
            nrc-hif-cspi.o \
            nrc-trx.o \
            nrc-fw.o \
            nrc-netlink.o \
            nrc-init.o \
            wim.o \
            nrc-s1g.o

KVERSION := $(shell uname -r)
KDIR := /lib/modules/$(KVERSION)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

checkstyle:
	perl ./checkpatch.pl --file --terse --ignore=LINUX_VERSION_CODE --no-tree *.c *.h

checkstyle2:
	perl ./checkpatch.pl --file --strict --terse --ignore=LINUX_VERSION_CODE --no-tree *.c *.h

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

.PHONY: all clean checkstyle checkstyle2 install