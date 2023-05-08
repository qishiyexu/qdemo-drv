NAME = qdemo-drv
ifneq ($(KERNELRELEASE),)
    obj-m := $(NAME).o
else
    KDIR := /usr/lib/modules/$(shell uname -r)/build/

all:
        make -C $(KDIR) M=$(PWD) modules
#       gcc -g -O0 ioctl.c -o ioctl

clean:
        rm -f *.ko *.o *.mod.o *.mod.c *.symvers
#       rm -rf ioctl
endif